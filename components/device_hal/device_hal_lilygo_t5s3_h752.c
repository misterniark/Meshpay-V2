#include "meshpay/device_hal.h"

#include "sdkconfig.h"

#include <string.h>

#define H752_PIN_SDA 6
#define H752_PIN_SCL 5
#define H752_PIN_TOUCH_INT 15
#define H752_PIN_TOUCH_RST 41
#define H752_PIN_BL 40

#define H752_TOUCH_ADDR_CST 0x5A
#define H752_TOUCH_ADDR_GT911 0x5D
#define H752_TOUCH_ADDR_GT911_ALT 0x14

#define H752_CST_REG_STATUS 0xD000
#define H752_CST_REG_POINTS 0xD007
#define H752_CST_REG_POWER 0xD106
#define H752_GT911_REG_COMMAND 0x8040
#define H752_GT911_REG_PRODUCT_ID 0x8140
#define H752_GT911_REG_STATUS 0x814E

#define H752_BL_LEDC_FREQ_HZ 5000
#define H752_CLEAR_CYCLES 1
#define H752_CLEAR_CYCLE_TIME 50
#define H752_REFRESH_PASSES 2

esp_err_t meshpay_hal_lilygo_h752_transform_touch(uint16_t raw_x,
                                                  uint16_t raw_y,
                                                  meshpay_touch_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t x = raw_y;
    int32_t y = MESHPAY_HAL_LILYGO_H752_HEIGHT - (int32_t)raw_x;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= MESHPAY_HAL_LILYGO_H752_WIDTH) {
        x = MESHPAY_HAL_LILYGO_H752_WIDTH - 1;
    }
    if (y >= MESHPAY_HAL_LILYGO_H752_HEIGHT) {
        y = MESHPAY_HAL_LILYGO_H752_HEIGHT - 1;
    }

    state->pressed = true;
    state->x = (int16_t)x;
    state->y = (int16_t)y;
    return ESP_OK;
}

uint16_t meshpay_hal_lilygo_h752_tps65185_vcom_code(uint16_t vcom_mv)
{
    return (uint16_t)(vcom_mv / 10U);
}

uint8_t meshpay_hal_lilygo_h752_rgb565_to_gray4(uint16_t rgb565)
{
    const uint8_t r = (uint8_t)(((rgb565 >> 11) & 0x1FU) * 255U / 31U);
    const uint8_t g = (uint8_t)(((rgb565 >> 5) & 0x3FU) * 255U / 63U);
    const uint8_t b = (uint8_t)((rgb565 & 0x1FU) * 255U / 31U);
    const uint8_t lum =
        (uint8_t)(((uint16_t)r * 30U + (uint16_t)g * 59U +
                   (uint16_t)b * 11U) /
                  100U);
    return lum >= 250U ? 0x0FU : 0x00U;
}

esp_err_t meshpay_hal_lilygo_h752_rgb565_to_epd4(const uint16_t *pixels,
                                                 size_t pixel_count,
                                                 uint8_t *out,
                                                 size_t out_size)
{
    const size_t required = (pixel_count + 1U) / 2U;
    if (pixels == NULL || out == NULL || out_size < required) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, required);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t gray = meshpay_hal_lilygo_h752_rgb565_to_gray4(pixels[i]);
        if ((i & 1U) == 0U) {
            out[i / 2U] = (uint8_t)((out[i / 2U] & 0xF0U) | gray);
        } else {
            out[i / 2U] = (uint8_t)((out[i / 2U] & 0x0FU) | (gray << 4));
        }
    }
    return ESP_OK;
}

