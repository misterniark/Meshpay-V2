#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_HAL_PACKET_MAX 512
#define MESHPAY_HAL_ESPNOW_PEER_SIZE 6
#define MESHPAY_HAL_ESPNOW_DEFAULT_CHANNEL 1
#define MESHPAY_HAL_ESPNOW_DEFAULT_QUEUE_LENGTH 32
#define MESHPAY_HAL_LORA_UART_DEFAULT_PORT 1
#define MESHPAY_HAL_LORA_UART_PIN_UNSET (-1)
#define MESHPAY_HAL_LORA_UART_DEFAULT_BAUD 9600
#define MESHPAY_HAL_LORA_UART_DEFAULT_RX_BUFFER 1024
#define MESHPAY_HAL_LORA_UART_DEFAULT_READ_TIMEOUT_MS 20
#define MESHPAY_HAL_LORA_UART_DEFAULT_TX_WAIT_MS 100
#define MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD 6
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPI_HOST 2
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_SCK 1
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MOSI 2
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MISO 10
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_NSS 4
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_RESET 5
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_BUSY 6
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_DIO1 7
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_RXEN 8
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_TXEN 9
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_AUX_CS (-1)
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_FREQUENCY_HZ 868100000UL
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPREADING_FACTOR 9
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_TCXO_CTRL_VOLTAGE 2
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_CALIBRATE_IMAGE true
#define MESHPAY_HAL_LORA_CORE1262_BW_125 0
#define MESHPAY_HAL_LORA_CORE1262_BW_250 1
#define MESHPAY_HAL_LORA_CORE1262_BW_500 2
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_BANDWIDTH \
    MESHPAY_HAL_LORA_CORE1262_BW_125
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_CODING_RATE 1
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_TX_POWER_DBM 14
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_QUEUE_LENGTH 4
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_READ_TIMEOUT_MS 20
#define MESHPAY_HAL_LORA_CORE1262_DEFAULT_TX_TIMEOUT_MS 8000
#define MESHPAY_HAL_LORA_CORE1262_MAX_PAYLOAD 255
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH 320
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT 172
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_NATIVE_SHORT 172
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_NATIVE_LONG 320
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_Y_OFFSET 34
#define MESHPAY_HAL_WAVESHARE_S3_TOUCH_DATA_LEN 14
#define MESHPAY_HAL_WAVESHARE_S3_RGB565_BYTES_PER_PIXEL 2
#define MESHPAY_HAL_LILYGO_H752_WIDTH 960
#define MESHPAY_HAL_LILYGO_H752_HEIGHT 540
#define MESHPAY_HAL_LILYGO_H752_FB_SIZE \
    ((MESHPAY_HAL_LILYGO_H752_WIDTH * MESHPAY_HAL_LILYGO_H752_HEIGHT) / 2)
#define MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN 9
#define MESHPAY_HAL_LILYGO_H752_TPS65185_ADDR 0x6B
#define MESHPAY_HAL_LILYGO_H752_TPS65185_VCOM_MV 2000

typedef enum {
    MESHPAY_BOARD_UNKNOWN = 0,
    MESHPAY_BOARD_CYD,
    MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
    MESHPAY_BOARD_LILYGO_T5S3_H752,
    MESHPAY_BOARD_LILYGO_TDECK,   /* LilyGo T-Deck / T-Deck Plus (carte fondateur) */
} meshpay_board_t;

typedef struct {
    bool pressed;
    int16_t x;
    int16_t y;
} meshpay_touch_state_t;

typedef struct {
    esp_err_t (*display_init)(void *ctx);
    esp_err_t (*display_flush)(void *ctx, const void *pixels,
                               uint16_t width, uint16_t height);
    esp_err_t (*touch_read)(void *ctx, meshpay_touch_state_t *state);
    esp_err_t (*storage_write)(void *ctx, const char *key,
                               const void *data, size_t len);
    esp_err_t (*storage_read)(void *ctx, const char *key,
                              void *data, size_t size, size_t *len);
    esp_err_t (*lora_send)(void *ctx, const uint8_t *data, size_t len);
    esp_err_t (*lora_recv)(void *ctx, uint8_t *data, size_t size, size_t *len);
    esp_err_t (*espnow_send)(void *ctx, const uint8_t *data, size_t len);
    esp_err_t (*espnow_recv)(void *ctx, uint8_t *data, size_t size, size_t *len);
    esp_err_t (*battery_mv)(void *ctx, uint16_t *mv);
    esp_err_t (*battery_status)(void *ctx, uint16_t *mv, uint8_t *percent);
} meshpay_hal_ops_t;

