idf_component_register(
    SRCS "rns_link_request.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_identity rns_destination rns_packet
)
