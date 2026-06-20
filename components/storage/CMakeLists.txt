idf_component_register(
    SRCS "storage.c" "storage_mock.c" "storage_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash rns_crypto rns_identity
)
