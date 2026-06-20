#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_transport_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_node rns_node_t;

typedef esp_err_t (*rns_node_packet_cb_t)(rns_node_t *node,
                                          const rns_packet_t *packet,
                                          void *ctx);

typedef struct {
    rns_node_packet_cb_t tx;
    rns_node_packet_cb_t rx;
    rns_node_packet_cb_t proof;
    rns_node_packet_cb_t request;
    void *ctx;
} rns_node_callbacks_t;

typedef struct {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t local_deliveries;
    uint32_t forwarded_packets;
    uint32_t duplicate_drops;
    uint32_t proof_packets;
    uint32_t request_packets;
} rns_node_stats_t;

struct rns_node {
    rns_identity_t identity;
    rns_destination_t destination;
    rns_transport_core_t transport;
    rns_node_callbacks_t callbacks;
    rns_node_stats_t stats;
};

esp_err_t rns_node_init(rns_node_t *node, const rns_identity_t *identity);
esp_err_t rns_node_set_callbacks(rns_node_t *node,
                                 const rns_node_callbacks_t *callbacks);
const rns_destination_t *rns_node_destination(const rns_node_t *node);
const rns_node_stats_t *rns_node_stats(const rns_node_t *node);

esp_err_t rns_node_announce(rns_node_t *node,
                            const uint8_t *app_data,
                            size_t app_data_len);
esp_err_t rns_node_send(rns_node_t *node,
                        const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
                        const uint8_t *data,
                        size_t data_len);
esp_err_t rns_node_send_packet(rns_node_t *node, const rns_packet_t *packet);
esp_err_t rns_node_poll(rns_node_t *node,
                        const uint8_t *wire,
                        size_t wire_len,
                        rns_transport_rx_result_t *result);
esp_err_t rns_node_receive_packet(rns_node_t *node,
                                  const rns_packet_t *packet,
                                  rns_transport_rx_result_t *result);

#ifdef __cplusplus
}
#endif
