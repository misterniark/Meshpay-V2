idf_component_register(
    SRCS "payment_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES wallet currency dag meshpay_tx rns_announce rns_identity rns_packet rns_packet_crypto
)