esp_err_t meshpay_hal_lilygo_h752_gt911_decode(const uint8_t *frame,
                                               size_t frame_len,
                                               meshpay_touch_state_t *state)
{
    if (frame == NULL || state == NULL ||
        frame_len < MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    const uint8_t status = frame[0];
    if ((status & 0x80U) == 0U || (status & 0x0FU) == 0U) {
        return ESP_OK;
    }

    const uint16_t raw_x = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
    const uint16_t raw_y = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
    return meshpay_hal_lilygo_h752_transform_touch(raw_x, raw_y, state);
}

#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lilygo_epd47_h752/epd_driver.h"

#include <stdlib.h>

static const char *TAG = "hal_lilygo_h752";

#define H752_BQ27220_ADDR 0x55
#define H752_BQ27220_CMD_VOLTAGE 0x08
#define H752_BQ27220_CMD_SOC 0x2C
#define H752_BATT_ADC_CHANNEL ADC_CHANNEL_3
#define H752_BATT_ADC_ATTEN ADC_ATTEN_DB_12
#define H752_BATT_ADC_DIVIDER 2U
#define H752_ADC_CALI_SCHEME_CURVE 1U
#define H752_ADC_CALI_SCHEME_LINE 2U

static esp_err_t i2c_write_reg16(uint8_t addr,
                                 uint16_t reg,
                                 const uint8_t *data,
                                 size_t len)
{
    uint8_t buf[2 + 8];
    if (len > 8U) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)reg;
    if (len > 0U && data != NULL) {
        memcpy(&buf[2], data, len);
    }
    return i2c_master_write_to_device(I2C_NUM_0,
                                      addr,
                                      buf,
                                      len + 2U,
                                      pdMS_TO_TICKS(80));
}

static esp_err_t i2c_read_reg16(uint8_t addr,
                                uint16_t reg,
                                uint8_t *data,
                                size_t len)
{
    uint8_t reg_buf[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)reg,
    };
    return i2c_master_write_read_device(I2C_NUM_0,
                                        addr,
                                        reg_buf,
                                        sizeof(reg_buf),
                                        data,
                                        len,
                                        pdMS_TO_TICKS(80));
}

static esp_err_t probe_i2c_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    const esp_err_t err =
        i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(80));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t i2c_read_reg8_word(uint8_t addr, uint8_t reg, uint16_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_write_read_device(I2C_NUM_0,
                                                 addr,
                                                 &reg,
                                                 1,
                                                 data,
                                                 sizeof(data),
                                                 pdMS_TO_TICKS(80));
    if (err != ESP_OK) {
        return err;
    }
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

static uint8_t h752_estimate_battery_percent(uint16_t mv)
{
    if (mv >= 4200U) {
        return 100;
    }
    if (mv <= 3300U) {
        return 0;
    }
    return (uint8_t)(((uint32_t)(mv - 3300U) * 100U) / 900U);
}

static bool h752_adc_calibration_init(adc_cali_handle_t *out_handle,
                                      uint8_t *out_scheme)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t err = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1,
            .chan = H752_BATT_ADC_CHANNEL,
            .atten = H752_BATT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_curve_fitting(&config, &handle);
        calibrated = err == ESP_OK;
        if (calibrated && out_scheme != NULL) {
            *out_scheme = H752_ADC_CALI_SCHEME_CURVE;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t config = {
            .unit_id = ADC_UNIT_1,
            .atten = H752_BATT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_line_fitting(&config, &handle);
        calibrated = err == ESP_OK;
        if (calibrated && out_scheme != NULL) {
            *out_scheme = H752_ADC_CALI_SCHEME_LINE;
        }
    }
#endif

    if (out_handle != NULL) {
        *out_handle = calibrated ? handle : NULL;
    }
    if (!calibrated && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGD(TAG, "Battery ADC calibration unavailable: %s",
                 esp_err_to_name(err));
    }
    return calibrated;
}

static esp_err_t h752_battery_adc_init(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->adc_ready) {
        return ESP_OK;
    }

    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = H752_BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(adc_handle,
                                     H752_BATT_ADC_CHANNEL,
                                     &channel_config);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(adc_handle);
        return err;
    }

    adc_cali_handle_t cali_handle = NULL;
    uint8_t cali_scheme = 0;
    driver->adc_calibrated =
        h752_adc_calibration_init(&cali_handle, &cali_scheme);
    driver->adc_cali_handle = (void *)cali_handle;
    driver->adc_cali_scheme = cali_scheme;
    driver->adc_handle = (void *)adc_handle;
    driver->adc_ready = true;
    return ESP_OK;
}

