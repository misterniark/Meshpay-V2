idf_component_register(
    SRCS "dag_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES dag_sync meshpay_tx rns_crypto rns_iface_lora rns_packet rns_resource
)
