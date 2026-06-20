#include "meshpay/device_hal.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include <string.h>

typedef struct {
    uint8_t data[MESHPAY_HAL_PACKET_MAX];
    size_t len;
    uint8_t src[MESHPAY_HAL_ESPNOW_PEER_SIZE];
} espnow_rx_frame_t;

static meshpay_hal_espnow_driver_t *s_espnow_driver;

void meshpay_hal_espnow_default_config(meshpay_hal_espnow_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    memset(config->peer, 0xff, sizeof(config->peer));
    config->channel = MESHPAY_HAL_ESPNOW_DEFAULT_CHANNEL;
    config->queue_length = MESHPAY_HAL_ESPNOW_DEFAULT_QUEUE_LENGTH;
}

static esp_err_t espnow_send(void *ctx, const uint8_t *data, size_t len)
{
    meshpay_hal_espnow_driver_t *driver =
        (meshpay_hal_espnow_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || data == NULL || len == 0 ||
        len > MESHPAY_HAL_PACKET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len >= ESP_NOW_MAX_DATA_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_now_send(driver->config.peer, data, len);
}

static esp_err_t espnow_recv(void *ctx,
                             uint8_t *data,
                             size_t size,
                             size_t *len)
{
    meshpay_hal_espnow_driver_t *driver =
        (meshpay_hal_espnow_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || data == NULL ||
        len == NULL || driver->rx_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    espnow_rx_frame_t frame;
    if (xQueueReceive(driver->rx_queue, &frame, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (size < frame.len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, frame.data, frame.len);
    *len = frame.len;
    return ESP_OK;
}

static const meshpay_hal_ops_t ESPNOW_OPS = {
    .espnow_send = espnow_send,
    .espnow_recv = espnow_recv,
};

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data,
                           int data_len)
{
    meshpay_hal_espnow_driver_t *driver = s_espnow_driver;
    if (driver == NULL || driver->rx_queue == NULL || data == NULL ||
        data_len <= 0 || data_len > MESHPAY_HAL_PACKET_MAX) {
        return;
    }

    espnow_rx_frame_t frame = {
        .len = (size_t)data_len,
    };
    memcpy(frame.data, data, frame.len);
    if (info != NULL && info->src_addr != NULL) {
        memcpy(frame.src, info->src_addr, sizeof(frame.src));
    }
    (void)xQueueSend(driver->rx_queue, &frame, 0);
}

static esp_err_t wifi_start_station(uint8_t channel)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),
                        "device_hal_espnow", "");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                        "device_hal_espnow", "");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "device_hal_espnow", "");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE),
                        "device_hal_espnow", "");
    return ESP_OK;
}

static esp_err_t espnow_add_peer(const meshpay_hal_espnow_config_t *config)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, config->peer, sizeof(peer.peer_addr));
    peer.channel = config->channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_is_peer_exist(config->peer)) {
        return ESP_OK;
    }
    esp_err_t err = esp_now_add_peer(&peer);
    return err == ESP_ERR_ESPNOW_EXIST ? ESP_OK : err;
}

esp_err_t meshpay_hal_espnow_driver_init(
    meshpay_hal_espnow_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_espnow_config_t *config)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_hal_espnow_config_t effective;
    if (config == NULL) {
        meshpay_hal_espnow_default_config(&effective);
    } else {
        effective = *config;
    }
    if (effective.queue_length == 0 || effective.channel == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(driver, 0, sizeof(*driver));
    driver->config = effective;
    driver->rx_queue = xQueueCreate(effective.queue_length,
                                    sizeof(espnow_rx_frame_t));
    if (driver->rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = wifi_start_station(effective.channel);
    if (err == ESP_OK) {
        err = esp_now_init();
    }
    if (err == ESP_OK) {
        err = esp_now_register_recv_cb(espnow_recv_cb);
    }
    if (err == ESP_OK) {
        err = espnow_add_peer(&effective);
    }
    if (err == ESP_OK) {
        driver->initialized = true;
        s_espnow_driver = driver;
        err = meshpay_hal_init(hal, board, &ESPNOW_OPS, driver);
    }
    if (err != ESP_OK) {
        (void)meshpay_hal_espnow_driver_deinit(driver);
    }
    return err;
}

esp_err_t meshpay_hal_espnow_driver_deinit(
    meshpay_hal_espnow_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_espnow_driver == driver) {
        s_espnow_driver = NULL;
        (void)esp_now_unregister_recv_cb();
        (void)esp_now_deinit();
        (void)esp_wifi_stop();
    }
    if (driver->rx_queue != NULL) {
        vQueueDelete(driver->rx_queue);
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}
