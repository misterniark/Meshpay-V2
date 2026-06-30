#include "meshpay/device_hal.h"
#include "unity.h"
#include <string.h>

TEST_CASE("device hal mock display touch and power contracts", "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_WAVESHARE_S3_TOUCH);
    TEST_ASSERT_EQUAL(MESHPAY_BOARD_WAVESHARE_S3_TOUCH, hal.board);

    uint16_t pixels[4] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_display_init(&hal));
    TEST_ASSERT_TRUE(mock.display_initialized);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_display_flush(&hal, pixels, 2, 2));
    TEST_ASSERT_EQUAL_UINT16(2, mock.last_width);
    TEST_ASSERT_EQUAL_UINT16(2, mock.last_height);

    meshpay_touch_state_t queued = {.pressed = true, .x = 12, .y = 34};
    meshpay_hal_mock_queue_touch(&mock, queued);
    meshpay_touch_state_t read = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_touch_read(&hal, &read));
    TEST_ASSERT_TRUE(read.pressed);
    TEST_ASSERT_EQUAL_INT16(12, read.x);
    TEST_ASSERT_EQUAL_INT16(34, read.y);

    uint16_t mv = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_battery_mv(&hal, &mv));
    TEST_ASSERT_EQUAL_UINT16(3700, mv);
    uint8_t percent = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_battery_status(&hal, &mv, &percent));
    TEST_ASSERT_EQUAL_UINT16(3700, mv);
    TEST_ASSERT_EQUAL_UINT8(72, percent);
}

TEST_CASE("device hal mock storage roundtrips blobs", "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_CYD);

    const uint8_t blob[] = {1, 2, 3, 4, 5};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_storage_write(&hal, "id",
                                                        blob, sizeof(blob)));

    uint8_t out[8] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_storage_read(&hal, "id",
                                                       out, sizeof(out),
                                                       &out_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(blob), out_len);
    TEST_ASSERT_EQUAL_MEMORY(blob, out, sizeof(blob));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_hal_storage_read(&hal, "missing",
                                               out, sizeof(out), &out_len));
}

TEST_CASE("device hal mock radio send receive and timeout", "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_LILYGO_T5S3_H752);

    const uint8_t lora[] = {0xaa, 0xbb, 0xcc};
    uint8_t out[8] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_lora_send(&hal, lora, sizeof(lora)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_lora_recv(&hal, out, sizeof(out),
                                                    &out_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(lora), out_len);
    TEST_ASSERT_EQUAL_MEMORY(lora, out, sizeof(lora));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      meshpay_hal_lora_recv(&hal, out, sizeof(out), &out_len));

    const uint8_t espnow[] = {0x01, 0x02};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_espnow_send(&hal, espnow,
                                                      sizeof(espnow)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_espnow_recv(&hal, out, sizeof(out),
                                                      &out_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(espnow), out_len);
    TEST_ASSERT_EQUAL_MEMORY(espnow, out, sizeof(espnow));
}

TEST_CASE("device hal espnow driver exposes safe default config", "[device_hal]")
{
    meshpay_hal_espnow_config_t config;
    meshpay_hal_espnow_default_config(&config);

    const uint8_t broadcast[MESHPAY_HAL_ESPNOW_PEER_SIZE] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    TEST_ASSERT_EQUAL_MEMORY(broadcast, config.peer, sizeof(broadcast));
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_HAL_ESPNOW_DEFAULT_CHANNEL,
                            config.channel);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_HAL_ESPNOW_DEFAULT_QUEUE_LENGTH,
                            config.queue_length);

    meshpay_hal_t hal;
    meshpay_hal_espnow_driver_t driver;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_espnow_driver_init(NULL,
                                                     &hal,
                                                     MESHPAY_BOARD_CYD,
                                                     &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_espnow_driver_init(&driver,
                                                     NULL,
                                                     MESHPAY_BOARD_CYD,
                                                     &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_espnow_driver_deinit(NULL));
}

