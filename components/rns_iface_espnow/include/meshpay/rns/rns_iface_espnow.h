#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF esp_now_send() requires frames smaller than the 250 byte v1 cap. */
#define RNS_ESPNOW_DEFAULT_FRAME_SIZE 249
#define RNS_ESPNOW_MAX_FRAME_SIZE 249
#define RNS_ESPNOW_FRAGMENT_HEADER_SIZE 23
#define RNS_ESPNOW_MESSAGE_ID_SIZE 16
#define RNS_ESPNOW_MAX_FRAGMENTS 16
#define RNS_ESPNOW_MAX_FRAGMENT_PAYLOAD \
    (RNS_ESPNOW_MAX_FRAME_SIZE - RNS_ESPNOW_FRAGMENT_HEADER_SIZE)

typedef struct {
    uint8_t message_id[RNS_ESPNOW_MESSAGE_ID_SIZE];
    uint8_t index;
    uint8_t count;
    uint8_t payload[RNS_ESPNOW_MAX_FRAGMENT_PAYLOAD];
    size_t payload_len;
} rns_espnow_fragment_t;

typedef struct {
    bool active;
    uint8_t message_id[RNS_ESPNOW_MESSAGE_ID_SIZE];
    uint8_t expected_count;
    bool received[RNS_ESPNOW_MAX_FRAGMENTS];
    uint8_t payloads[RNS_ESPNOW_MAX_FRAGMENTS][RNS_ESPNOW_MAX_FRAGMENT_PAYLOAD];
    size_t payload_lens[RNS_ESPNOW_MAX_FRAGMENTS];
    size_t received_count;
} rns_espnow_reassembler_t;

esp_err_t rns_iface_espnow_fragment_packet(const uint8_t *packet,
                                           size_t packet_len,
                                           size_t frame_size,
                                           rns_espnow_fragment_t *fragments,
                                           size_t max_fragments,
                                           size_t *fragment_count);
esp_err_t rns_iface_espnow_pack_fragment(const rns_espnow_fragment_t *fragment,
                                         uint8_t *frame,
                                         size_t frame_len,
                                         size_t *written);
esp_err_t rns_iface_espnow_unpack_fragment(const uint8_t *frame,
                                           size_t frame_len,
                                           rns_espnow_fragment_t *fragment);
void rns_iface_espnow_reassembler_init(rns_espnow_reassembler_t *reassembler);
esp_err_t rns_iface_espnow_reassembler_accept(rns_espnow_reassembler_t *reassembler,
                                              const uint8_t *frame,
                                              size_t frame_len,
                                              uint8_t *packet,
                                              size_t packet_len,
                                              size_t *written,
                                              bool *complete);

#ifdef __cplusplus
}
#endif
