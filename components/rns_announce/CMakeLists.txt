idf_component_register(
    SRCS "rns_announce.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_identity rns_destination rns_packet
)
