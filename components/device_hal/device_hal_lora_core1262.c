#include "meshpay/device_hal.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sx126x.h"
#include "sx126x_hal.h"
#include "sx126x_hal_context.h"

#include <string.h>

#define C1262_SPI_CLOCK_HZ (8 * 1000 * 1000)
#define C1262_TCXO_TIMEOUT_RTC_STEPS 320
#define C1262_INIT_ATTEMPTS 2
#define C1262_RX_TASK_STACK_BYTES 4096
#define C1262_RX_TASK_PRIORITY 5
#define C1262_RX_QUEUE_ITEM_MAX MESHPAY_HAL_LORA_CORE1262_MAX_PAYLOAD
#define C1262_BUSY_LOCK_MS 5000U
#define C1262_RX_LOCK_MS 1000U
#define C1262_RX_IRQ_POLL_MS 250U
#define C1262_PIN_UNSET (-1)

static const char *TAG = "device_hal_c1262";

typedef struct {
    uint8_t data[C1262_RX_QUEUE_ITEM_MAX];
    size_t len;
} c1262_rx_frame_t;

typedef struct {
    sx126x_mod_params_lora_t mod;
    sx126x_pkt_params_lora_t pkt;
    uint32_t freq_hz;
    sx126x_tcxo_ctrl_voltages_t tcxo_voltage;
    bool calibrate_image;
    int8_t power_dbm;
} c1262_radio_params_t;

struct meshpay_hal_lora_core1262_internal {
    core1262_hw_t hw;
    spi_host_device_t spi_host;
    int pin_sck;
    int pin_mosi;
    int pin_miso;
    int pin_dio1;
    int pin_rxen;
    int pin_txen;
    int pin_aux_cs;
    bool dio2_rf_switch;
    c1262_radio_params_t params;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t radio_mutex;
    SemaphoreHandle_t dio1_sem;
    SemaphoreHandle_t rx_stop_sem;
    TaskHandle_t rx_task_handle;
    volatile bool rx_running;
    bool spi_bus_ready;
    bool spi_device_ready;
    bool isr_ready;
};

static bool pin_valid(int pin)
{
    return pin >= 0 && pin < 64;
}

static bool pin_valid_or_unset(int pin)
{
    return pin == C1262_PIN_UNSET || pin_valid(pin);
}

static bool uses_gpio_rf_switch(
    const meshpay_hal_lora_core1262_config_t *config)
{
    return config->pin_rxen != C1262_PIN_UNSET &&
           config->pin_txen != C1262_PIN_UNSET;
}

static bool pins_are_distinct(const meshpay_hal_lora_core1262_config_t *config)
{
    const int pins[] = {
        config->pin_sck,   config->pin_mosi, config->pin_miso,
        config->pin_nss,   config->pin_reset, config->pin_busy,
        config->pin_dio1,  config->pin_rxen, config->pin_txen,
        config->pin_aux_cs,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] == C1262_PIN_UNSET) {
            continue;
        }
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (pins[j] == C1262_PIN_UNSET) {
                continue;
            }
            if (pins[i] == pins[j]) {
                return false;
            }
        }
    }
    return true;
}

void meshpay_hal_lora_core1262_default_config(
    meshpay_hal_lora_core1262_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->spi_host = MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPI_HOST;
    config->pin_sck = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_SCK;
    config->pin_mosi = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MOSI;
    config->pin_miso = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MISO;
    config->pin_nss = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_NSS;
    config->pin_reset = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_RESET;
    config->pin_busy = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_BUSY;
    config->pin_dio1 = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_DIO1;
    config->pin_rxen = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_RXEN;
    config->pin_txen = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_TXEN;
    config->pin_aux_cs = MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_AUX_CS;
    config->frequency_hz = MESHPAY_HAL_LORA_CORE1262_DEFAULT_FREQUENCY_HZ;
    config->spreading_factor =
        MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPREADING_FACTOR;
    config->tcxo_ctrl_voltage =
        MESHPAY_HAL_LORA_CORE1262_DEFAULT_TCXO_CTRL_VOLTAGE;
    config->calibrate_image =
        MESHPAY_HAL_LORA_CORE1262_DEFAULT_CALIBRATE_IMAGE;
    config->bandwidth = MESHPAY_HAL_LORA_CORE1262_DEFAULT_BANDWIDTH;
    config->coding_rate = MESHPAY_HAL_LORA_CORE1262_DEFAULT_CODING_RATE;
    config->tx_power_dbm = MESHPAY_HAL_LORA_CORE1262_DEFAULT_TX_POWER_DBM;
    config->queue_length = MESHPAY_HAL_LORA_CORE1262_DEFAULT_QUEUE_LENGTH;
    config->read_timeout_ms =
        MESHPAY_HAL_LORA_CORE1262_DEFAULT_READ_TIMEOUT_MS;
    config->tx_timeout_ms = MESHPAY_HAL_LORA_CORE1262_DEFAULT_TX_TIMEOUT_MS;
}

