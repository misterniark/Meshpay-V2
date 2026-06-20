#include "meshpay/device_hal.h"

#include <string.h>

static esp_err_t mock_display_init(void *ctx)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    mock->display_initialized = true;
    return ESP_OK;
}

static esp_err_t mock_display_flush(void *ctx, const void *pixels,
                                    uint16_t width, uint16_t height)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    if (pixels == NULL || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mock->last_width = width;
    mock->last_height = height;
    return ESP_OK;
}

static esp_err_t mock_touch_read(void *ctx, meshpay_touch_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    *state = mock->touch;
    return ESP_OK;
}

static esp_err_t mock_storage_write(void *ctx, const char *key,
                                    const void *data, size_t len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    if (key == NULL || data == NULL || len > sizeof(mock->storage)) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(mock->storage_key, key, sizeof(mock->storage_key) - 1);
    memcpy(mock->storage, data, len);
    mock->storage_len = len;
    return ESP_OK;
}

static esp_err_t mock_storage_read(void *ctx, const char *key,
                                   void *data, size_t size, size_t *len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    if (key == NULL || data == NULL || len == NULL ||
        strncmp(mock->storage_key, key, sizeof(mock->storage_key)) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (size < mock->storage_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, mock->storage, mock->storage_len);
    *len = mock->storage_len;
    return ESP_OK;
}

static esp_err_t packet_send(uint8_t *slot, size_t *slot_len,
                             const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > MESHPAY_HAL_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(slot, data, len);
    *slot_len = len;
    return ESP_OK;
}

static esp_err_t packet_recv(uint8_t *slot, size_t *slot_len,
                             uint8_t *data, size_t size, size_t *len)
{
    if (data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*slot_len == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (size < *slot_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, slot, *slot_len);
    *len = *slot_len;
    *slot_len = 0;
    return ESP_OK;
}

static esp_err_t mock_lora_send(void *ctx, const uint8_t *data, size_t len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    return packet_send(mock->lora_packet, &mock->lora_len, data, len);
}

static esp_err_t mock_lora_recv(void *ctx, uint8_t *data, size_t size, size_t *len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    return packet_recv(mock->lora_packet, &mock->lora_len, data, size, len);
}

static esp_err_t mock_espnow_send(void *ctx, const uint8_t *data, size_t len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    return packet_send(mock->espnow_packet, &mock->espnow_len, data, len);
}

static esp_err_t mock_espnow_recv(void *ctx, uint8_t *data, size_t size, size_t *len)
{
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    return packet_recv(mock->espnow_packet, &mock->espnow_len, data, size, len);
}

static esp_err_t mock_battery_mv(void *ctx, uint16_t *mv)
{
    if (mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    *mv = mock->battery_mv;
    return ESP_OK;
}

static esp_err_t mock_battery_status(void *ctx, uint16_t *mv, uint8_t *percent)
{
    if (mv == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    *mv = mock->battery_mv;
    *percent = 72;
    return ESP_OK;
}

static const meshpay_hal_ops_t MOCK_OPS = {
    .display_init = mock_display_init,
    .display_flush = mock_display_flush,
    .touch_read = mock_touch_read,
    .storage_write = mock_storage_write,
    .storage_read = mock_storage_read,
    .lora_send = mock_lora_send,
    .lora_recv = mock_lora_recv,
    .espnow_send = mock_espnow_send,
    .espnow_recv = mock_espnow_recv,
    .battery_mv = mock_battery_mv,
    .battery_status = mock_battery_status,
};

void meshpay_hal_mock_init(meshpay_hal_mock_t *mock,
                           meshpay_hal_t *hal,
                           meshpay_board_t board)
{
    if (mock == NULL || hal == NULL) {
        return;
    }
    memset(mock, 0, sizeof(*mock));
    mock->battery_mv = 3700;
    (void)meshpay_hal_init(hal, board, &MOCK_OPS, mock);
}

void meshpay_hal_mock_queue_touch(meshpay_hal_mock_t *mock,
                                  meshpay_touch_state_t state)
{
    if (mock != NULL) {
        mock->touch = state;
    }
}

void meshpay_hal_mock_queue_lora(meshpay_hal_mock_t *mock,
                                 const uint8_t *data, size_t len)
{
    if (mock != NULL && data != NULL && len <= sizeof(mock->lora_packet)) {
        memcpy(mock->lora_packet, data, len);
        mock->lora_len = len;
    }
}

void meshpay_hal_mock_queue_espnow(meshpay_hal_mock_t *mock,
                                   const uint8_t *data, size_t len)
{
    if (mock != NULL && data != NULL && len <= sizeof(mock->espnow_packet)) {
        memcpy(mock->espnow_packet, data, len);
        mock->espnow_len = len;
    }
}
