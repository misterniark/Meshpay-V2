#include "meshpay/rns/rns_resource.h"

#include <string.h>

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)((value >> 8) & 0xff);
    out[1] = (uint8_t)(value & 0xff);
}

static uint16_t get_u16(const uint8_t *in)
{
    return ((uint16_t)in[0] << 8) | in[1];
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)((value >> 24) & 0xff);
    out[1] = (uint8_t)((value >> 16) & 0xff);
    out[2] = (uint8_t)((value >> 8) & 0xff);
    out[3] = (uint8_t)(value & 0xff);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           in[3];
}

static bool link_is_active(const rns_link_t *link)
{
    if (link == NULL || link->status != RNS_LINK_STATUS_ACTIVE) {
        return false;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        acc |= link->link_id[i];
    }
    return acc != 0;
}

esp_err_t rns_resource_create_packets(const rns_link_t *link,
                                      const uint8_t *data,
                                      size_t data_len,
                                      rns_packet_t *packets,
                                      size_t max_packets,
                                      size_t *packet_count)
{
    if (!link_is_active(link) || data == NULL || packets == NULL ||
        packet_count == NULL || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > RNS_RESOURCE_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t count = (data_len + RNS_RESOURCE_MAX_PAYLOAD_SIZE - 1) /
                   RNS_RESOURCE_MAX_PAYLOAD_SIZE;
    if (count == 0 || count > RNS_RESOURCE_MAX_FRAGMENTS || max_packets < count) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t checksum[RNS_RESOURCE_CHECKSUM_SIZE];
    esp_err_t err = rns_crypto_sha256(data, data_len, checksum);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < count; ++i) {
        size_t offset = i * RNS_RESOURCE_MAX_PAYLOAD_SIZE;
        size_t remaining = data_len - offset;
        size_t take = remaining > RNS_RESOURCE_MAX_PAYLOAD_SIZE
                          ? RNS_RESOURCE_MAX_PAYLOAD_SIZE
                          : remaining;

        rns_packet_clear(&packets[i]);
        packets[i].destination_type = RNS_DESTINATION_TYPE_LINK;
        packets[i].packet_type = RNS_PACKET_TYPE_DATA;
        packets[i].context = RNS_PACKET_CONTEXT_RESOURCE;
        memcpy(packets[i].destination_hash, link->link_id, RNS_DESTINATION_HASH_SIZE);

        size_t pos = 0;
        packets[i].data[pos++] = RNS_RESOURCE_VERSION;
        memcpy(packets[i].data + pos, checksum, RNS_RESOURCE_ID_SIZE);
        pos += RNS_RESOURCE_ID_SIZE;
        put_u16(packets[i].data + pos, (uint16_t)i);
        pos += 2;
        put_u16(packets[i].data + pos, (uint16_t)count);
        pos += 2;
        put_u32(packets[i].data + pos, (uint32_t)data_len);
        pos += 4;
        memcpy(packets[i].data + pos, checksum, RNS_RESOURCE_CHECKSUM_SIZE);
        pos += RNS_RESOURCE_CHECKSUM_SIZE;
        memcpy(packets[i].data + pos, data + offset, take);
        pos += take;
        packets[i].data_len = pos;
    }

    *packet_count = count;
    rns_crypto_secure_zero(checksum, sizeof(checksum));
    return ESP_OK;
}

