#include "meshpay/rns/rns_node.h"

#include <string.h>

static esp_err_t node_dispatch_local(rns_node_t *node,
                                     const rns_packet_t *packet)
{
    if (packet->packet_type == RNS_PACKET_TYPE_PROOF) {
        node->stats.proof_packets++;
        if (node->callbacks.proof != NULL) {
            return node->callbacks.proof(node, packet, node->callbacks.ctx);
        }
        return ESP_OK;
    }

    if (packet->packet_type == RNS_PACKET_TYPE_DATA &&
        packet->context == RNS_PACKET_CONTEXT_REQUEST) {
        node->stats.request_packets++;
        if (node->callbacks.request != NULL) {
            return node->callbacks.request(node, packet, node->callbacks.ctx);
        }
        return ESP_OK;
    }

    if (node->callbacks.rx != NULL) {
        return node->callbacks.rx(node, packet, node->callbacks.ctx);
    }
    return ESP_OK;
}

static esp_err_t transport_local_rx(const rns_packet_t *packet, void *ctx)
{
    rns_node_t *node = (rns_node_t *)ctx;
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    node->stats.local_deliveries++;
    return node_dispatch_local(node, packet);
}

static esp_err_t transport_forward_tx(const rns_packet_t *packet, void *ctx)
{
    rns_node_t *node = (rns_node_t *)ctx;
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    node->stats.forwarded_packets++;
    return rns_node_send_packet(node, packet);
}

esp_err_t rns_node_init(rns_node_t *node, const rns_identity_t *identity)
{
    if (node == NULL || identity == NULL || !identity->has_public) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(node, 0, sizeof(*node));
    memcpy(&node->identity, identity, sizeof(node->identity));
    esp_err_t err = rns_destination_create_meshpay_wallet(&node->identity,
                                                          &node->destination);
    if (err != ESP_OK) {
        memset(node, 0, sizeof(*node));
        return err;
    }

    rns_transport_core_init(&node->transport);
    const rns_transport_callbacks_t transport_callbacks = {
        .local_rx = transport_local_rx,
        .forward_tx = transport_forward_tx,
        .ctx = node,
    };
    err = rns_transport_core_set_callbacks(&node->transport,
                                           &transport_callbacks);
    if (err == ESP_OK) {
        err = rns_transport_core_add_local_destination(&node->transport,
                                                       node->destination.hash);
    }
    if (err != ESP_OK) {
        memset(node, 0, sizeof(*node));
    }
    return err;
}

esp_err_t rns_node_set_callbacks(rns_node_t *node,
                                 const rns_node_callbacks_t *callbacks)
{
    if (node == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    node->callbacks = *callbacks;
    return ESP_OK;
}

const rns_destination_t *rns_node_destination(const rns_node_t *node)
{
    return node == NULL ? NULL : &node->destination;
}

const rns_node_stats_t *rns_node_stats(const rns_node_t *node)
{
    return node == NULL ? NULL : &node->stats;
}

esp_err_t rns_node_send_packet(rns_node_t *node, const rns_packet_t *packet)
{
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (node->callbacks.tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = node->callbacks.tx(node, packet, node->callbacks.ctx);
    if (err == ESP_OK) {
        node->stats.tx_packets++;
    }
    return err;
}

esp_err_t rns_node_announce(rns_node_t *node,
                            const uint8_t *app_data,
                            size_t app_data_len)
{
    if (node == NULL || (app_data == NULL && app_data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE];
    esp_err_t err = rns_crypto_random(random_hash, sizeof(random_hash));
    if (err != ESP_OK) {
        return err;
    }

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.destination_type = node->destination.type;
    packet.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(packet.destination_hash, node->destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    err = rns_announce_encode(&node->destination,
                              &node->identity,
                              random_hash,
                              app_data,
                              app_data_len,
                              packet.data,
                              sizeof(packet.data),
                              &packet.data_len);
    rns_crypto_secure_zero(random_hash, sizeof(random_hash));
    if (err != ESP_OK) {
        return err;
    }
    return rns_node_send_packet(node, &packet);
}

esp_err_t rns_node_send(rns_node_t *node,
                        const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
                        const uint8_t *data,
                        size_t data_len)
{
    if (node == NULL || destination_hash == NULL ||
        (data == NULL && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > RNS_PACKET_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_DATA;
    packet.context = RNS_PACKET_CONTEXT_NONE;
    memcpy(packet.destination_hash, destination_hash, RNS_DESTINATION_HASH_SIZE);
    if (data_len > 0) {
        memcpy(packet.data, data, data_len);
    }
    packet.data_len = data_len;

    return rns_node_send_packet(node, &packet);
}

esp_err_t rns_node_receive_packet(rns_node_t *node,
                                  const rns_packet_t *packet,
                                  rns_transport_rx_result_t *result)
{
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_transport_rx_result_t local_result = RNS_TRANSPORT_RX_ACCEPTED;
    esp_err_t err = rns_transport_core_receive(&node->transport,
                                               packet,
                                               &local_result);
    if (result != NULL) {
        *result = local_result;
    }
    if (err != ESP_OK) {
        return err;
    }

    node->stats.rx_packets++;
    if (local_result == RNS_TRANSPORT_RX_DUPLICATE_DROP) {
        node->stats.duplicate_drops++;
    }
    if (local_result == RNS_TRANSPORT_RX_DUPLICATE_DROP &&
        packet->packet_type == RNS_PACKET_TYPE_DATA &&
        packet->context == RNS_PACKET_CONTEXT_REQUEST &&
        rns_destination_hash_equal(packet->destination_hash,
                                   node->destination.hash)) {
        node->stats.local_deliveries++;
        return node_dispatch_local(node, packet);
    }
    const bool plain_data = packet->packet_type == RNS_PACKET_TYPE_DATA &&
                            packet->destination_type == RNS_DESTINATION_TYPE_PLAIN;
    if ((packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE || plain_data) &&
        (plain_data || local_result != RNS_TRANSPORT_RX_DUPLICATE_DROP) &&
        local_result != RNS_TRANSPORT_RX_DROPPED &&
        local_result != RNS_TRANSPORT_RX_LOCAL_DELIVERED) {
        node->stats.local_deliveries++;
        return node_dispatch_local(node, packet);
    }
    return ESP_OK;
}

esp_err_t rns_node_poll(rns_node_t *node,
                        const uint8_t *wire,
                        size_t wire_len,
                        rns_transport_rx_result_t *result)
{
    if (node == NULL || wire == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_packet_t packet;
    esp_err_t err = rns_packet_unpack(wire, wire_len, &packet);
    if (err != ESP_OK) {
        return err;
    }
    return rns_node_receive_packet(node, &packet, result);
}
