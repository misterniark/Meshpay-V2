idf_component_register(
    SRCS "app_main_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos payment_engine dag_sync ui device_hal storage rns_announce rns_crypto
)