esp_err_t rns_resource_decode_fragment(const rns_packet_t *packet,
                                       rns_resource_fragment_t *fragment)
{
    if (packet == NULL || fragment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->packet_type != RNS_PACKET_TYPE_DATA ||
        packet->context != RNS_PACKET_CONTEXT_RESOURCE ||
        packet->destination_type != RNS_DESTINATION_TYPE_LINK ||
        packet->data_len < RNS_RESOURCE_FRAGMENT_HEADER_SIZE ||
        packet->data[0] != RNS_RESOURCE_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(fragment, 0, sizeof(*fragment));
    size_t pos = 1;
    memcpy(fragment->resource_id, packet->data + pos, RNS_RESOURCE_ID_SIZE);
    pos += RNS_RESOURCE_ID_SIZE;
    fragment->index = get_u16(packet->data + pos);
    pos += 2;
    fragment->count = get_u16(packet->data + pos);
    pos += 2;
    fragment->total_len = get_u32(packet->data + pos);
    pos += 4;
    memcpy(fragment->checksum, packet->data + pos, RNS_RESOURCE_CHECKSUM_SIZE);
    pos += RNS_RESOURCE_CHECKSUM_SIZE;

    if (fragment->count == 0 ||
        fragment->count > RNS_RESOURCE_MAX_FRAGMENTS ||
        fragment->index >= fragment->count ||
        fragment->total_len == 0 ||
        fragment->total_len > RNS_RESOURCE_MAX_DATA_SIZE ||
        !rns_crypto_constant_equal(fragment->resource_id,
                                   fragment->checksum,
                                   RNS_RESOURCE_ID_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }

    fragment->payload_len = packet->data_len - pos;
    if (fragment->payload_len == 0 ||
        fragment->payload_len > RNS_RESOURCE_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(fragment->payload, packet->data + pos, fragment->payload_len);
    return ESP_OK;
}

void rns_resource_reassembler_init(rns_resource_reassembler_t *reassembler)
{
    if (reassembler == NULL) {
        return;
    }
    memset(reassembler, 0, sizeof(*reassembler));
}

static bool same_resource(const rns_resource_reassembler_t *reassembler,
                          const rns_resource_fragment_t *fragment)
{
    return reassembler->active &&
           reassembler->expected_count == fragment->count &&
           reassembler->total_len == fragment->total_len &&
           rns_crypto_constant_equal(reassembler->resource_id,
                                     fragment->resource_id,
                                     RNS_RESOURCE_ID_SIZE);
}

static void start_resource(rns_resource_reassembler_t *reassembler,
                           const rns_resource_fragment_t *fragment)
{
    memset(reassembler, 0, sizeof(*reassembler));
    reassembler->active = true;
    reassembler->expected_count = fragment->count;
    reassembler->total_len = fragment->total_len;
    memcpy(reassembler->resource_id, fragment->resource_id, RNS_RESOURCE_ID_SIZE);
    memcpy(reassembler->checksum, fragment->checksum, RNS_RESOURCE_CHECKSUM_SIZE);
}

esp_err_t rns_resource_reassembler_accept(rns_resource_reassembler_t *reassembler,
                                          const rns_packet_t *packet,
                                          uint8_t *data,
                                          size_t data_len,
                                          size_t *written,
                                          bool *complete)
{
    if (reassembler == NULL || packet == NULL || data == NULL || complete == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *complete = false;
    if (written != NULL) {
        *written = 0;
    }

    rns_resource_fragment_t fragment;
    esp_err_t err = rns_resource_decode_fragment(packet, &fragment);
    if (err != ESP_OK) {
        return err;
    }

    if (!same_resource(reassembler, &fragment)) {
        start_resource(reassembler, &fragment);
    }

    if (reassembler->received[fragment.index]) {
        if (reassembler->payload_lens[fragment.index] != fragment.payload_len ||
            memcmp(reassembler->payloads[fragment.index],
                   fragment.payload,
                   fragment.payload_len) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        memcpy(reassembler->payloads[fragment.index],
               fragment.payload,
               fragment.payload_len);
        reassembler->payload_lens[fragment.index] = fragment.payload_len;
        reassembler->received[fragment.index] = true;
        reassembler->received_count++;
    }

    if (reassembler->received_count != reassembler->expected_count) {
        return ESP_OK;
    }

    size_t total = 0;
    for (size_t i = 0; i < reassembler->expected_count; ++i) {
        total += reassembler->payload_lens[i];
    }
    if (total != reassembler->total_len || data_len < total) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    for (size_t i = 0; i < reassembler->expected_count; ++i) {
        memcpy(data + pos, reassembler->payloads[i], reassembler->payload_lens[i]);
        pos += reassembler->payload_lens[i];
    }

    uint8_t checksum[RNS_RESOURCE_CHECKSUM_SIZE];
    err = rns_crypto_sha256(data, total, checksum);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(checksum, sizeof(checksum));
        return err;
    }
    if (!rns_crypto_constant_equal(checksum,
                                   reassembler->checksum,
                                   RNS_RESOURCE_CHECKSUM_SIZE)) {
        rns_crypto_secure_zero(checksum, sizeof(checksum));
        return ESP_ERR_INVALID_STATE;
    }
    rns_crypto_secure_zero(checksum, sizeof(checksum));

    if (written != NULL) {
        *written = total;
    }
    *complete = true;
    return ESP_OK;
}

void rns_resource_reassembler_pool_init(rns_resource_reassembler_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    memset(pool, 0, sizeof(*pool));
    for (size_t i = 0; i < RNS_RESOURCE_REASSEMBLER_POOL_SIZE; ++i) {
        rns_resource_reassembler_init(&pool->slots[i]);
    }
}

esp_err_t rns_resource_reassembler_pool_accept(
    rns_resource_reassembler_pool_t *pool,
    const rns_packet_t *packet,
    uint8_t *data,
    size_t data_len,
    size_t *written,
    bool *complete)
{
    if (pool == NULL || packet == NULL || data == NULL || complete == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *complete = false;
    if (written != NULL) {
        *written = 0;
    }

    /* Decodage prealable du fragment pour router vers le bon slot (le decodage
     * sera refait par accept() ; c'est peu couteux et evite de dupliquer la
     * logique de routage dans accept()). */
    rns_resource_fragment_t fragment;
    esp_err_t err = rns_resource_decode_fragment(packet, &fragment);
    if (err != ESP_OK) {
        return err;
    }

    /* 1) Slot d'un resource deja en cours qui correspond. */
    size_t chosen = RNS_RESOURCE_REASSEMBLER_POOL_SIZE;
    for (size_t i = 0; i < RNS_RESOURCE_REASSEMBLER_POOL_SIZE; ++i) {
        if (same_resource(&pool->slots[i], &fragment)) {
            chosen = i;
            break;
        }
    }
    /* 2) Sinon un slot libre. */
    if (chosen == RNS_RESOURCE_REASSEMBLER_POOL_SIZE) {
        for (size_t i = 0; i < RNS_RESOURCE_REASSEMBLER_POOL_SIZE; ++i) {
            if (!pool->slots[i].active) {
                chosen = i;
                break;
            }
        }
    }
    /* 3) Sinon le slot le moins recemment utilise (son partiel est abandonne ;
     * accept() le reinitialisera via start_resource()). */
    if (chosen == RNS_RESOURCE_REASSEMBLER_POOL_SIZE) {
        chosen = 0;
        /* "moins recemment utilise" = plus grand age. Age = delta SIGNE par
         * rapport au tick courant -> robuste au wrap de l'uint32 (revue #3). */
        for (size_t i = 1; i < RNS_RESOURCE_REASSEMBLER_POOL_SIZE; ++i) {
            int32_t age_i = (int32_t)(pool->tick - pool->last_used[i]);
            int32_t age_chosen = (int32_t)(pool->tick - pool->last_used[chosen]);
            if (age_i > age_chosen) {
                chosen = i;
            }
        }
    }

    err = rns_resource_reassembler_accept(&pool->slots[chosen],
                                          packet,
                                          data,
                                          data_len,
                                          written,
                                          complete);
    pool->last_used[chosen] = ++pool->tick;

    /* Libere le slot UNIQUEMENT a la completion. NE PAS liberer sur erreur
     * (revue #2) : une erreur transitoire d'accept() (fragment duplique au
     * payload different sous corruption, taille incoherente) detruirait sinon
     * tout le partiel deja accumule d'un resource SAIN (memset), sapant la
     * convergence. Un slot eventuellement bloque (rare : exige un fragment
     * corrompu passant le CRC ESP-NOW) est recupere par l'eviction LRU des
     * qu'un autre resource a besoin d'un slot. */
    if (*complete) {
        rns_resource_reassembler_init(&pool->slots[chosen]);
    }
    return err;
}