TEST_CASE("device hal lora uart driver exposes safe default config", "[device_hal]")
{
    meshpay_hal_lora_uart_config_t config;
    meshpay_hal_lora_uart_default_config(&config);

    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_UART_DEFAULT_PORT,
                          config.uart_port);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_UART_PIN_UNSET, config.tx_io);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_UART_PIN_UNSET, config.rx_io);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_HAL_LORA_UART_DEFAULT_BAUD,
                             config.baud_rate);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_HAL_LORA_UART_DEFAULT_READ_TIMEOUT_MS,
                             config.read_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_HAL_LORA_UART_DEFAULT_TX_WAIT_MS,
                             config.tx_wait_ms);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_HAL_LORA_UART_DEFAULT_RX_BUFFER,
                             config.rx_buffer_size);

    meshpay_hal_t hal;
    meshpay_hal_lora_uart_driver_t driver;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_uart_driver_init(NULL,
                                                        &hal,
                                                        MESHPAY_BOARD_CYD,
                                                        &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_uart_driver_init(&driver,
                                                        NULL,
                                                        MESHPAY_BOARD_CYD,
                                                        &config));

    config.rx_buffer_size = MESHPAY_HAL_PACKET_MAX - 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_uart_driver_init(&driver,
                                                        &hal,
                                                        MESHPAY_BOARD_CYD,
                                                        &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_uart_driver_deinit(NULL));
}

TEST_CASE("device hal lora uart frames roundtrip and reject corruption",
          "[device_hal]")
{
    const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef, 0x42};
    uint8_t frame[MESHPAY_HAL_PACKET_MAX +
                  MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD] = {0};
    size_t frame_len = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lora_uart_encode_frame(payload,
                                                         sizeof(payload),
                                                         frame,
                                                         sizeof(frame),
                                                         &frame_len));
    TEST_ASSERT_EQUAL_UINT8(0x4d, frame[0]);
    TEST_ASSERT_EQUAL_UINT8(0x50, frame[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[2]);
    TEST_ASSERT_EQUAL_UINT8(sizeof(payload), frame[3]);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload) +
                                 MESHPAY_HAL_LORA_UART_FRAME_OVERHEAD,
                             frame_len);

    uint8_t decoded[sizeof(payload)] = {0};
    size_t decoded_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lora_uart_decode_frame(frame,
                                                         frame_len,
                                                         decoded,
                                                         sizeof(decoded),
                                                         &decoded_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), decoded_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, decoded, sizeof(payload));

    frame[frame_len - 1U] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_hal_lora_uart_decode_frame(frame,
                                                         frame_len,
                                                         decoded,
                                                         sizeof(decoded),
                                                         &decoded_len));
}

TEST_CASE("device hal core1262 exposes waveshare defaults and validates pins",
          "[device_hal]")
{
    meshpay_hal_lora_core1262_config_t config;
    meshpay_hal_lora_core1262_default_config(&config);

    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPI_HOST,
                          config.spi_host);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_SCK,
                          config.pin_sck);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MOSI,
                          config.pin_mosi);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_MISO,
                          config.pin_miso);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_DIO1,
                          config.pin_dio1);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_RXEN,
                          config.pin_rxen);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_TXEN,
                          config.pin_txen);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_PIN_AUX_CS,
                          config.pin_aux_cs);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_HAL_LORA_CORE1262_DEFAULT_FREQUENCY_HZ,
                             config.frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_HAL_LORA_CORE1262_DEFAULT_SPREADING_FACTOR,
                            config.spreading_factor);
    TEST_ASSERT_EQUAL_UINT8(
        MESHPAY_HAL_LORA_CORE1262_DEFAULT_TCXO_CTRL_VOLTAGE,
        config.tcxo_ctrl_voltage);
    TEST_ASSERT_EQUAL(MESHPAY_HAL_LORA_CORE1262_DEFAULT_CALIBRATE_IMAGE,
                      config.calibrate_image);
    TEST_ASSERT_EQUAL_INT(MESHPAY_HAL_LORA_CORE1262_DEFAULT_TX_POWER_DBM,
                          config.tx_power_dbm);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lora_core1262_validate_config(&config));

    config.pin_miso = config.pin_mosi;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_validate_config(&config));

    meshpay_hal_lora_core1262_default_config(&config);
    config.frequency_hz = 1000;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_validate_config(&config));

    meshpay_hal_lora_core1262_default_config(&config);
    config.pin_sck = 18;
    config.pin_mosi = 17;
    config.pin_miso = 8;
    config.pin_nss = 46;
    config.pin_reset = 43;
    config.pin_busy = 44;
    config.pin_dio1 = 3;
    config.pin_rxen = -1;
    config.pin_txen = -1;
    config.pin_aux_cs = 16;
    config.tcxo_ctrl_voltage = 4;
    config.calibrate_image = false;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lora_core1262_validate_config(&config));

    config.pin_txen = 9;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_validate_config(&config));

    meshpay_hal_lora_core1262_default_config(&config);
    config.tcxo_ctrl_voltage = 8;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_validate_config(&config));

    meshpay_hal_t hal;
    meshpay_hal_lora_core1262_driver_t driver;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_driver_init(
                          NULL,
                          &hal,
                          MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
                          &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_driver_init(
                          &driver,
                          NULL,
                          MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
                          &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lora_core1262_driver_deinit(NULL));
}

