#include "meshpay/device_hal.h"

#include <string.h>

esp_err_t meshpay_hal_init(meshpay_hal_t *hal,
                           meshpay_board_t board,
                           const meshpay_hal_ops_t *ops,
                           void *ctx)
{
    if (hal == NULL || ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(hal, 0, sizeof(*hal));
    hal->board = board;
    hal->ops = ops;
    hal->ctx = ctx;
    return ESP_OK;
}

#define CALL_OP(hal, member, ...) \
    (((hal) == NULL || (hal)->ops == NULL || (hal)->ops->member == NULL) \
        ? ESP_ERR_INVALID_ARG \
        : (hal)->ops->member((hal)->ctx, __VA_ARGS__))

esp_err_t meshpay_hal_display_init(meshpay_hal_t *hal)
{
    if (hal == NULL || hal->ops == NULL || hal->ops->display_init == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return hal->ops->display_init(hal->ctx);
}

esp_err_t meshpay_hal_display_flush(meshpay_hal_t *hal, const void *pixels,
                                    uint16_t width, uint16_t height)
{
    return CALL_OP(hal, display_flush, pixels, width, height);
}

esp_err_t meshpay_hal_touch_read(meshpay_hal_t *hal,
                                 meshpay_touch_state_t *state)
{
    return CALL_OP(hal, touch_read, state);
}

esp_err_t meshpay_hal_storage_write(meshpay_hal_t *hal, const char *key,
                                    const void *data, size_t len)
{
    return CALL_OP(hal, storage_write, key, data, len);
}

esp_err_t meshpay_hal_storage_read(meshpay_hal_t *hal, const char *key,
                                   void *data, size_t size, size_t *len)
{
    return CALL_OP(hal, storage_read, key, data, size, len);
}

esp_err_t meshpay_hal_lora_send(meshpay_hal_t *hal,
                                const uint8_t *data, size_t len)
{
    return CALL_OP(hal, lora_send, data, len);
}

esp_err_t meshpay_hal_lora_recv(meshpay_hal_t *hal,
                                uint8_t *data, size_t size, size_t *len)
{
    return CALL_OP(hal, lora_recv, data, size, len);
}

esp_err_t meshpay_hal_espnow_send(meshpay_hal_t *hal,
                                  const uint8_t *data, size_t len)
{
    return CALL_OP(hal, espnow_send, data, len);
}

esp_err_t meshpay_hal_espnow_recv(meshpay_hal_t *hal,
                                  uint8_t *data, size_t size, size_t *len)
{
    return CALL_OP(hal, espnow_recv, data, size, len);
}

esp_err_t meshpay_hal_battery_mv(meshpay_hal_t *hal, uint16_t *mv)
{
    return CALL_OP(hal, battery_mv, mv);
}

/* Lecture d'une touche clavier I2C (T-Deck).
 * Retourne ESP_ERR_INVALID_ARG si out_ascii est NULL ou si l'op n'est pas câblée
 * (cartes sans clavier : keyboard_read = NULL dans la table d'ops). */
esp_err_t meshpay_hal_keyboard_read(meshpay_hal_t *hal, uint8_t *out_ascii)
{
    if (out_ascii == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return CALL_OP(hal, keyboard_read, out_ascii);
}

esp_err_t meshpay_hal_battery_status(meshpay_hal_t *hal,
                                     uint16_t *mv,
                                     uint8_t *percent)
{
    if (hal == NULL || hal->ops == NULL || mv == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hal->ops->battery_status != NULL) {
        return hal->ops->battery_status(hal->ctx, mv, percent);
    }
    if (hal->ops->battery_mv != NULL) {
        esp_err_t err = hal->ops->battery_mv(hal->ctx, mv);
        if (err == ESP_OK) {
            *percent = 0;
        }
        return err;
    }
    return ESP_ERR_INVALID_ARG;
}
