idf_component_register(
    SRCS "rns_packet_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_identity rns_packet
)