TEST_CASE("waveshare s3 touch display window applies panel offset",
          "[device_hal]")
{
    uint8_t caset[4] = {0};
    uint8_t raset[4] = {0};

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_waveshare_s3_touch_window(
                          MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH,
                          MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT,
                          caset,
                          raset));

    const uint8_t expected_caset[] = {0x00, 0x00, 0x01, 0x3f};
    const uint8_t expected_raset[] = {0x00, 0x22, 0x00, 0xcd};
    TEST_ASSERT_EQUAL_MEMORY(expected_caset, caset, sizeof(caset));
    TEST_ASSERT_EQUAL_MEMORY(expected_raset, raset, sizeof(raset));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_touch_window(321,
                                                            172,
                                                            caset,
                                                            raset));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_touch_window(320,
                                                            173,
                                                            caset,
                                                            raset));
}

TEST_CASE("waveshare s3 touch decodes axs5106l coordinates",
          "[device_hal]")
{
    uint8_t touch_data[MESHPAY_HAL_WAVESHARE_S3_TOUCH_DATA_LEN] = {0};
    touch_data[1] = 1;
    touch_data[2] = 0x00;
    touch_data[3] = 135;
    touch_data[4] = 0x00;
    touch_data[5] = 87;

    meshpay_touch_state_t state = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_waveshare_s3_touch_decode(
                          touch_data,
                          sizeof(touch_data),
                          &state));
    TEST_ASSERT_TRUE(state.pressed);
    TEST_ASSERT_EQUAL_INT16(87, state.x);
    TEST_ASSERT_EQUAL_INT16(135, state.y);

    touch_data[1] = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_waveshare_s3_touch_decode(
                          touch_data,
                          sizeof(touch_data),
                          &state));
    TEST_ASSERT_FALSE(state.pressed);

    touch_data[1] = 1;
    touch_data[2] = 0x0f;
    touch_data[3] = 0xff;
    touch_data[4] = 0x0f;
    touch_data[5] = 0xff;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_waveshare_s3_touch_decode(
                          touch_data,
                          sizeof(touch_data),
                          &state));
    TEST_ASSERT_TRUE(state.pressed);
    TEST_ASSERT_EQUAL_INT16(MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH - 1,
                            state.x);
    TEST_ASSERT_EQUAL_INT16(MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT - 1,
                            state.y);
}

TEST_CASE("waveshare s3 rgb565 conversion emits big endian bus bytes",
          "[device_hal]")
{
    const uint16_t pixels[] = {0x1234, 0xabcd, 0x00ff};
    uint8_t out[sizeof(pixels)] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_waveshare_s3_rgb565_to_be(
                          pixels,
                          3,
                          out,
                          sizeof(out)));

    const uint8_t expected[] = {0x12, 0x34, 0xab, 0xcd, 0x00, 0xff};
    TEST_ASSERT_EQUAL_MEMORY(expected, out, sizeof(expected));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_rgb565_to_be(
                          pixels,
                          3,
                          out,
                          sizeof(out) - 1U));
}

TEST_CASE("waveshare s3 touch driver rejects invalid handles",
          "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_waveshare_s3_touch_driver_t driver;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_touch_driver_init(NULL,
                                                                 &hal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_touch_driver_init(&driver,
                                                                 NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_waveshare_s3_touch_driver_deinit(NULL));
}

TEST_CASE("lilygo h752 touch transform swaps and clamps axes",
          "[device_hal]")
{
    meshpay_touch_state_t state = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lilygo_h752_transform_touch(135,
                                                              87,
                                                              &state));
    TEST_ASSERT_TRUE(state.pressed);
    TEST_ASSERT_EQUAL_INT16(87, state.x);
    TEST_ASSERT_EQUAL_INT16(405, state.y);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lilygo_h752_transform_touch(4095,
                                                              4095,
                                                              &state));
    TEST_ASSERT_TRUE(state.pressed);
    TEST_ASSERT_EQUAL_INT16(MESHPAY_HAL_LILYGO_H752_WIDTH - 1, state.x);
    TEST_ASSERT_EQUAL_INT16(0, state.y);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lilygo_h752_transform_touch(0, 0, NULL));
}

