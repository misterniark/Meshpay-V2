idf_component_register(
    SRCS "descriptor_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES currency_descriptor rns_packet meshpay_tx
)
