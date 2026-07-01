cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../components/project_skeleton"
    "../components/rns_fixtures"
    "../components/rns_crypto"
    "../components/rns_identity"
    "../components/rns_destination"
    "../components/rns_packet"
    "../components/rns_packet_crypto"
    "../components/rns_announce"
    "../components/rns_transport_core"
    "../components/rns_iface_espnow"
    "../components/rns_iface_lora"
    "../components/rns_link_request"
    "../components/rns_request_response"
    "../components/rns_resource"
    "../components/rns_node"
    "../components/rns_radio"
    "../components/storage"
    "../components/meshpay_tx"
    "../components/currency_descriptor"
    "../components/descriptor_sync"
    "../components/dag"
    "../components/dag_store"
    "../components/currency"
    "../components/wallet"
    "../components/payment_engine"
    "../components/dag_sync"
    "../components/dag_monitor"
    "../components/device_hal"
    "../components/ui"
    "../components/app_main"
    "../components/hardware_smoke"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshpayv2_test_app)
