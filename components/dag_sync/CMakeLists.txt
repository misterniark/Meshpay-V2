idf_component_register(
    SRCS "dag_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES dag meshpay_tx rns_packet rns_request_response rns_resource
)
