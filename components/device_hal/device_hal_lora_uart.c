#include "meshpay/device_hal.h"

#include "driver/uart.h"
#include "esp_check.h"
#include <string.h>

#define LORA_UART_MAGIC_0 0x4d
#define LORA_UART_MAGIC_1 0x50
#define LORA_UART_HEADER_SIZE 4
#define LORA_UART_CRC_SIZE 2

void meshpay_hal_lora_uart_default_config(
    meshpay_hal_lora_uart_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->uart_port = MESHPAY_HAL_LORA_UART_DEFAULT_PORT;
    config->tx_io = MESHPAY_HAL_LORA_UART_PIN_UNSET;
    config->rx_io = MESHPAY_HAL_LORA_UART_PIN_UNSET;
    config->rts_io = UART_PIN_NO_CHANGE;
    config->cts_io = UART_PIN_NO_CHANGE;
    config->baud_rate = MESHPAY_HAL_LORA_UART_DEFAULT_BAUD;
    config->read_timeout_ms = MESHPAY_HAL_LORA_UART_DEFAULT_READ_TIMEOUT_MS;
    config->tx_wait_ms = MESHPAY_HAL_LORA_UART_DEFAULT_TX_WAIT_MS;
    config->rx_buffer_size = MESHPAY_HAL_LORA_UART_DEFAULT_RX_BUFFER;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0 ? (uint16_t)((crc << 1) ^ 0x1021U)
                                       : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t meshpay_hal_lora_uart_encode_frame(const uint8_t *payload,
                                             size_t payload_len,
                                             uint8_t *frame,
                                             size_t frame_size,
                                             size_t *frame_len)
{
    if (payload == NULL || frame == NULL || frame_len == NULL ||
        payload_len == 0 || payload_len > MESHPAY_HAL_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t required_len =
        payload_len + MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD;
    if (frame_size < required_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[0] = LORA_UART_MAGIC_0;
    frame[1] = LORA_UART_MAGIC_1;
    frame[2] = (uint8_t)(payload_len >> 8);
    frame[3] = (uint8_t)payload_len;
    memcpy(frame + LORA_UART_HEADER_SIZE, payload, payload_len);
    uint16_t crc = crc16_ccitt(payload, payload_len);
    frame[LORA_UART_HEADER_SIZE + payload_len] = (uint8_t)(crc >> 8);
    frame[LORA_UART_HEADER_SIZE + payload_len + 1U] = (uint8_t)crc;
    *frame_len = required_len;
    return ESP_OK;
}

esp_err_t meshpay_hal_lora_uart_decode_frame(const uint8_t *frame,
                                             size_t frame_len,
                                             uint8_t *payload,
                                             size_t payload_size,
                                             size_t *payload_len)
{
    if (frame == NULL || payload == NULL || payload_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_len < MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame[0] != LORA_UART_MAGIC_0 || frame[1] != LORA_UART_MAGIC_1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t decoded_len = ((size_t)frame[2] << 8) | frame[3];
    if (decoded_len == 0 || decoded_len > MESHPAY_HAL_PACKET_MAX ||
        frame_len != decoded_len + MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_size < decoded_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *frame_payload = frame + LORA_UART_HEADER_SIZE;
    const uint8_t *crc_bytes = frame_payload + decoded_len;
    const uint16_t expected_crc =
        ((uint16_t)crc_bytes[0] << 8) | (uint16_t)crc_bytes[1];
    if (crc16_ccitt(frame_payload, decoded_len) != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(payload, frame_payload, decoded_len);
    *payload_len = decoded_len;
    return ESP_OK;
}

static esp_err_t lora_uart_send(void *ctx, const uint8_t *data, size_t len)
{
    meshpay_hal_lora_uart_driver_t *driver =
        (meshpay_hal_lora_uart_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || data == NULL || len == 0 ||
        len > MESHPAY_HAL_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[MESHPAY_HAL_PACKET_MAX +
                  MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD];
    size_t frame_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_hal_lora_uart_encode_frame(data,
                                                           len,
                                                           frame,
                                                           sizeof(frame),
                                                           &frame_len),
                        "device_hal_lora_uart", "");

    int written = uart_write_bytes((uart_port_t)driver->config.uart_port,
                                   frame,
                                   frame_len);
    if (written < 0 || (size_t)written != frame_len) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(
        (uart_port_t)driver->config.uart_port,
        pdMS_TO_TICKS(driver->config.tx_wait_ms));
}

static esp_err_t read_byte(meshpay_hal_lora_uart_driver_t *driver,
                           uint8_t *byte)
{
    int read = uart_read_bytes((uart_port_t)driver->config.uart_port,
                               byte,
                               1,
                               pdMS_TO_TICKS(driver->config.read_timeout_ms));
    if (read == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return read == 1 ? ESP_OK : ESP_FAIL;
}

static esp_err_t lora_uart_recv(void *ctx,
                                uint8_t *data,
                                size_t size,
                                size_t *len)
{
    meshpay_hal_lora_uart_driver_t *driver =
        (meshpay_hal_lora_uart_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || data == NULL ||
        len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t byte = 0;
    esp_err_t err = ESP_OK;
    do {
        err = read_byte(driver, &byte);
    } while (err == ESP_OK && byte != LORA_UART_MAGIC_0);
    if (err != ESP_OK) {
        return err;
    }

    err = read_byte(driver, &byte);
    if (err != ESP_OK) {
        return err;
    }
    if (byte != LORA_UART_MAGIC_1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t len_hi = 0;
    uint8_t len_lo = 0;
    ESP_RETURN_ON_ERROR(read_byte(driver, &len_hi),
                        "device_hal_lora_uart", "");
    ESP_RETURN_ON_ERROR(read_byte(driver, &len_lo),
                        "device_hal_lora_uart", "");
    size_t payload_len = ((size_t)len_hi << 8) | len_lo;
    if (payload_len == 0 || payload_len > MESHPAY_HAL_PACKET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (size < payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int read = uart_read_bytes((uart_port_t)driver->config.uart_port,
                               data,
                               payload_len,
                               pdMS_TO_TICKS(driver->config.read_timeout_ms));
    if (read == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (read < 0 || (size_t)read != payload_len) {
        return ESP_FAIL;
    }

    uint8_t crc_bytes[LORA_UART_CRC_SIZE] = {0};
    read = uart_read_bytes((uart_port_t)driver->config.uart_port,
                           crc_bytes,
                           sizeof(crc_bytes),
                           pdMS_TO_TICKS(driver->config.read_timeout_ms));
    if (read == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (read < 0 || (size_t)read != sizeof(crc_bytes)) {
        return ESP_FAIL;
    }

    uint16_t expected_crc =
        ((uint16_t)crc_bytes[0] << 8) | (uint16_t)crc_bytes[1];
    if (crc16_ccitt(data, payload_len) != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    *len = payload_len;
    return ESP_OK;
}

static const meshpay_hal_ops_t LORA_UART_OPS = {
    .lora_send = lora_uart_send,
    .lora_recv = lora_uart_recv,
};

esp_err_t meshpay_hal_lora_uart_driver_init(
    meshpay_hal_lora_uart_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_lora_uart_config_t *config)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_hal_lora_uart_config_t effective;
    if (config == NULL) {
        meshpay_hal_lora_uart_default_config(&effective);
    } else {
        effective = *config;
    }
    if (effective.uart_port < 0 || effective.baud_rate == 0 ||
        effective.rx_buffer_size < MESHPAY_HAL_PACKET_MAX ||
        effective.read_timeout_ms == 0 || effective.tx_wait_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(driver, 0, sizeof(*driver));
    driver->config = effective;

    const uart_config_t uart_config = {
        .baud_rate = (int)effective.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install((uart_port_t)effective.uart_port,
                                        effective.rx_buffer_size,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (err == ESP_OK) {
        err = uart_param_config((uart_port_t)effective.uart_port,
                                &uart_config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin((uart_port_t)effective.uart_port,
                           effective.tx_io,
                           effective.rx_io,
                           effective.rts_io,
                           effective.cts_io);
    }
    if (err == ESP_OK) {
        driver->initialized = true;
        err = meshpay_hal_init(hal, board, &LORA_UART_OPS, driver);
    }
    if (err != ESP_OK) {
        (void)meshpay_hal_lora_uart_driver_deinit(driver);
    }
    return err;
}

esp_err_t meshpay_hal_lora_uart_driver_deinit(
    meshpay_hal_lora_uart_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->initialized) {
        (void)uart_driver_delete((uart_port_t)driver->config.uart_port);
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}
