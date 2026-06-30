#include "meshpay/device_hal.h"

#include "sdkconfig.h"

#include <string.h>

#define WS147_PIN_MOSI 39
#define WS147_PIN_SCK 38
#define WS147_PIN_CS 21
#define WS147_PIN_DC 45
#define WS147_PIN_RST 40
#define WS147_PIN_BL 46
#define WS147_BOOT_FILL_RGB565 0xffff

#define AXS5106_PIN_SCL 41
#define AXS5106_PIN_SDA 42
#define AXS5106_PIN_RST 47
#define AXS5106_I2C_ADDR 0x63
#define AXS5106_REG_TOUCH 0x01

#define JD9853_SPI_CLOCK_HZ (10 * 1000 * 1000)
#define JD9853_SPI_MAX_TRANSFER_SIZE (32 * 1024)
#define JD9853_SPI_POLLING_CHUNK_BYTES 64
#define JD9853_INIT_DELAY_FLAG 0x80

#define WS147_BL_LEDC_TIMER LEDC_TIMER_0
#define WS147_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define WS147_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define WS147_BL_LEDC_FREQ_HZ 5000
#define WS147_BL_LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define WS147_BL_LEDC_MAX_DUTY 255

#define JD9853_CMD_CASET 0x2A
#define JD9853_CMD_RASET 0x2B
#define JD9853_CMD_RAMWR 0x2C
#define JD9853_CMD_MADCTL 0x36
#define JD9853_CMD_INVON 0x21

