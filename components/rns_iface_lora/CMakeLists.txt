idf_component_register(
    SRCS "rns_iface_lora.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_packet
)
