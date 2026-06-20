idf_component_register(
    SRCS "rns_request_response.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_crypto rns_link_request rns_packet
)