TEST_CASE("lilygo h752 gt911 frame decodes status and point",
          "[device_hal]")
{
    uint8_t frame[MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN] = {0};
    frame[0] = 0x81;
    frame[1] = 0x00;
    frame[2] = 135;
    frame[3] = 0;
    frame[4] = 87;
    frame[5] = 0;

    meshpay_touch_state_t state = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lilygo_h752_gt911_decode(frame,
                                                           sizeof(frame),
                                                           &state));
    TEST_ASSERT_TRUE(state.pressed);
    TEST_ASSERT_EQUAL_INT16(87, state.x);
    TEST_ASSERT_EQUAL_INT16(405, state.y);

    frame[0] = 0x00;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lilygo_h752_gt911_decode(frame,
                                                           sizeof(frame),
                                                           &state));
    TEST_ASSERT_FALSE(state.pressed);
}

/* Test Step 1 (TDD) : vérifie le décodeur GT911 brut, sans transformation d'axes.
 * Ce test doit d'abord provoquer une erreur de compilation (symbole absent),
 * puis passer au vert après implémentation de meshpay_hal_gt911_decode_raw. */
TEST_CASE("gt911 raw decode extracts pressed flag and coordinates", "[device_hal]")
{
    uint8_t frame[MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN] = {0};
    bool pressed = true;
    uint16_t x = 0, y = 0;

    /* status sans bit 0x80 -> pas de point. */
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_hal_gt911_decode_raw(frame, sizeof(frame), &pressed, &x, &y));
    TEST_ASSERT_FALSE(pressed);

    /* status valide (0x80 | count=1), point (0x0123, 0x0045). */
    frame[0] = 0x81;
    frame[2] = 0x23; frame[3] = 0x01;
    frame[4] = 0x45; frame[5] = 0x00;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_hal_gt911_decode_raw(frame, sizeof(frame), &pressed, &x, &y));
    TEST_ASSERT_TRUE(pressed);
    TEST_ASSERT_EQUAL_UINT16(0x0123, x);
    TEST_ASSERT_EQUAL_UINT16(0x0045, y);
}

TEST_CASE("lilygo h752 rgb565 packs into 4bpp epaper framebuffer",
          "[device_hal]")
{
    const uint16_t pixels[] = {0xffff, 0x0000, 0x0000, 0xffff};
    uint8_t out[2] = {0};

    TEST_ASSERT_EQUAL(0x0f,
                      meshpay_hal_lilygo_h752_rgb565_to_gray4(0xffff));
    TEST_ASSERT_EQUAL(0x00,
                      meshpay_hal_lilygo_h752_rgb565_to_gray4(0x0000));
    TEST_ASSERT_EQUAL(200,
                      meshpay_hal_lilygo_h752_tps65185_vcom_code(
                          MESHPAY_HAL_LILYGO_H752_TPS65185_VCOM_MV));

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_lilygo_h752_rgb565_to_epd4(
                          pixels,
                          4,
                          out,
                          sizeof(out)));
    const uint8_t expected[] = {0x0f, 0xf0};
    TEST_ASSERT_EQUAL_MEMORY(expected, out, sizeof(expected));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lilygo_h752_rgb565_to_epd4(
                          pixels,
                          4,
                          out,
                          sizeof(out) - 1U));
}

TEST_CASE("lilygo h752 driver rejects invalid handles",
          "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_lilygo_t5s3_h752_driver_t driver;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lilygo_t5s3_h752_driver_init(NULL,
                                                               &hal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lilygo_t5s3_h752_driver_init(&driver,
                                                               NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_lilygo_t5s3_h752_driver_deinit(NULL));
}

TEST_CASE("device hal mock keyboard read returns queued ascii", "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_LILYGO_TDECK);

    uint8_t ch = 0xFF;
    /* Sans touche en attente : 0 (aucune touche), ESP_OK. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8(0, ch);

    /* Une touche mise en file est restituée puis consommée. */
    meshpay_hal_mock_queue_keyboard(&mock, 'A');
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8('A', ch);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8(0, ch);

    /* Garde-fou argument. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, meshpay_hal_keyboard_read(&hal, NULL));
}
