#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_TRANSPORT_DUPLICATE_CACHE_SIZE 48
#define RNS_TRANSPORT_PATH_TABLE_SIZE 32
#define RNS_TRANSPORT_LOCAL_DESTINATIONS_MAX 8

typedef enum {
    RNS_TRANSPORT_RX_ACCEPTED = 0,
    RNS_TRANSPORT_RX_DUPLICATE_DROP,
    RNS_TRANSPORT_RX_LOCAL_DELIVERED,
    RNS_TRANSPORT_RX_FORWARDED,
    RNS_TRANSPORT_RX_DROPPED,
} rns_transport_rx_result_t;

typedef struct {
    bool in_use;
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t via_transport_id[RNS_DESTINATION_HASH_SIZE];
    uint8_t announce_packet_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t hops;
} rns_transport_path_t;

typedef esp_err_t (*rns_transport_packet_cb_t)(const rns_packet_t *packet,
                                               void *ctx);

typedef struct {
    rns_transport_packet_cb_t local_rx;
    rns_transport_packet_cb_t forward_tx;
    void *ctx;
} rns_transport_callbacks_t;

typedef struct {
    uint8_t duplicate_hashes[RNS_TRANSPORT_DUPLICATE_CACHE_SIZE][RNS_CRYPTO_SHA256_SIZE];
    size_t duplicate_count;
    size_t duplicate_next;

    uint8_t local_destinations[RNS_TRANSPORT_LOCAL_DESTINATIONS_MAX][RNS_DESTINATION_HASH_SIZE];
    size_t local_destination_count;

    rns_transport_path_t paths[RNS_TRANSPORT_PATH_TABLE_SIZE];
    rns_transport_callbacks_t callbacks;
} rns_transport_core_t;

void rns_transport_core_init(rns_transport_core_t *core);
esp_err_t rns_transport_core_set_callbacks(rns_transport_core_t *core,
                                           const rns_transport_callbacks_t *callbacks);
esp_err_t rns_transport_core_add_local_destination(
    rns_transport_core_t *core,
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE]);
esp_err_t rns_transport_core_receive(rns_transport_core_t *core,
                                     const rns_packet_t *packet,
                                     rns_transport_rx_result_t *result);
const rns_transport_path_t *rns_transport_core_find_path(
    const rns_transport_core_t *core,
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE]);

#ifdef __cplusplus
}
#endif