static esp_err_t h752_battery_adc_mv(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver,
    uint16_t *mv)
{
    if (driver == NULL || mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = h752_battery_adc_init(driver);
    if (err != ESP_OK) {
        return err;
    }

    int raw = 0;
    int raw_sum = 0;
    adc_oneshot_unit_handle_t adc_handle =
        (adc_oneshot_unit_handle_t)driver->adc_handle;
    for (uint8_t i = 0; i < 4U; ++i) {
        err = adc_oneshot_read(adc_handle, H752_BATT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        raw_sum += raw;
    }
    raw = raw_sum / 4;

    int pin_mv = 0;
    if (driver->adc_calibrated && driver->adc_cali_handle != NULL) {
        err = adc_cali_raw_to_voltage(
            (adc_cali_handle_t)driver->adc_cali_handle,
            raw,
            &pin_mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        pin_mv = (raw * 3300) / 4095;
    }

    uint32_t battery_mv = (uint32_t)pin_mv * H752_BATT_ADC_DIVIDER;
    if (battery_mv > UINT16_MAX) {
        battery_mv = UINT16_MAX;
    }
    *mv = (uint16_t)battery_mv;
    return ESP_OK;
}

static esp_err_t h752_battery_status(void *ctx,
                                     uint16_t *mv,
                                     uint8_t *percent)
{
    if (ctx == NULL || mv == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver =
        (meshpay_hal_lilygo_t5s3_h752_driver_t *)ctx;
    if (!driver->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t gauge_mv = 0;
    esp_err_t err = i2c_read_reg8_word(H752_BQ27220_ADDR,
                                       H752_BQ27220_CMD_VOLTAGE,
                                       &gauge_mv);
    if (err == ESP_OK && gauge_mv <= 6000U) {
        uint16_t gauge_soc = 0;
        *mv = gauge_mv;
        if (i2c_read_reg8_word(H752_BQ27220_ADDR,
                               H752_BQ27220_CMD_SOC,
                               &gauge_soc) == ESP_OK) {
            *percent = gauge_soc > 100U ? 100U : (uint8_t)gauge_soc;
        } else {
            *percent = h752_estimate_battery_percent(gauge_mv);
        }
        return ESP_OK;
    }

    err = h752_battery_adc_mv(driver, mv);
    if (err != ESP_OK) {
        return err;
    }
    *percent = h752_estimate_battery_percent(*mv);
    return ESP_OK;
}

static esp_err_t h752_battery_mv(void *ctx, uint16_t *mv)
{
    uint8_t percent = 0;
    return h752_battery_status(ctx, mv, &percent);
}

static void configure_touch_int_input(void)
{
    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << H752_PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&int_conf);
}

static void gt911_select_address(uint8_t addr)
{
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << H752_PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&rst_conf);

    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << H752_PIN_TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&int_conf);

    gpio_set_level(H752_PIN_TOUCH_RST, 0);
    gpio_set_level(H752_PIN_TOUCH_INT,
                   addr == H752_TOUCH_ADDR_GT911_ALT ? 1 : 0);
    esp_rom_delay_us(120);
    gpio_set_level(H752_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    configure_touch_int_input();
    vTaskDelay(pdMS_TO_TICKS(20));
}

#if CONFIG_MESHPAY_LILYGO_H752_TPS65185
static esp_err_t tps65185_write_reg8(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_write_to_device(I2C_NUM_0,
                                      MESHPAY_HAL_LILYGO_H752_TPS65185_ADDR,
                                      buf,
                                      sizeof(buf),
                                      pdMS_TO_TICKS(80));
}

static esp_err_t init_tps65185(void)
{
    esp_err_t err = tps65185_write_reg8(0x09, 0xE1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "TPS65185 not available at 0x%02X: %s",
                 MESHPAY_HAL_LILYGO_H752_TPS65185_ADDR,
                 esp_err_to_name(err));
        return err;
    }
    (void)tps65185_write_reg8(0x0A, 0xAA);

    const uint16_t vcom_code = meshpay_hal_lilygo_h752_tps65185_vcom_code(
        MESHPAY_HAL_LILYGO_H752_TPS65185_VCOM_MV);
    const uint8_t vcom_buf[3] = {
        0x03,
        (uint8_t)vcom_code,
        (uint8_t)(vcom_code >> 8),
    };
    err = i2c_master_write_to_device(I2C_NUM_0,
                                     MESHPAY_HAL_LILYGO_H752_TPS65185_ADDR,
                                     vcom_buf,
                                     sizeof(vcom_buf),
                                     pdMS_TO_TICKS(80));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TPS65185 Vcom write failed: %s", esp_err_to_name(err));
    }
    (void)tps65185_write_reg8(0x01, 0x3F);
    ESP_LOGI(TAG,
             "TPS65185 configured: Vcom=-%u mV code=0x%04x",
             (unsigned)MESHPAY_HAL_LILYGO_H752_TPS65185_VCOM_MV,
             (unsigned)vcom_code);
    return ESP_OK;
}
#endif

static void log_gt911_product_id(uint8_t addr)
{
    uint8_t product[4] = {0};
    if (i2c_read_reg16(addr,
                       H752_GT911_REG_PRODUCT_ID,
                       product,
                       sizeof(product)) == ESP_OK) {
        ESP_LOGI(TAG,
                 "GT911 product id: %c%c%c%c",
                 product[0] != 0 ? product[0] : ' ',
                 product[1] != 0 ? product[1] : ' ',
                 product[2] != 0 ? product[2] : ' ',
                 product[3] != 0 ? product[3] : ' ');
    }
}

static esp_err_t init_i2c_touch(meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    gt911_select_address(H752_TOUCH_ADDR_GT911);

    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = H752_PIN_SDA,
        .scl_io_num = H752_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags = 0,
    };
    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (probe_i2c_addr(H752_TOUCH_ADDR_GT911) == ESP_OK) {
        driver->touch_available = true;
        driver->touch_cst = false;
        driver->touch_addr = H752_TOUCH_ADDR_GT911;
    } else {
        gt911_select_address(H752_TOUCH_ADDR_GT911_ALT);
        if (probe_i2c_addr(H752_TOUCH_ADDR_GT911_ALT) == ESP_OK) {
            driver->touch_available = true;
            driver->touch_cst = false;
            driver->touch_addr = H752_TOUCH_ADDR_GT911_ALT;
            ESP_LOGW(TAG, "GT911 detected at 0x14; H752 normally uses 0x5D");
        }
    }

    if (driver->touch_available && !driver->touch_cst) {
        log_gt911_product_id(driver->touch_addr);
        const uint8_t clear = 0;
        (void)i2c_write_reg16(driver->touch_addr,
                              H752_GT911_REG_COMMAND,
                              &clear,
                              1);
        (void)i2c_write_reg16(driver->touch_addr,
                              H752_GT911_REG_STATUS,
                              &clear,
                              1);
    }

    if (!driver->touch_available &&
        probe_i2c_addr(H752_TOUCH_ADDR_CST) == ESP_OK) {
        driver->touch_available = true;
        driver->touch_cst = true;
        driver->touch_addr = H752_TOUCH_ADDR_CST;
        const uint8_t wake = 0x06;
        (void)i2c_write_reg16(driver->touch_addr, H752_CST_REG_POWER, &wake, 1);
    }

    ESP_LOGI(TAG,
             "H752 touch %s addr=0x%02x SDA=%d SCL=%d",
             driver->touch_available
                 ? (driver->touch_cst ? "CST" : "GT911")
                 : "absent",
             driver->touch_addr,
             H752_PIN_SDA,
             H752_PIN_SCL);
    return driver->touch_available ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t init_backlight(meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    gpio_set_direction(H752_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(H752_PIN_BL, 0);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = H752_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = H752_PIN_BL,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err == ESP_OK) {
        driver->backlight_ready = true;
    }
    return err;
}

static void refresh_full(meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    if (driver->framebuffer == NULL) {
        return;
    }

    ESP_LOGI(TAG, "H752 e-paper full refresh");
    epd_poweron();
    epd_clear_area_cycles(epd_full_screen(),
                          H752_CLEAR_CYCLES,
                          H752_CLEAR_CYCLE_TIME);
    for (uint8_t pass = 0; pass < H752_REFRESH_PASSES; ++pass) {
        epd_draw_grayscale_image(epd_full_screen(), driver->framebuffer);
    }
    epd_poweroff();
    driver->dirty = false;
    driver->next_refresh_us = esp_timer_get_time();
}

static esp_err_t h752_display_init(void *ctx)
{
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver =
        (meshpay_hal_lilygo_t5s3_h752_driver_t *)ctx;
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->initialized) {
        return ESP_OK;
    }

    if (driver->lock == NULL) {
        driver->lock = xSemaphoreCreateMutex();
        if (driver->lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    driver->framebuffer = heap_caps_malloc(MESHPAY_HAL_LILYGO_H752_FB_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (driver->framebuffer == NULL) {
        driver->framebuffer = heap_caps_malloc(MESHPAY_HAL_LILYGO_H752_FB_SIZE,
                                               MALLOC_CAP_8BIT);
    }
    if (driver->framebuffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(driver->framebuffer, 0xFF, MESHPAY_HAL_LILYGO_H752_FB_SIZE);

    ESP_LOGI(TAG,
             "Init LilyGo T5 S3 Pro H752 display %ux%u",
             (unsigned)MESHPAY_HAL_LILYGO_H752_WIDTH,
             (unsigned)MESHPAY_HAL_LILYGO_H752_HEIGHT);
    epd_init();
    (void)init_backlight(driver);
    (void)init_i2c_touch(driver);

    epd_poweron();
    vTaskDelay(pdMS_TO_TICKS(20));
#if CONFIG_MESHPAY_LILYGO_H752_TPS65185
    (void)init_tps65185();
#else
    ESP_LOGI(TAG, "H752 TPS65185 init skipped for original H752 profile");
#endif
    epd_clear_area_cycles(epd_full_screen(),
                          H752_CLEAR_CYCLES,
                          H752_CLEAR_CYCLE_TIME);
    epd_draw_grayscale_image(epd_full_screen(), driver->framebuffer);
    epd_poweroff();

    driver->dirty = false;
    driver->initialized = true;
    ESP_LOGI(TAG, "LilyGo H752 display/touch HAL ready");
    return ESP_OK;
}

static esp_err_t h752_display_flush(void *ctx,
                                    const void *pixels,
                                    uint16_t width,
                                    uint16_t height)
{
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver =
        (meshpay_hal_lilygo_t5s3_h752_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || pixels == NULL ||
        driver->framebuffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width != MESHPAY_HAL_LILYGO_H752_WIDTH ||
        height != MESHPAY_HAL_LILYGO_H752_HEIGHT) {
        return ESP_ERR_INVALID_SIZE;
    }

    SemaphoreHandle_t lock = (SemaphoreHandle_t)driver->lock;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const size_t pixel_count =
        (size_t)MESHPAY_HAL_LILYGO_H752_WIDTH *
        (size_t)MESHPAY_HAL_LILYGO_H752_HEIGHT;
    esp_err_t err = meshpay_hal_lilygo_h752_rgb565_to_epd4(
        (const uint16_t *)pixels,
        pixel_count,
        (uint8_t *)driver->framebuffer,
        MESHPAY_HAL_LILYGO_H752_FB_SIZE);
    if (err == ESP_OK) {
        driver->dirty = true;
        refresh_full(driver);
    }
    xSemaphoreGive(lock);
    return err;
}

static esp_err_t h752_touch_read(void *ctx, meshpay_touch_state_t *state)
{
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver =
        (meshpay_hal_lilygo_t5s3_h752_driver_t *)ctx;
    if (driver == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    if (!driver->touch_available) {
        return ESP_OK;
    }

    if (driver->touch_cst) {
        uint8_t head[7] = {0};
        esp_err_t err = i2c_read_reg16(driver->touch_addr,
                                       H752_CST_REG_STATUS,
                                       head,
                                       sizeof(head));
        if (err != ESP_OK || head[0] == 0xABU) {
            return ESP_OK;
        }

        const uint8_t count = head[5] & 0x0FU;
        if (count > 0U) {
            const uint16_t raw_x =
                (uint16_t)((head[2] << 4) | (head[3] & 0x0FU));
            const uint16_t raw_y =
                (uint16_t)((head[1] << 4) | ((head[3] >> 4) & 0x0FU));
            (void)meshpay_hal_lilygo_h752_transform_touch(raw_x, raw_y, state);
        }

        const uint8_t clear = 0xAB;
        (void)i2c_write_reg16(driver->touch_addr,
                              H752_CST_REG_STATUS,
                              &clear,
                              1);
        return ESP_OK;
    }

    uint8_t frame[MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN] = {0};
    esp_err_t err = i2c_read_reg16(driver->touch_addr,
                                   H752_GT911_REG_STATUS,
                                   frame,
                                   sizeof(frame));
    if (err != ESP_OK) {
        return ESP_OK;
    }

    err = meshpay_hal_lilygo_h752_gt911_decode(frame, sizeof(frame), state);
    if ((frame[0] & 0x80U) != 0U) {
        const uint8_t clear = 0;
        (void)i2c_write_reg16(driver->touch_addr,
                              H752_GT911_REG_STATUS,
                              &clear,
                              1);
    }
    return err == ESP_OK ? ESP_OK : err;
}

static const meshpay_hal_ops_t H752_OPS = {
    .display_init = h752_display_init,
    .display_flush = h752_display_flush,
    .touch_read = h752_touch_read,
    .battery_mv = h752_battery_mv,
    .battery_status = h752_battery_status,
};

esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_init(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver,
    meshpay_hal_t *hal)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return meshpay_hal_init(hal,
                            MESHPAY_BOARD_LILYGO_T5S3_H752,
                            &H752_OPS,
                            driver);
}

esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_deinit(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->framebuffer != NULL) {
        free(driver->framebuffer);
    }
    if (driver->lock != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t)driver->lock);
    }
    if (driver->adc_cali_handle != NULL) {
        if (driver->adc_cali_scheme == H752_ADC_CALI_SCHEME_CURVE) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            (void)adc_cali_delete_scheme_curve_fitting(
                (adc_cali_handle_t)driver->adc_cali_handle);
#endif
        } else if (driver->adc_cali_scheme == H752_ADC_CALI_SCHEME_LINE) {
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            (void)adc_cali_delete_scheme_line_fitting(
                (adc_cali_handle_t)driver->adc_cali_handle);
#endif
        }
    }
    if (driver->adc_handle != NULL) {
        (void)adc_oneshot_del_unit(
            (adc_oneshot_unit_handle_t)driver->adc_handle);
    }
    if (driver->initialized) {
        (void)i2c_driver_delete(I2C_NUM_0);
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#else

esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_init(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver,
    meshpay_hal_t *hal)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_deinit(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#endif