esp_err_t meshpay_hal_waveshare_s3_touch_window(uint16_t width,
                                                uint16_t height,
                                                uint8_t caset[4],
                                                uint8_t raset[4])
{
    if (caset == NULL || raset == NULL || width == 0 || height == 0 ||
        width > MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH ||
        height > MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t x2 = width - 1U;
    const uint16_t y1 = MESHPAY_HAL_WAVESHARE_S3_TOUCH_Y_OFFSET;
    const uint16_t y2 = y1 + height - 1U;

    caset[0] = 0;
    caset[1] = 0;
    caset[2] = (uint8_t)(x2 >> 8);
    caset[3] = (uint8_t)x2;
    raset[0] = (uint8_t)(y1 >> 8);
    raset[1] = (uint8_t)y1;
    raset[2] = (uint8_t)(y2 >> 8);
    raset[3] = (uint8_t)y2;
    return ESP_OK;
}

esp_err_t meshpay_hal_waveshare_s3_touch_decode(
    const uint8_t *touch_data,
    size_t touch_data_len,
    meshpay_touch_state_t *state)
{
    if (touch_data == NULL || state == NULL ||
        touch_data_len < MESHPAY_HAL_WAVESHARE_S3_TOUCH_DATA_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    if (touch_data[1] == 0) {
        return ESP_OK;
    }

    const uint16_t raw_x =
        ((uint16_t)(touch_data[2] & 0x0f) << 8) | touch_data[3];
    const uint16_t raw_y =
        ((uint16_t)(touch_data[4] & 0x0f) << 8) | touch_data[5];

    uint16_t x = raw_y;
    uint16_t y = raw_x;
    if (x >= MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH) {
        x = MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH - 1U;
    }
    if (y >= MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT) {
        y = MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT - 1U;
    }

    state->pressed = true;
    state->x = (int16_t)x;
    state->y = (int16_t)y;
    return ESP_OK;
}

esp_err_t meshpay_hal_waveshare_s3_rgb565_to_be(const uint16_t *pixels,
                                                size_t pixel_count,
                                                uint8_t *out,
                                                size_t out_size)
{
    if (pixels == NULL || out == NULL ||
        out_size < pixel_count *
                       MESHPAY_HAL_WAVESHARE_S3_RGB565_BYTES_PER_PIXEL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        out[i * 2U] = (uint8_t)(pixels[i] >> 8);
        out[i * 2U + 1U] = (uint8_t)pixels[i];
    }
    return ESP_OK;
}

/* Gardé par le board Kconfig (comme le H752) : sur un build S3 d'une AUTRE carte
 * — ou le test_app (board UNKNOWN) — ce driver (i2c « neuf », i2c_master.h) ne
 * doit PAS être lié, sinon il entre en conflit avec le driver i2c « ancien »
 * du T-Deck/H752 (abort au boot : « driver_ng is not allowed with this old
 * driver »). */
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal_ws147";

static const uint8_t JD9853_INIT_SEQ[] = {
    0x11, 0x00 | JD9853_INIT_DELAY_FLAG, 120,

    0xDF, 2, 0x98, 0x53,
    0xB2, 1, 0x23,
    0xB7, 4, 0x00, 0x47, 0x00, 0x6F,
    0xBB, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,
    0xC0, 2, 0x44, 0xA4,
    0xC1, 1, 0x16,
    0xC3, 8, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,
    0xC4, 12, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A,
        0x16, 0x82,

    0xC8, 32,
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

    0xD0, 5, 0x04, 0x06, 0x6B, 0x0F, 0x00,
    0xD7, 2, 0x00, 0x30,
    0xE6, 1, 0x14,
    0xDE, 1, 0x01,
    0xB7, 5, 0x03, 0x13, 0xEF, 0x35, 0x35,
    0xC1, 3, 0x14, 0x15, 0xC0,
    0xC2, 2, 0x06, 0x3A,
    0xC4, 2, 0x72, 0x12,
    0xBE, 1, 0x00,
    0xDE, 1, 0x02,
    0xE5, 3, 0x00, 0x02, 0x00,
    0xE5, 3, 0x01, 0x02, 0x00,
    0xDE, 1, 0x00,
    0x35, 1, 0x00,
    0x3A, 1, 0x05,
    0x2A, 4, 0x00, 0x22, 0x00, 0xCD,
    0x2B, 4, 0x00, 0x00, 0x01, 0x3F,
    0xDE, 1, 0x02,
    0xE5, 3, 0x00, 0x02, 0x00,
    0xDE, 1, 0x00,
    0x36, 1, 0x00,
    0x29, 0,
};

static esp_err_t send_cmd(meshpay_hal_waveshare_s3_touch_driver_t *driver,
                          uint8_t cmd)
{
    gpio_set_level(WS147_PIN_DC, 0);
    spi_transaction_t transaction = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
    };
    transaction.tx_data[0] = cmd;
    return spi_device_polling_transmit((spi_device_handle_t)driver->spi_handle,
                                       &transaction);
}

static esp_err_t send_data(meshpay_hal_waveshare_s3_touch_driver_t *driver,
                           const uint8_t *data,
                           size_t len)
{
    if (driver == NULL || driver->spi_handle == NULL ||
        (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    gpio_set_level(WS147_PIN_DC, 1);
    size_t offset = 0;
    while (offset < len) {
        const size_t remaining = len - offset;
        const size_t chunk = remaining < JD9853_SPI_POLLING_CHUNK_BYTES
                                 ? remaining
                                 : JD9853_SPI_POLLING_CHUNK_BYTES;
        spi_transaction_t transaction = {
            .length = chunk * 8U,
        };
        if (chunk <= sizeof(transaction.tx_data)) {
            transaction.flags = SPI_TRANS_USE_TXDATA;
            memcpy(transaction.tx_data, data + offset, chunk);
        } else {
            transaction.tx_buffer = data + offset;
        }

        esp_err_t err =
            spi_device_polling_transmit((spi_device_handle_t)driver->spi_handle,
                                        &transaction);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t send_cmd_data(meshpay_hal_waveshare_s3_touch_driver_t *driver,
                               uint8_t cmd,
                               uint8_t data)
{
    esp_err_t err = send_cmd(driver, cmd);
    if (err != ESP_OK) {
        return err;
    }
    return send_data(driver, &data, 1);
}

static esp_err_t run_init_sequence(
    meshpay_hal_waveshare_s3_touch_driver_t *driver)
{
    size_t i = 0;
    while (i < sizeof(JD9853_INIT_SEQ)) {
        if (i + 2U > sizeof(JD9853_INIT_SEQ)) {
            return ESP_ERR_INVALID_SIZE;
        }

        const uint8_t cmd = JD9853_INIT_SEQ[i++];
        const uint8_t count_byte = JD9853_INIT_SEQ[i++];
        const bool has_delay = (count_byte & JD9853_INIT_DELAY_FLAG) != 0;
        const uint8_t data_len = count_byte & 0x7f;

        if (i + data_len + (has_delay ? 1U : 0U) >
            sizeof(JD9853_INIT_SEQ)) {
            return ESP_ERR_INVALID_SIZE;
        }

        esp_err_t err = send_cmd(driver, cmd);
        if (err == ESP_OK && data_len > 0) {
            err = send_data(driver, &JD9853_INIT_SEQ[i], data_len);
        }
        if (err != ESP_OK) {
            return err;
        }

        i += data_len;
        if (has_delay) {
            vTaskDelay(pdMS_TO_TICKS(JD9853_INIT_SEQ[i++]));
        }
    }
    return ESP_OK;
}

static esp_err_t init_gpio(void)
{
    const uint64_t output_pins =
        (1ULL << WS147_PIN_DC) | (1ULL << WS147_PIN_RST) |
        (1ULL << AXS5106_PIN_RST);
    gpio_config_t output_config = {
        .pin_bit_mask = output_pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&output_config);
}

static void reset_touch(void)
{
    gpio_set_level(AXS5106_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(AXS5106_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void reset_lcd(void)
{
    gpio_set_level(WS147_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(WS147_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t init_spi(meshpay_hal_waveshare_s3_touch_driver_t *driver)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = WS147_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = WS147_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
#if SOC_SPI_SUPPORT_OCT
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
#endif
        .max_transfer_sz = JD9853_SPI_MAX_TRANSFER_SIZE,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST,
                                       &bus_config,
                                       SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = JD9853_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = WS147_PIN_CS,
        .queue_size = 4,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t handle = NULL;
    err = spi_bus_add_device(SPI2_HOST, &device_config, &handle);
    if (err != ESP_OK) {
        (void)spi_bus_free(SPI2_HOST);
        return err;
    }

    driver->spi_handle = (void *)handle;
    return ESP_OK;
}

static esp_err_t init_i2c(meshpay_hal_waveshare_s3_touch_driver_t *driver)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = AXS5106_PIN_SCL,
        .sda_io_num = AXS5106_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXS5106_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    i2c_master_dev_handle_t device = NULL;
    err = i2c_master_bus_add_device(bus, &device_config, &device);
    if (err != ESP_OK) {
        (void)i2c_del_master_bus(bus);
        return err;
    }

    driver->i2c_bus_handle = (void *)bus;
    driver->i2c_dev_handle = (void *)device;

    err = i2c_master_probe(bus, AXS5106_I2C_ADDR, 100);
    driver->touch_available = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "AXS5106L probe failed at 0x%02x: %s",
                 AXS5106_I2C_ADDR,
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t fill_color(meshpay_hal_waveshare_s3_touch_driver_t *driver,
                            uint16_t rgb565);

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = WS147_BL_LEDC_MODE,
        .timer_num = WS147_BL_LEDC_TIMER,
        .duty_resolution = WS147_BL_LEDC_RESOLUTION,
        .freq_hz = WS147_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_config = {
        .speed_mode = WS147_BL_LEDC_MODE,
        .channel = WS147_BL_LEDC_CHANNEL,
        .timer_sel = WS147_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = WS147_PIN_BL,
        .duty = WS147_BL_LEDC_MAX_DUTY,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        return err;
    }

    err = ledc_update_duty(WS147_BL_LEDC_MODE, WS147_BL_LEDC_CHANNEL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Waveshare backlight LEDC GPIO%u duty=%u",
                 (unsigned)WS147_PIN_BL,
                 (unsigned)WS147_BL_LEDC_MAX_DUTY);
    }
    return err;
}

static esp_err_t fill_window_color(
    meshpay_hal_waveshare_s3_touch_driver_t *driver,
    const uint8_t caset[4],
    const uint8_t raset[4],
    size_t pixel_count,
    uint16_t rgb565)
{
    esp_err_t err = send_cmd(driver, JD9853_CMD_CASET);
    if (err == ESP_OK) {
        err = send_data(driver, caset, 4);
    }
    if (err == ESP_OK) {
        err = send_cmd(driver, JD9853_CMD_RASET);
    }
    if (err == ESP_OK) {
        err = send_data(driver, raset, 4);
    }
    if (err == ESP_OK) {
        err = send_cmd(driver, JD9853_CMD_RAMWR);
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t tx_buf[256];
    for (size_t i = 0; i < sizeof(tx_buf); i += 2U) {
        tx_buf[i] = (uint8_t)(rgb565 >> 8);
        tx_buf[i + 1U] = (uint8_t)rgb565;
    }

    size_t remaining =
        pixel_count *
        MESHPAY_HAL_WAVESHARE_S3_RGB565_BYTES_PER_PIXEL;
    while (remaining > 0) {
        const size_t chunk = remaining < sizeof(tx_buf)
                                 ? remaining
                                 : sizeof(tx_buf);
        err = send_data(driver, tx_buf, chunk);
        if (err != ESP_OK) {
            return err;
        }
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t fill_color(meshpay_hal_waveshare_s3_touch_driver_t *driver,
                            uint16_t rgb565)
{
    uint8_t caset[4] = {0};
    uint8_t raset[4] = {0};
    esp_err_t err = meshpay_hal_waveshare_s3_touch_window(
        MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH,
        MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT,
        caset,
        raset);
    if (err != ESP_OK) {
        return err;
    }
    return fill_window_color(
        driver,
        caset,
        raset,
        (size_t)MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH *
            (size_t)MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT,
        rgb565);
}

static esp_err_t display_init(void *ctx)
{
    meshpay_hal_waveshare_s3_touch_driver_t *driver =
        (meshpay_hal_waveshare_s3_touch_driver_t *)ctx;
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initialising Waveshare 1.47 JD9853 + AXS5106L");

    esp_err_t err = init_gpio();
    if (err == ESP_OK) {
        err = init_spi(driver);
    }
    if (err == ESP_OK) {
        reset_touch();
        err = init_i2c(driver);
    }
    if (err == ESP_OK) {
        err = init_backlight();
    }
    if (err == ESP_OK) {
        reset_lcd();
        err = run_init_sequence(driver);
    }
    if (err == ESP_OK) {
        err = send_cmd(driver, JD9853_CMD_INVON);
    }
    if (err == ESP_OK) {
        err = send_cmd_data(driver, JD9853_CMD_MADCTL, 0x60);
        if (err == ESP_OK) {
            err = fill_color(driver, WS147_BOOT_FILL_RGB565);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Waveshare display init failed: %s",
                 esp_err_to_name(err));
        (void)meshpay_hal_waveshare_s3_touch_driver_deinit(driver);
        return err;
    }

    driver->initialized = true;
    ESP_LOGI(TAG,
             "Waveshare display ready %ux%u touch=%s",
             (unsigned)MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH,
             (unsigned)MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT,
             driver->touch_available ? "ok" : "unavailable");
    return ESP_OK;
}

static esp_err_t display_flush(void *ctx,
                               const void *pixels,
                               uint16_t width,
                               uint16_t height)
{
    meshpay_hal_waveshare_s3_touch_driver_t *driver =
        (meshpay_hal_waveshare_s3_touch_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t caset[4] = {0};
    uint8_t raset[4] = {0};
    esp_err_t err = meshpay_hal_waveshare_s3_touch_window(width,
                                                          height,
                                                          caset,
                                                          raset);
    if (err != ESP_OK) {
        return err;
    }

    err = send_cmd(driver, JD9853_CMD_CASET);
    if (err == ESP_OK) {
        err = send_data(driver, caset, sizeof(caset));
    }
    if (err == ESP_OK) {
        err = send_cmd(driver, JD9853_CMD_RASET);
    }
    if (err == ESP_OK) {
        err = send_data(driver, raset, sizeof(raset));
    }
    if (err == ESP_OK) {
        err = send_cmd(driver, JD9853_CMD_RAMWR);
    }
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t *src = (const uint16_t *)pixels;
    size_t remaining = (size_t)width * (size_t)height;
    uint8_t tx_buf[1024];
    while (remaining > 0) {
        size_t chunk_pixels =
            remaining < (sizeof(tx_buf) / 2U) ? remaining
                                               : (sizeof(tx_buf) / 2U);
        err = meshpay_hal_waveshare_s3_rgb565_to_be(src,
                                                    chunk_pixels,
                                                    tx_buf,
                                                    sizeof(tx_buf));
        if (err == ESP_OK) {
            err = send_data(driver, tx_buf, chunk_pixels * 2U);
        }
        if (err != ESP_OK) {
            return err;
        }
        src += chunk_pixels;
        remaining -= chunk_pixels;
    }
    return ESP_OK;
}

static esp_err_t touch_read(void *ctx, meshpay_touch_state_t *state)
{
    meshpay_hal_waveshare_s3_touch_driver_t *driver =
        (meshpay_hal_waveshare_s3_touch_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    if (!driver->touch_available || driver->i2c_dev_handle == NULL) {
        return ESP_OK;
    }

    uint8_t reg_addr = AXS5106_REG_TOUCH;
    uint8_t touch_data[MESHPAY_HAL_WAVESHARE_S3_TOUCH_DATA_LEN] = {0};
    esp_err_t err = i2c_master_transmit(
        (i2c_master_dev_handle_t)driver->i2c_dev_handle,
        &reg_addr,
        1,
        100);
    if (err == ESP_OK) {
        err = i2c_master_receive(
            (i2c_master_dev_handle_t)driver->i2c_dev_handle,
            touch_data,
            sizeof(touch_data),
            100);
    }
    if (err != ESP_OK) {
        static int64_t last_warn_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 1000000) {
            ESP_LOGW(TAG, "AXS5106L read failed: %s", esp_err_to_name(err));
            last_warn_us = now_us;
        }
        if (err == ESP_ERR_INVALID_STATE && driver->i2c_bus_handle != NULL) {
            (void)i2c_master_bus_reset(
                (i2c_master_bus_handle_t)driver->i2c_bus_handle);
        }
        return ESP_OK;
    }

    return meshpay_hal_waveshare_s3_touch_decode(touch_data,
                                                 sizeof(touch_data),
                                                 state);
}

static const meshpay_hal_ops_t WAVESHARE_S3_TOUCH_OPS = {
    .display_init = display_init,
    .display_flush = display_flush,
    .touch_read = touch_read,
};

esp_err_t meshpay_hal_waveshare_s3_touch_driver_init(
    meshpay_hal_waveshare_s3_touch_driver_t *driver,
    meshpay_hal_t *hal)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return meshpay_hal_init(hal,
                            MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
                            &WAVESHARE_S3_TOUCH_OPS,
                            driver);
}

esp_err_t meshpay_hal_waveshare_s3_touch_driver_deinit(
    meshpay_hal_waveshare_s3_touch_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (driver->spi_handle != NULL) {
        (void)spi_bus_remove_device((spi_device_handle_t)driver->spi_handle);
    }
    if (driver->spi_handle != NULL || driver->initialized) {
        (void)spi_bus_free(SPI2_HOST);
    }
    if (driver->i2c_dev_handle != NULL) {
        (void)i2c_master_bus_rm_device(
            (i2c_master_dev_handle_t)driver->i2c_dev_handle);
    }
    if (driver->i2c_bus_handle != NULL) {
        (void)i2c_del_master_bus(
            (i2c_master_bus_handle_t)driver->i2c_bus_handle);
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#else

esp_err_t meshpay_hal_waveshare_s3_touch_driver_init(
    meshpay_hal_waveshare_s3_touch_driver_t *driver,
    meshpay_hal_t *hal)
{
    (void)driver;
    (void)hal;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t meshpay_hal_waveshare_s3_touch_driver_deinit(
    meshpay_hal_waveshare_s3_touch_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#endif
