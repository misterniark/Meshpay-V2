idf_component_register(
    SRCS "rns_resource.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_link_request rns_packet
)
