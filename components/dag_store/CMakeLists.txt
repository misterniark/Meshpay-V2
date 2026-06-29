idf_component_register(
    SRCS "dag_store.c" "dag_store_mock.c" "dag_store_partition.c"
    INCLUDE_DIRS "include"
    REQUIRES dag meshpay_tx rns_crypto esp_partition
)
