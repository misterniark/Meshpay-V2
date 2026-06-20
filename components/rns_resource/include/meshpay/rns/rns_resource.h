#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_link_request.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_RESOURCE_VERSION 1
#define RNS_RESOURCE_ID_SIZE RNS_DESTINATION_HASH_SIZE
#define RNS_RESOURCE_CHECKSUM_SIZE RNS_CRYPTO_SHA256_SIZE
#define RNS_RESOURCE_FRAGMENT_HEADER_SIZE 57
#define RNS_RESOURCE_MAX_PAYLOAD_SIZE \
    (RNS_PACKET_MAX_DATA_SIZE - RNS_RESOURCE_FRAGMENT_HEADER_SIZE)
#define RNS_RESOURCE_MAX_FRAGMENTS 16
#define RNS_RESOURCE_MAX_DATA_SIZE \
    (RNS_RESOURCE_MAX_PAYLOAD_SIZE * RNS_RESOURCE_MAX_FRAGMENTS)

typedef struct {
    uint8_t resource_id[RNS_RESOURCE_ID_SIZE];
    uint8_t checksum[RNS_RESOURCE_CHECKSUM_SIZE];
    uint16_t index;
    uint16_t count;
    uint32_t total_len;
    uint8_t payload[RNS_RESOURCE_MAX_PAYLOAD_SIZE];
    size_t payload_len;
} rns_resource_fragment_t;

typedef struct {
    bool active;
    uint8_t resource_id[RNS_RESOURCE_ID_SIZE];
    uint8_t checksum[RNS_RESOURCE_CHECKSUM_SIZE];
    uint16_t expected_count;
    uint32_t total_len;
    bool received[RNS_RESOURCE_MAX_FRAGMENTS];
    uint8_t payloads[RNS_RESOURCE_MAX_FRAGMENTS][RNS_RESOURCE_MAX_PAYLOAD_SIZE];
    size_t payload_lens[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t received_count;
} rns_resource_reassembler_t;

esp_err_t rns_resource_create_packets(const rns_link_t *link,
                                      const uint8_t *data,
                                      size_t data_len,
                                      rns_packet_t *packets,
                                      size_t max_packets,
                                      size_t *packet_count);
esp_err_t rns_resource_decode_fragment(const rns_packet_t *packet,
                                       rns_resource_fragment_t *fragment);
void rns_resource_reassembler_init(rns_resource_reassembler_t *reassembler);
esp_err_t rns_resource_reassembler_accept(rns_resource_reassembler_t *reassembler,
                                          const rns_packet_t *packet,
                                          uint8_t *data,
                                          size_t data_len,
                                          size_t *written,
                                          bool *complete);

#ifdef __cplusplus
}
#endif
