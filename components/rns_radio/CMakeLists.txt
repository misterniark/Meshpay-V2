idf_component_register(
    SRCS "rns_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES device_hal rns_iface_espnow rns_iface_lora rns_node
)
