idf_component_register(
    SRCS "rns_node.c"
    INCLUDE_DIRS "include"
    REQUIRES rns_announce rns_transport_core
)