esp_err_t meshpay_hal_lora_core1262_validate_config(
    const meshpay_hal_lora_core1262_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->spi_host < 0 || config->spi_host > SPI3_HOST ||
        !pin_valid(config->pin_sck) ||
        !pin_valid(config->pin_mosi) ||
        !pin_valid(config->pin_miso) ||
        !pin_valid(config->pin_nss) ||
        !pin_valid(config->pin_reset) ||
        !pin_valid(config->pin_busy) ||
        !pin_valid(config->pin_dio1) ||
        !pin_valid_or_unset(config->pin_rxen) ||
        !pin_valid_or_unset(config->pin_txen) ||
        !pin_valid_or_unset(config->pin_aux_cs) ||
        !pins_are_distinct(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((config->pin_rxen == C1262_PIN_UNSET) !=
        (config->pin_txen == C1262_PIN_UNSET)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->frequency_hz < 300000000UL ||
        config->frequency_hz > 960000000UL ||
        config->spreading_factor < 7 ||
        config->spreading_factor > 12 ||
        config->tcxo_ctrl_voltage > SX126X_TCXO_CTRL_3_3V ||
        config->bandwidth > MESHPAY_HAL_LORA_CORE1262_BW_500 ||
        config->coding_rate < 1 ||
        config->coding_rate > 4 ||
        config->queue_length == 0 ||
        config->read_timeout_ms == 0 ||
        config->tx_timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t map_radio_params(
    const meshpay_hal_lora_core1262_config_t *config,
    c1262_radio_params_t *out)
{
    if (config == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(meshpay_hal_lora_core1262_validate_config(config),
                        TAG, "");

    memset(out, 0, sizeof(*out));
    out->mod.sf = (sx126x_lora_sf_t)config->spreading_factor;
    switch (config->bandwidth) {
    case MESHPAY_HAL_LORA_CORE1262_BW_125:
        out->mod.bw = SX126X_LORA_BW_125;
        break;
    case MESHPAY_HAL_LORA_CORE1262_BW_250:
        out->mod.bw = SX126X_LORA_BW_250;
        break;
    case MESHPAY_HAL_LORA_CORE1262_BW_500:
        out->mod.bw = SX126X_LORA_BW_500;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    out->mod.cr = (sx126x_lora_cr_t)config->coding_rate;
    out->mod.ldro =
        ((config->spreading_factor >= 11 &&
          config->bandwidth == MESHPAY_HAL_LORA_CORE1262_BW_125) ||
         (config->spreading_factor == 12 &&
          config->bandwidth == MESHPAY_HAL_LORA_CORE1262_BW_250))
            ? 1
            : 0;

    out->pkt.preamble_len_in_symb = 8;
    out->pkt.header_type = SX126X_LORA_PKT_EXPLICIT;
    out->pkt.pld_len_in_bytes = 0;
    out->pkt.crc_is_on = true;
    out->pkt.invert_iq_is_on = false;
    out->freq_hz = config->frequency_hz;
    out->tcxo_voltage =
        (sx126x_tcxo_ctrl_voltages_t)config->tcxo_ctrl_voltage;
    out->calibrate_image = config->calibrate_image;

    int8_t power = config->tx_power_dbm;
    if (power < -9) {
        power = -9;
    } else if (power > 22) {
        power = 22;
    }
    out->power_dbm = power;
    return ESP_OK;
}

static esp_err_t sx_to_esp(sx126x_status_t status)
{
    return status == SX126X_STATUS_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t set_lora_rx_packet_params(
    meshpay_hal_lora_core1262_internal_t *ctx)
{
    sx126x_pkt_params_lora_t pkt = ctx->params.pkt;
    pkt.pld_len_in_bytes = MESHPAY_HAL_LORA_CORE1262_MAX_PAYLOAD;
    return sx_to_esp(sx126x_set_lora_pkt_params(&ctx->hw, &pkt));
}

static esp_err_t check_chip_status(
    meshpay_hal_lora_core1262_internal_t *ctx,
    const char *step)
{
    sx126x_chip_status_t status = {0};
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_get_status(&ctx->hw, &status)),
                        TAG,
                        "");
    ESP_LOGI(TAG,
             "Core1262 status after %s cmd=%u mode=%u",
             step,
             (unsigned)status.cmd_status,
             (unsigned)status.chip_mode);
    if (status.chip_mode > SX126X_CHIP_MODE_TX ||
        status.chip_mode == SX126X_CHIP_MODE_UNUSED ||
        status.chip_mode == SX126X_CHIP_MODE_RFU) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void rf_switch_rx(meshpay_hal_lora_core1262_internal_t *ctx)
{
    if (ctx->dio2_rf_switch) {
        return;
    }
    esp_err_t tx_err = gpio_set_level((gpio_num_t)ctx->pin_txen, 0);
    esp_err_t rx_err = gpio_set_level((gpio_num_t)ctx->pin_rxen, 1);
    if (tx_err != ESP_OK || rx_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Core1262 RF switch RX set failed rx=%s tx=%s",
                 esp_err_to_name(rx_err),
                 esp_err_to_name(tx_err));
    }
}

static void rf_switch_tx(meshpay_hal_lora_core1262_internal_t *ctx)
{
    if (ctx->dio2_rf_switch) {
        return;
    }
    esp_err_t rx_err = gpio_set_level((gpio_num_t)ctx->pin_rxen, 0);
    esp_err_t tx_err = gpio_set_level((gpio_num_t)ctx->pin_txen, 1);
    if (rx_err != ESP_OK || tx_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Core1262 RF switch TX set failed rx=%s tx=%s",
                 esp_err_to_name(rx_err),
                 esp_err_to_name(tx_err));
    }
}

static void rf_switch_probe(meshpay_hal_lora_core1262_internal_t *ctx)
{
    if (ctx->dio2_rf_switch) {
        return;
    }

    (void)gpio_set_level((gpio_num_t)ctx->pin_rxen, 0);
    (void)gpio_set_level((gpio_num_t)ctx->pin_txen, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    const int off_rx = gpio_get_level((gpio_num_t)ctx->pin_rxen);
    const int off_tx = gpio_get_level((gpio_num_t)ctx->pin_txen);

    (void)gpio_set_level((gpio_num_t)ctx->pin_rxen, 1);
    (void)gpio_set_level((gpio_num_t)ctx->pin_txen, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    const int rx_rx = gpio_get_level((gpio_num_t)ctx->pin_rxen);
    const int rx_tx = gpio_get_level((gpio_num_t)ctx->pin_txen);

    (void)gpio_set_level((gpio_num_t)ctx->pin_rxen, 0);
    (void)gpio_set_level((gpio_num_t)ctx->pin_txen, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    const int tx_rx = gpio_get_level((gpio_num_t)ctx->pin_rxen);
    const int tx_tx = gpio_get_level((gpio_num_t)ctx->pin_txen);

    ESP_LOGI(TAG,
             "Core1262 RF switch probe pins rxen=%d txen=%d off=%d/%d rx=%d/%d tx=%d/%d",
             ctx->pin_rxen,
             ctx->pin_txen,
             off_rx,
             off_tx,
             rx_rx,
             rx_tx,
             tx_rx,
             tx_tx);
    rf_switch_rx(ctx);
}

static void IRAM_ATTR dio1_isr_handler(void *arg)
{
    meshpay_hal_lora_core1262_internal_t *ctx =
        (meshpay_hal_lora_core1262_internal_t *)arg;
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->dio1_sem, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t apply_radio_config(
    meshpay_hal_lora_core1262_internal_t *ctx)
{
    const void *sx = &ctx->hw;
    sx126x_cal_mask_t cal_mask = SX126X_CAL_ALL;
    if (!ctx->params.calibrate_image) {
        cal_mask = (sx126x_cal_mask_t)(cal_mask & ~SX126X_CAL_IMAGE);
    }

    ESP_LOGI(TAG,
             "Core1262 init: standby tcxo=%u cal_mask=0x%02x rf_switch=%s",
             (unsigned)ctx->params.tcxo_voltage,
             (unsigned)cal_mask,
             ctx->dio2_rf_switch ? "dio2" : "gpio");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_standby(
                            sx,
                            SX126X_STANDBY_CFG_RC)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "standby"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_dio3_as_tcxo_ctrl(
                            sx,
                            ctx->params.tcxo_voltage,
                            C1262_TCXO_TIMEOUT_RTC_STEPS)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "tcxo"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_cal(sx, cal_mask)), TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "cal"), TAG, "");
    ESP_RETURN_ON_ERROR(
        sx_to_esp(sx126x_set_dio2_as_rf_sw_ctrl(sx,
                                                 ctx->dio2_rf_switch)),
        TAG,
        "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "dio2_rf_switch"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_pkt_type(
                            sx,
                            SX126X_PKT_TYPE_LORA)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "pkt_type"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_rf_freq(sx, ctx->params.freq_hz)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "rf_freq"), TAG, "");

    const sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x02,
        .hp_max = 0x02,
        .device_sel = 0x00,
        .pa_lut = 0x01,
    };
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_pa_cfg(sx, &pa_cfg)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "pa_cfg"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_tx_params(
                            sx,
                            ctx->params.power_dbm,
                            SX126X_RAMP_200_US)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "tx_params"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_lora_mod_params(
                            sx,
                            &ctx->params.mod)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "lora_mod"), TAG, "");
    ESP_RETURN_ON_ERROR(set_lora_rx_packet_params(ctx), TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "lora_pkt_rx"), TAG, "");
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_buffer_base_address(
                            sx,
                            0x00,
                            0x00)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "buffer_base"), TAG, "");

    const uint16_t irq_mask = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
                              SX126X_IRQ_TIMEOUT | SX126X_IRQ_CRC_ERROR;
    const uint16_t dio1_mask = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
                               SX126X_IRQ_TIMEOUT;
    ESP_RETURN_ON_ERROR(sx_to_esp(sx126x_set_dio_irq_params(
                            sx,
                            irq_mask,
                            dio1_mask,
                            0x0000,
                            0x0000)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(check_chip_status(ctx, "dio_irq"), TAG, "");
    return ESP_OK;
}

static esp_err_t recover_radio_locked(
    meshpay_hal_lora_core1262_internal_t *ctx)
{
    rf_switch_rx(ctx);
    if (sx126x_hal_reset(&ctx->hw) != SX126X_HAL_STATUS_OK) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(apply_radio_config(ctx), TAG, "");
    rf_switch_rx(ctx);
    if (ctx->rx_running) {
        return sx_to_esp(sx126x_set_rx_with_timeout_in_rtc_step(
            &ctx->hw,
            SX126X_RX_CONTINUOUS));
    }
    return ESP_OK;
}

static void c1262_rx_task(void *param)
{
    meshpay_hal_lora_core1262_internal_t *ctx =
        (meshpay_hal_lora_core1262_internal_t *)param;
    uint8_t packet[C1262_RX_QUEUE_ITEM_MAX];

    while (ctx->rx_running) {
        const bool dio1_event =
            xSemaphoreTake(ctx->dio1_sem,
                           pdMS_TO_TICKS(C1262_RX_IRQ_POLL_MS)) == pdTRUE;
        if (xSemaphoreTake(ctx->radio_mutex,
                           pdMS_TO_TICKS(C1262_RX_LOCK_MS)) != pdTRUE) {
            continue;
        }

        sx126x_irq_mask_t irq = 0;
        (void)sx126x_get_irq_status(&ctx->hw, &irq);
        if (irq == 0) {
            xSemaphoreGive(ctx->radio_mutex);
            continue;
        }
        (void)sx126x_clear_irq_status(&ctx->hw, irq);

        if ((irq & SX126X_IRQ_RX_DONE) != 0 &&
            (irq & SX126X_IRQ_CRC_ERROR) == 0) {
            sx126x_rx_buffer_status_t status = {0};
            if (sx126x_get_rx_buffer_status(&ctx->hw, &status) ==
                    SX126X_STATUS_OK &&
                status.pld_len_in_bytes > 0 &&
                sx126x_read_buffer(&ctx->hw,
                                   status.buffer_start_pointer,
                                   packet,
                                   status.pld_len_in_bytes) ==
                    SX126X_STATUS_OK) {
                c1262_rx_frame_t frame = {
                    .len = status.pld_len_in_bytes,
                };
                memcpy(frame.data, packet, frame.len);
                ESP_LOGI(TAG,
                         "Core1262 RX done len=%u irq=0x%04x dio1_event=%d",
                         (unsigned)frame.len,
                         (unsigned)irq,
                         dio1_event ? 1 : 0);
                if (xQueueSend(ctx->rx_queue, &frame, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "LoRa RX queue full, frame dropped");
                }
            }
        } else if ((irq & SX126X_IRQ_CRC_ERROR) != 0) {
            ESP_LOGW(TAG,
                     "Core1262 RX CRC error irq=0x%04x dio1_event=%d",
                     (unsigned)irq,
                     dio1_event ? 1 : 0);
        } else if ((irq & SX126X_IRQ_TIMEOUT) != 0) {
            ESP_LOGD(TAG,
                     "Core1262 RX timeout irq=0x%04x dio1_event=%d",
                     (unsigned)irq,
                     dio1_event ? 1 : 0);
        } else {
            ESP_LOGD(TAG,
                     "Core1262 RX irq=0x%04x dio1_event=%d",
                     (unsigned)irq,
                     dio1_event ? 1 : 0);
        }

        if (ctx->rx_running) {
            rf_switch_rx(ctx);
            (void)sx126x_set_rx_with_timeout_in_rtc_step(
                &ctx->hw,
                SX126X_RX_CONTINUOUS);
        }
        xSemaphoreGive(ctx->radio_mutex);
    }

    if (ctx->rx_stop_sem != NULL) {
        xSemaphoreGive(ctx->rx_stop_sem);
    }
    vTaskDelete(NULL);
}

static esp_err_t c1262_send(void *ctx_ptr, const uint8_t *data, size_t len)
{
    meshpay_hal_lora_core1262_driver_t *driver =
        (meshpay_hal_lora_core1262_driver_t *)ctx_ptr;
    if (driver == NULL || !driver->initialized ||
        driver->internal == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > MESHPAY_HAL_LORA_CORE1262_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    meshpay_hal_lora_core1262_internal_t *ctx = driver->internal;
    if (xSemaphoreTake(ctx->radio_mutex,
                       pdMS_TO_TICKS(C1262_BUSY_LOCK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const void *sx = &ctx->hw;
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(sx_to_esp(sx126x_set_standby(
                          sx,
                          SX126X_STANDBY_CFG_RC)),
                      done, TAG, "");
    ESP_GOTO_ON_ERROR(sx_to_esp(sx126x_write_buffer(
                          sx,
                          0x00,
                          data,
                          (uint8_t)len)),
                      done, TAG, "");

    ctx->params.pkt.pld_len_in_bytes = (uint8_t)len;
    ESP_GOTO_ON_ERROR(sx_to_esp(sx126x_set_lora_pkt_params(
                          sx,
                          &ctx->params.pkt)),
                      done, TAG, "");

    (void)sx126x_clear_irq_status(sx,
                                  SX126X_IRQ_TX_DONE |
                                      SX126X_IRQ_TIMEOUT);
    while (xSemaphoreTake(ctx->dio1_sem, 0) == pdTRUE) {
    }
    rf_switch_tx(ctx);
    int rxen_at_tx = ctx->dio2_rf_switch ? -1 : gpio_get_level(ctx->pin_rxen);
    int txen_at_tx = ctx->dio2_rf_switch ? -1 : gpio_get_level(ctx->pin_txen);
    const uint32_t toa_ms = sx126x_get_lora_time_on_air_in_ms(
        &ctx->params.pkt,
        &ctx->params.mod);
    ESP_LOGI(TAG,
             "Core1262 TX start len=%u timeout_ms=%lu toa_ms=%lu tx_start=%d/%d",
             (unsigned)len,
             (unsigned long)driver->config.tx_timeout_ms,
             (unsigned long)toa_ms,
             rxen_at_tx,
             txen_at_tx);
    ESP_GOTO_ON_ERROR(sx_to_esp(sx126x_set_tx(sx, 0)),
                      done, TAG, "");

    bool tx_done = false;
    bool dio1_seen = false;
    bool timeout_seen = false;
    uint32_t waited_ms = 0;
    sx126x_irq_mask_t last_irq = 0;
    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us =
        (int64_t)driver->config.tx_timeout_ms * 1000LL;
    TickType_t poll_delay = pdMS_TO_TICKS(10);
    if (poll_delay == 0) {
        poll_delay = 1;
    }
    while ((esp_timer_get_time() - start_us) < timeout_us) {
        sx126x_irq_mask_t irq = 0;
        ESP_GOTO_ON_ERROR(sx_to_esp(sx126x_get_irq_status(sx, &irq)),
                          done, TAG, "");
        last_irq = irq;
        waited_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000LL);
        if ((irq & SX126X_IRQ_TX_DONE) != 0) {
            tx_done = true;
            break;
        }
        if (gpio_get_level((gpio_num_t)ctx->pin_dio1) != 0) {
            dio1_seen = true;
            tx_done = true;
            break;
        }
        if ((irq & SX126X_IRQ_TIMEOUT) != 0) {
            timeout_seen = true;
            break;
        }
        vTaskDelay(poll_delay);
    }
    (void)sx126x_clear_irq_status(sx,
                                  SX126X_IRQ_TX_DONE |
                                      SX126X_IRQ_TIMEOUT);
    if (!tx_done) {
        sx126x_chip_status_t status = {0};
        (void)sx126x_get_status(sx, &status);
        ESP_LOGW(TAG,
                 "Core1262 TX timeout len=%u waited_ms=%lu irq=0x%04x timeout_irq=%d "
                 "cmd=%u mode=%u busy=%d dio1=%d rxen=%d txen=%d tx_start=%d/%d",
                 (unsigned)len,
                 (unsigned long)waited_ms,
                 (unsigned)last_irq,
                 timeout_seen ? 1 : 0,
                 (unsigned)status.cmd_status,
                 (unsigned)status.chip_mode,
                 gpio_get_level(ctx->hw.pin_busy),
                 gpio_get_level(ctx->pin_dio1),
                 ctx->dio2_rf_switch ? -1 : gpio_get_level(ctx->pin_rxen),
                 ctx->dio2_rf_switch ? -1 : gpio_get_level(ctx->pin_txen),
                 rxen_at_tx,
                 txen_at_tx);
        ret = ESP_ERR_TIMEOUT;
        goto done;
    }
    ESP_LOGI(TAG,
             "Core1262 TX done len=%u waited_ms=%lu irq=0x%04x dio1=%d",
             (unsigned)len,
             (unsigned long)waited_ms,
             (unsigned)last_irq,
             dio1_seen ? 1 : 0);

done:
    if (ret == ESP_OK) {
        rf_switch_rx(ctx);
        if (ctx->rx_running) {
            ret = set_lora_rx_packet_params(ctx);
            if (ret == ESP_OK) {
                ret = sx_to_esp(sx126x_set_rx_with_timeout_in_rtc_step(
                    sx,
                    SX126X_RX_CONTINUOUS));
            }
        }
    } else {
        (void)recover_radio_locked(ctx);
    }
    xSemaphoreGive(ctx->radio_mutex);
    return ret;
}

static esp_err_t c1262_recv(void *ctx_ptr,
                            uint8_t *data,
                            size_t size,
                            size_t *len)
{
    meshpay_hal_lora_core1262_driver_t *driver =
        (meshpay_hal_lora_core1262_driver_t *)ctx_ptr;
    if (driver == NULL || !driver->initialized ||
        driver->internal == NULL || data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    c1262_rx_frame_t frame;
    if (xQueueReceive(driver->internal->rx_queue,
                      &frame,
                      pdMS_TO_TICKS(driver->config.read_timeout_ms)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (size < frame.len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, frame.data, frame.len);
    *len = frame.len;
    return ESP_OK;
}

static const meshpay_hal_ops_t LORA_CORE1262_OPS = {
    .lora_send = c1262_send,
    .lora_recv = c1262_recv,
};

static esp_err_t configure_gpio(
    const meshpay_hal_lora_core1262_config_t *config)
{
    uint64_t output_pins = (1ULL << config->pin_nss) |
                           (1ULL << config->pin_reset);
    if (uses_gpio_rf_switch(config)) {
        output_pins |= (1ULL << config->pin_rxen) |
                       (1ULL << config->pin_txen);
    }
    if (config->pin_aux_cs != C1262_PIN_UNSET) {
        output_pins |= 1ULL << config->pin_aux_cs;
    }
    const gpio_config_t output_cfg = {
        .pin_bit_mask = output_pins,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "");
    (void)gpio_set_level((gpio_num_t)config->pin_nss, 1);
    (void)gpio_set_level((gpio_num_t)config->pin_reset, 1);
    if (config->pin_aux_cs != C1262_PIN_UNSET) {
        (void)gpio_set_level((gpio_num_t)config->pin_aux_cs, 1);
    }
    if (uses_gpio_rf_switch(config)) {
        (void)gpio_set_level((gpio_num_t)config->pin_rxen, 1);
        (void)gpio_set_level((gpio_num_t)config->pin_txen, 0);
    }

    const gpio_config_t busy_cfg = {
        .pin_bit_mask = 1ULL << config->pin_busy,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_cfg), TAG, "");

    const gpio_config_t dio1_cfg = {
        .pin_bit_mask = 1ULL << config->pin_dio1,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    return gpio_config(&dio1_cfg);
}

esp_err_t meshpay_hal_lora_core1262_driver_init(
    meshpay_hal_lora_core1262_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_lora_core1262_config_t *config)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_hal_lora_core1262_config_t effective;
    if (config == NULL) {
        meshpay_hal_lora_core1262_default_config(&effective);
    } else {
        effective = *config;
    }
    ESP_RETURN_ON_ERROR(meshpay_hal_lora_core1262_validate_config(&effective),
                        TAG, "");

    memset(driver, 0, sizeof(*driver));
    driver->config = effective;
    meshpay_hal_lora_core1262_internal_t *ctx =
        heap_caps_calloc(1,
                         sizeof(*ctx),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    driver->internal = ctx;
    ctx->spi_host = (spi_host_device_t)effective.spi_host;
    ctx->pin_sck = effective.pin_sck;
    ctx->pin_mosi = effective.pin_mosi;
    ctx->pin_miso = effective.pin_miso;
    ctx->pin_dio1 = effective.pin_dio1;
    ctx->pin_rxen = effective.pin_rxen;
    ctx->pin_txen = effective.pin_txen;
    ctx->pin_aux_cs = effective.pin_aux_cs;
    ctx->dio2_rf_switch = !uses_gpio_rf_switch(&effective);
    ctx->hw.pin_nss = effective.pin_nss;
    ctx->hw.pin_busy = effective.pin_busy;
    ctx->hw.pin_reset = effective.pin_reset;

    esp_err_t err = map_radio_params(&effective, &ctx->params);
    if (err == ESP_OK) {
        err = configure_gpio(&effective);
        if (err == ESP_OK) {
            rf_switch_probe(ctx);
        }
    }

    if (err == ESP_OK) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num = effective.pin_mosi,
            .miso_io_num = effective.pin_miso,
            .sclk_io_num = effective.pin_sck,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = MESHPAY_HAL_LORA_CORE1262_MAX_PAYLOAD + 16,
        };
        err = spi_bus_initialize(ctx->spi_host,
                                 &bus_cfg,
                                 SPI_DMA_DISABLED);
        if (err == ESP_OK) {
            /* On a initialisé le bus : on en est propriétaire, on le libèrera
             * au deinit. */
            ctx->spi_bus_ready = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* Bus déjà initialisé par un autre driver sur le MÊME host (l'écran
             * ST7789 du T-Deck, broches SPI partagées). On s'y greffe comme 2ᵉ
             * device et on hérite de SA config (DMA + max_transfer_sz). On NE
             * marque PAS spi_bus_ready : on n'est pas propriétaire, donc le
             * deinit ne libèrera pas le bus (sinon on couperait l'affichage). */
            err = ESP_OK;
        }
        ESP_LOGI(TAG, "SPI host=%d : bus %s", (int)ctx->spi_host,
                 ctx->spi_bus_ready ? "initialisé (propriétaire)"
                                    : "partagé (greffé sur l'écran)");
    }
    if (err == ESP_OK) {
        const spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = C1262_SPI_CLOCK_HZ,
            .mode = 0,
            .spics_io_num = -1,
            .queue_size = 1,
        };
        err = spi_bus_add_device(ctx->spi_host, &dev_cfg, &ctx->hw.spi);
        if (err == ESP_OK) {
            ctx->spi_device_ready = true;
        }
    }
    if (err == ESP_OK &&
        sx126x_hal_reset(&ctx->hw) != SX126X_HAL_STATUS_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = ESP_FAIL;
        for (int attempt = 1; attempt <= C1262_INIT_ATTEMPTS; ++attempt) {
            err = apply_radio_config(ctx);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG,
                     "Core1262 config attempt %d/%d failed (%s), resetting",
                     attempt,
                     C1262_INIT_ATTEMPTS,
                     esp_err_to_name(err));
            if (sx126x_hal_reset(&ctx->hw) != SX126X_HAL_STATUS_OK) {
                err = ESP_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (err == ESP_OK) {
        ctx->rx_queue = xQueueCreate(effective.queue_length,
                                     sizeof(c1262_rx_frame_t));
        ctx->radio_mutex = xSemaphoreCreateMutex();
        ctx->dio1_sem = xSemaphoreCreateBinary();
        ctx->rx_stop_sem = xSemaphoreCreateBinary();
        if (ctx->rx_queue == NULL || ctx->radio_mutex == NULL ||
            ctx->dio1_sem == NULL || ctx->rx_stop_sem == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
            err = isr_err;
        }
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add((gpio_num_t)effective.pin_dio1,
                                   dio1_isr_handler,
                                   ctx);
        if (err == ESP_OK) {
            ctx->isr_ready = true;
        }
    }
    if (err == ESP_OK) {
        rf_switch_rx(ctx);
        err = sx_to_esp(sx126x_set_rx_with_timeout_in_rtc_step(
            &ctx->hw,
            SX126X_RX_CONTINUOUS));
    }
    if (err == ESP_OK) {
        ctx->rx_running = true;
        BaseType_t ok = xTaskCreate(c1262_rx_task,
                                    "c1262_rx",
                                    C1262_RX_TASK_STACK_BYTES,
                                    ctx,
                                    C1262_RX_TASK_PRIORITY,
                                    &ctx->rx_task_handle);
        if (ok != pdPASS) {
            ctx->rx_running = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        driver->initialized = true;
        err = meshpay_hal_init(hal, board, &LORA_CORE1262_OPS, driver);
    }
    if (err != ESP_OK) {
        (void)meshpay_hal_lora_core1262_driver_deinit(driver);
        return err;
    }

    ESP_LOGI(TAG,
             "Core1262 ready spi=%d freq=%lu sf=%u power=%d tcxo=%u cal_image=%s rf_switch=%s",
             effective.spi_host,
             (unsigned long)effective.frequency_hz,
             (unsigned)effective.spreading_factor,
             (int)effective.tx_power_dbm,
             (unsigned)effective.tcxo_ctrl_voltage,
             effective.calibrate_image ? "on" : "off",
             ctx->dio2_rf_switch ? "dio2" : "gpio");
    return ESP_OK;
}

esp_err_t meshpay_hal_lora_core1262_driver_deinit(
    meshpay_hal_lora_core1262_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_lora_core1262_internal_t *ctx = driver->internal;
    if (ctx != NULL) {
        if (ctx->rx_running) {
            ctx->rx_running = false;
            if (ctx->dio1_sem != NULL) {
                xSemaphoreGive(ctx->dio1_sem);
            }
            if (ctx->rx_stop_sem != NULL) {
                (void)xSemaphoreTake(ctx->rx_stop_sem, pdMS_TO_TICKS(1000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            ctx->rx_task_handle = NULL;
        }
        if (ctx->isr_ready) {
            (void)gpio_isr_handler_remove((gpio_num_t)ctx->pin_dio1);
        }
        if (ctx->radio_mutex != NULL &&
            xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (ctx->spi_device_ready) {
                (void)sx126x_set_standby(&ctx->hw, SX126X_STANDBY_CFG_RC);
            }
            xSemaphoreGive(ctx->radio_mutex);
        }
        if (ctx->spi_device_ready) {
            (void)spi_bus_remove_device(ctx->hw.spi);
        }
        if (ctx->spi_bus_ready) {
            (void)spi_bus_free(ctx->spi_host);
        }
        if (ctx->rx_queue != NULL) {
            vQueueDelete(ctx->rx_queue);
        }
        if (ctx->dio1_sem != NULL) {
            vSemaphoreDelete(ctx->dio1_sem);
        }
        if (ctx->rx_stop_sem != NULL) {
            vSemaphoreDelete(ctx->rx_stop_sem);
        }
        if (ctx->radio_mutex != NULL) {
            vSemaphoreDelete(ctx->radio_mutex);
        }
        heap_caps_free(ctx);
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}
