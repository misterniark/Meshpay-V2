#pragma once

#include "esp_err.h"
#include "meshpay/device_hal.h"
#include "meshpay/rns/rns_iface_espnow.h"
#include "meshpay/rns/rns_iface_lora.h"
#include "meshpay/rns/rns_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RNS_RADIO_BEARER_ESPNOW = 1u << 0,
    RNS_RADIO_BEARER_LORA = 1u << 1,
    RNS_RADIO_BEARER_ALL = RNS_RADIO_BEARER_ESPNOW | RNS_RADIO_BEARER_LORA,
} rns_radio_bearer_t;

typedef uint8_t (*rns_radio_bearer_selector_t)(const rns_packet_t *packet,
                                               void *ctx);

typedef struct {
    meshpay_hal_t *hal;
    uint8_t enabled_bearers;
    rns_radio_bearer_selector_t bearer_selector;
    void *bearer_selector_ctx;
    size_t espnow_frame_size;
    size_t lora_frame_size;
    rns_espnow_reassembler_t espnow_reassembler;
    rns_lora_reassembler_t lora_reassembler;
    uint32_t tx_packets;
    uint32_t tx_frames_espnow;
    uint32_t tx_frames_lora;
    uint32_t rx_packets;
    uint32_t rx_frames_espnow;
    uint32_t rx_frames_lora;
} rns_radio_t;

typedef struct {
    rns_radio_t *radio;
    rns_node_callbacks_t upper_callbacks;
} rns_radio_node_adapter_t;

esp_err_t rns_radio_init(rns_radio_t *radio,
                         meshpay_hal_t *hal,
                         uint8_t enabled_bearers);
esp_err_t rns_radio_set_bearer_selector(
    rns_radio_t *radio,
    rns_radio_bearer_selector_t selector,
    void *ctx);
esp_err_t rns_radio_send_packet_over(rns_radio_t *radio,
                                     const rns_packet_t *packet,
                                     uint8_t bearers);
esp_err_t rns_radio_send_packet(rns_radio_t *radio,
                                const rns_packet_t *packet);
esp_err_t rns_radio_receive_espnow_frame(rns_radio_t *radio,
                                         rns_node_t *node,
                                         const uint8_t *frame,
                                         size_t frame_len,
                                         rns_transport_rx_result_t *result);
esp_err_t rns_radio_receive_lora_frame(rns_radio_t *radio,
                                       rns_node_t *node,
                                       const uint8_t *frame,
                                       size_t frame_len,
                                       rns_transport_rx_result_t *result);
esp_err_t rns_radio_poll_hal(rns_radio_t *radio,
                             rns_node_t *node,
                             rns_transport_rx_result_t *result);
esp_err_t rns_radio_bind_node(rns_radio_node_adapter_t *adapter,
                              rns_radio_t *radio,
                              rns_node_t *node,
                              const rns_node_callbacks_t *upper_callbacks);

#ifdef __cplusplus
}
#endif