typedef struct {
    meshpay_board_t board;
    const meshpay_hal_ops_t *ops;
    void *ctx;
} meshpay_hal_t;

typedef struct {
    uint8_t peer[MESHPAY_HAL_ESPNOW_PEER_SIZE];
    uint8_t channel;
    uint8_t queue_length;
} meshpay_hal_espnow_config_t;

typedef struct {
    meshpay_hal_espnow_config_t config;
    QueueHandle_t rx_queue;
    bool initialized;
} meshpay_hal_espnow_driver_t;

typedef struct {
    int uart_port;
    int tx_io;
    int rx_io;
    int rts_io;
    int cts_io;
    uint32_t baud_rate;
    uint32_t read_timeout_ms;
    uint32_t tx_wait_ms;
    size_t rx_buffer_size;
} meshpay_hal_lora_uart_config_t;

typedef struct {
    meshpay_hal_lora_uart_config_t config;
    bool initialized;
} meshpay_hal_lora_uart_driver_t;

typedef struct {
    int spi_host;
    int pin_sck;
    int pin_mosi;
    int pin_miso;
    int pin_nss;
    int pin_reset;
    int pin_busy;
    int pin_dio1;
    int pin_rxen;
    int pin_txen;
    int pin_aux_cs;
    uint32_t frequency_hz;
    uint8_t spreading_factor;
    uint8_t tcxo_ctrl_voltage;
    bool calibrate_image;
    uint8_t bandwidth;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    uint8_t queue_length;
    uint32_t read_timeout_ms;
    uint32_t tx_timeout_ms;
} meshpay_hal_lora_core1262_config_t;

typedef struct meshpay_hal_lora_core1262_internal
    meshpay_hal_lora_core1262_internal_t;

typedef struct {
    meshpay_hal_lora_core1262_config_t config;
    meshpay_hal_lora_core1262_internal_t *internal;
    bool initialized;
} meshpay_hal_lora_core1262_driver_t;

typedef struct {
    void *spi_handle;
    void *i2c_bus_handle;
    void *i2c_dev_handle;
    bool initialized;
    bool touch_available;
} meshpay_hal_waveshare_s3_touch_driver_t;

typedef struct {
    void *framebuffer;
    void *lock;
    void *adc_handle;
    void *adc_cali_handle;
    int64_t next_refresh_us;
    uint8_t touch_addr;
    uint8_t adc_cali_scheme;
    bool initialized;
    bool touch_available;
    bool touch_cst;
    bool backlight_ready;
    bool adc_ready;
    bool adc_calibrated;
    bool dirty;
} meshpay_hal_lilygo_t5s3_h752_driver_t;

esp_err_t meshpay_hal_init(meshpay_hal_t *hal,
                           meshpay_board_t board,
                           const meshpay_hal_ops_t *ops,
                           void *ctx);
esp_err_t meshpay_hal_display_init(meshpay_hal_t *hal);
esp_err_t meshpay_hal_display_flush(meshpay_hal_t *hal, const void *pixels,
                                    uint16_t width, uint16_t height);
esp_err_t meshpay_hal_touch_read(meshpay_hal_t *hal,
                                 meshpay_touch_state_t *state);
esp_err_t meshpay_hal_storage_write(meshpay_hal_t *hal, const char *key,
                                    const void *data, size_t len);
esp_err_t meshpay_hal_storage_read(meshpay_hal_t *hal, const char *key,
                                   void *data, size_t size, size_t *len);
esp_err_t meshpay_hal_lora_send(meshpay_hal_t *hal,
                                const uint8_t *data, size_t len);
esp_err_t meshpay_hal_lora_recv(meshpay_hal_t *hal,
                                uint8_t *data, size_t size, size_t *len);
esp_err_t meshpay_hal_espnow_send(meshpay_hal_t *hal,
                                  const uint8_t *data, size_t len);
esp_err_t meshpay_hal_espnow_recv(meshpay_hal_t *hal,
                                  uint8_t *data, size_t size, size_t *len);
esp_err_t meshpay_hal_battery_mv(meshpay_hal_t *hal, uint16_t *mv);
esp_err_t meshpay_hal_battery_status(meshpay_hal_t *hal,
                                     uint16_t *mv,
                                     uint8_t *percent);

typedef struct {
    bool display_initialized;
    uint16_t last_width;
    uint16_t last_height;
    meshpay_touch_state_t touch;
    uint8_t storage[MESHPAY_HAL_PACKET_MAX];
    size_t storage_len;
    char storage_key[16];
    uint8_t lora_packet[MESHPAY_HAL_PACKET_MAX];
    size_t lora_len;
    uint8_t espnow_packet[MESHPAY_HAL_PACKET_MAX];
    size_t espnow_len;
    uint16_t battery_mv;
} meshpay_hal_mock_t;

