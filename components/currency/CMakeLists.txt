idf_component_register(
    SRCS "currency.c"
    INCLUDE_DIRS "include"
    REQUIRES dag meshpay_tx rns_crypto rns_destination rns_identity currency_descriptor checkpoint
)
