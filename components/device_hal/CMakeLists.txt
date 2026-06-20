set(DEVICE_HAL_SRCS
        "device_hal.c"
        "device_hal_mock.c"
        "device_hal_espnow.c"
        "device_hal_lora_uart.c"
        "device_hal_lora_core1262.c"
        "sx126x_hal.c"
        "sx126x/sx126x.c"
        "sx126x/sx126x_driver_version.c"
        "device_hal_lilygo_t5s3_h752.c"
        "device_hal_waveshare_s3_touch.c"
)

set(DEVICE_HAL_INCLUDE_DIRS "include" "sx126x")
set(DEVICE_HAL_REQUIRES
        driver
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        freertos
)

if(CONFIG_IDF_TARGET_ESP32S3 AND CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752)
    list(APPEND DEVICE_HAL_SRCS
        "lilygo_epd47_h752/ed047tc1.c"
        "lilygo_epd47_h752/epd_driver.c"
        "lilygo_epd47_h752/i2s_data_bus.c"
        "lilygo_epd47_h752/rmt_pulse.c"
    )
    list(APPEND DEVICE_HAL_INCLUDE_DIRS "lilygo_epd47_h752")
    list(APPEND DEVICE_HAL_INCLUDE_DIRS
        "${IDF_PATH}/components/esp_lcd/include"
        "${IDF_PATH}/components/esp_lcd/interface"
    )
    list(APPEND DEVICE_HAL_REQUIRES esp_lcd)
endif()

idf_component_register(
    SRCS ${DEVICE_HAL_SRCS}
    INCLUDE_DIRS ${DEVICE_HAL_INCLUDE_DIRS}
    REQUIRES ${DEVICE_HAL_REQUIRES}
)

if(CONFIG_IDF_TARGET_ESP32S3 AND CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--whole-archive"
        idf::esp_lcd
        "-Wl,--no-whole-archive"
    )
endif()