void meshpay_hal_mock_init(meshpay_hal_mock_t *mock,
                           meshpay_hal_t *hal,
                           meshpay_board_t board);
void meshpay_hal_mock_queue_touch(meshpay_hal_mock_t *mock,
                                  meshpay_touch_state_t state);
void meshpay_hal_mock_queue_lora(meshpay_hal_mock_t *mock,
                                 const uint8_t *data, size_t len);
void meshpay_hal_mock_queue_espnow(meshpay_hal_mock_t *mock,
                                   const uint8_t *data, size_t len);

void meshpay_hal_espnow_default_config(meshpay_hal_espnow_config_t *config);
esp_err_t meshpay_hal_espnow_driver_init(
    meshpay_hal_espnow_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_espnow_config_t *config);
esp_err_t meshpay_hal_espnow_driver_deinit(
    meshpay_hal_espnow_driver_t *driver);

void meshpay_hal_lora_uart_default_config(
    meshpay_hal_lora_uart_config_t *config);
esp_err_t meshpay_hal_lora_uart_encode_frame(const uint8_t *payload,
                                             size_t payload_len,
                                             uint8_t *frame,
                                             size_t frame_size,
                                             size_t *frame_len);
esp_err_t meshpay_hal_lora_uart_decode_frame(const uint8_t *frame,
                                             size_t frame_len,
                                             uint8_t *payload,
                                             size_t payload_size,
                                             size_t *payload_len);
esp_err_t meshpay_hal_lora_uart_driver_init(
    meshpay_hal_lora_uart_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_lora_uart_config_t *config);
esp_err_t meshpay_hal_lora_uart_driver_deinit(
    meshpay_hal_lora_uart_driver_t *driver);

void meshpay_hal_lora_core1262_default_config(
    meshpay_hal_lora_core1262_config_t *config);
esp_err_t meshpay_hal_lora_core1262_validate_config(
    const meshpay_hal_lora_core1262_config_t *config);
esp_err_t meshpay_hal_lora_core1262_driver_init(
    meshpay_hal_lora_core1262_driver_t *driver,
    meshpay_hal_t *hal,
    meshpay_board_t board,
    const meshpay_hal_lora_core1262_config_t *config);
esp_err_t meshpay_hal_lora_core1262_driver_deinit(
    meshpay_hal_lora_core1262_driver_t *driver);

esp_err_t meshpay_hal_waveshare_s3_touch_window(uint16_t width,
                                                uint16_t height,
                                                uint8_t caset[4],
                                                uint8_t raset[4]);
esp_err_t meshpay_hal_waveshare_s3_touch_decode(
    const uint8_t *touch_data,
    size_t touch_data_len,
    meshpay_touch_state_t *state);
esp_err_t meshpay_hal_waveshare_s3_rgb565_to_be(const uint16_t *pixels,
                                                size_t pixel_count,
                                                uint8_t *out,
                                                size_t out_size);
esp_err_t meshpay_hal_waveshare_s3_touch_driver_init(
    meshpay_hal_waveshare_s3_touch_driver_t *driver,
    meshpay_hal_t *hal);
esp_err_t meshpay_hal_waveshare_s3_touch_driver_deinit(
    meshpay_hal_waveshare_s3_touch_driver_t *driver);

esp_err_t meshpay_hal_lilygo_h752_transform_touch(uint16_t raw_x,
                                                  uint16_t raw_y,
                                                  meshpay_touch_state_t *state);
uint16_t meshpay_hal_lilygo_h752_tps65185_vcom_code(uint16_t vcom_mv);
uint8_t meshpay_hal_lilygo_h752_rgb565_to_gray4(uint16_t rgb565);
esp_err_t meshpay_hal_lilygo_h752_rgb565_to_epd4(const uint16_t *pixels,
                                                 size_t pixel_count,
                                                 uint8_t *out,
                                                 size_t out_size);
esp_err_t meshpay_hal_lilygo_h752_gt911_decode(const uint8_t *frame,
                                               size_t frame_len,
                                               meshpay_touch_state_t *state);
esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_init(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver,
    meshpay_hal_t *hal);
esp_err_t meshpay_hal_lilygo_t5s3_h752_driver_deinit(
    meshpay_hal_lilygo_t5s3_h752_driver_t *driver);

#ifdef __cplusplus
}
#endif
