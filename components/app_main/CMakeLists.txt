idf_component_register(
    SRCS "app_main_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos payment_engine dag_sync ui device_hal storage dag_store rns_announce rns_crypto currency currency_descriptor descriptor_sync
)
