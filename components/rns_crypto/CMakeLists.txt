idf_component_register(
    SRCS
        "rns_crypto.c"
        "vendor/monocypher/monocypher.c"
        "vendor/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/monocypher"
    REQUIRES mbedtls esp_system
)

