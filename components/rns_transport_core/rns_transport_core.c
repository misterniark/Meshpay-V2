#include "meshpay/rns/rns_transport_core.h"

#include <string.h>

void rns_transport_core_init(rns_transport_core_t *core)
{
    if (core == NULL) {
        return;
    }
    memset(core, 0, sizeof(*core));
}

esp_err_t rns_transport_core_set_callbacks(rns_transport_core_t *core,
                                           const rns_transport_callbacks_t *callbacks)
{
    if (core == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    core->callbacks = *callbacks;
    return ESP_OK;
}

static bool destination_hash_zero(const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE])
{
    uint8_t acc = 0;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        acc |= destination_hash[i];
    }
    return acc == 0;
}

esp_err_t rns_transport_core_add_local_destination(
    rns_transport_core_t *core,
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE])
{
    if (core == NULL || destination_hash == NULL ||
        destination_hash_zero(destination_hash)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < core->local_destination_count; ++i) {
        if (rns_destination_hash_equal(core->local_destinations[i], destination_hash)) {
            return ESP_OK;
        }
    }
    if (core->local_destination_count >= RNS_TRANSPORT_LOCAL_DESTINATIONS_MAX) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(core->local_destinations[core->local_destination_count],
           destination_hash,
           RNS_DESTINATION_HASH_SIZE);
    core->local_destination_count++;
    return ESP_OK;
}

static bool duplicate_seen(const rns_transport_core_t *core,
                           const uint8_t hash[RNS_CRYPTO_SHA256_SIZE])
{
    for (size_t i = 0; i < core->duplicate_count; ++i) {
        if (rns_crypto_constant_equal(core->duplicate_hashes[i],
                                      hash,
                                      RNS_CRYPTO_SHA256_SIZE)) {
            return true;
        }
    }
    return false;
}

static void duplicate_remember(rns_transport_core_t *core,
                               const uint8_t hash[RNS_CRYPTO_SHA256_SIZE])
{
    memcpy(core->duplicate_hashes[core->duplicate_next],
           hash,
           RNS_CRYPTO_SHA256_SIZE);
    core->duplicate_next = (core->duplicate_next + 1) % RNS_TRANSPORT_DUPLICATE_CACHE_SIZE;
    if (core->duplicate_count < RNS_TRANSPORT_DUPLICATE_CACHE_SIZE) {
        core->duplicate_count++;
    }
}

static bool is_local_destination(const rns_transport_core_t *core,
                                 const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE])
{
    for (size_t i = 0; i < core->local_destination_count; ++i) {
        if (rns_destination_hash_equal(core->local_destinations[i], destination_hash)) {
            return true;
        }
    }
    return false;
}

static esp_err_t update_path_from_announce(rns_transport_core_t *core,
                                           const rns_packet_t *packet,
                                           const uint8_t packet_hash[RNS_CRYPTO_SHA256_SIZE])
{
    if (packet->packet_type != RNS_PACKET_TYPE_ANNOUNCE) {
        return ESP_OK;
    }

    rns_announce_t announce;
    esp_err_t err = rns_announce_verify_and_remember(packet, &announce);
    if (err != ESP_OK) {
        return err;
    }

    size_t slot = RNS_TRANSPORT_PATH_TABLE_SIZE;
    for (size_t i = 0; i < RNS_TRANSPORT_PATH_TABLE_SIZE; ++i) {
        if (core->paths[i].in_use &&
            rns_destination_hash_equal(core->paths[i].destination_hash,
                                       packet->destination_hash)) {
            slot = i;
            break;
        }
        if (!core->paths[i].in_use && slot == RNS_TRANSPORT_PATH_TABLE_SIZE) {
            slot = i;
        }
    }
    if (slot == RNS_TRANSPORT_PATH_TABLE_SIZE) {
        return ESP_ERR_NO_MEM;
    }

    rns_transport_path_t *path = &core->paths[slot];
    memset(path, 0, sizeof(*path));
    path->in_use = true;
    memcpy(path->destination_hash, packet->destination_hash, RNS_DESTINATION_HASH_SIZE);
    if (packet->header_type == RNS_PACKET_HEADER_TYPE_2) {
        memcpy(path->via_transport_id, packet->transport_id, RNS_DESTINATION_HASH_SIZE);
    }
    memcpy(path->announce_packet_hash, packet_hash, RNS_CRYPTO_SHA256_SIZE);
    path->hops = packet->hops;
    return ESP_OK;
}

const rns_transport_path_t *rns_transport_core_find_path(
    const rns_transport_core_t *core,
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE])
{
    if (core == NULL || destination_hash == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < RNS_TRANSPORT_PATH_TABLE_SIZE; ++i) {
        if (core->paths[i].in_use &&
            rns_destination_hash_equal(core->paths[i].destination_hash,
                                       destination_hash)) {
            return &core->paths[i];
        }
    }
    return NULL;
}

esp_err_t rns_transport_core_receive(rns_transport_core_t *core,
                                     const rns_packet_t *packet,
                                     rns_transport_rx_result_t *result)
{
    if (core == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (result != NULL) {
        *result = RNS_TRANSPORT_RX_ACCEPTED;
    }

    uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_packet_hash(packet, hash);
    if (err != ESP_OK) {
        return err;
    }

    if (duplicate_seen(core, hash)) {
        if (result != NULL) {
            *result = RNS_TRANSPORT_RX_DUPLICATE_DROP;
        }
        return ESP_OK;
    }
    duplicate_remember(core, hash);

    err = update_path_from_announce(core, packet, hash);
    if (err != ESP_OK) {
        if (result != NULL) {
            *result = RNS_TRANSPORT_RX_DROPPED;
        }
        return err;
    }

    if (is_local_destination(core, packet->destination_hash)) {
        if (core->callbacks.local_rx != NULL) {
            err = core->callbacks.local_rx(packet, core->callbacks.ctx);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (result != NULL) {
            *result = RNS_TRANSPORT_RX_LOCAL_DELIVERED;
        }
        return ESP_OK;
    }

    if (packet->propagation_type == RNS_PACKET_PROPAGATION_BROADCAST &&
        core->callbacks.forward_tx != NULL) {
        rns_packet_t forwarded = *packet;
        err = rns_packet_increment_hops(&forwarded);
        if (err != ESP_OK) {
            if (result != NULL) {
                *result = RNS_TRANSPORT_RX_DROPPED;
            }
            return err;
        }
        err = core->callbacks.forward_tx(&forwarded, core->callbacks.ctx);
        if (err != ESP_OK) {
            return err;
        }
        if (result != NULL) {
            *result = RNS_TRANSPORT_RX_FORWARDED;
        }
        return ESP_OK;
    }

    return ESP_OK;
}
