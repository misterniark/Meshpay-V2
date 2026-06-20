#include "meshpay/rns/rns_packet.h"

#include <string.h>

void rns_packet_clear(rns_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }
    memset(packet, 0, sizeof(*packet));
}

uint8_t rns_packet_encode_header_byte(const rns_packet_t *packet)
{
    uint8_t header = 0;
    if (packet->header_type == RNS_PACKET_HEADER_TYPE_2) {
        header |= RNS_PACKET_HEADER_TYPE_MASK;
    }
    if (packet->context_flag) {
        header |= RNS_PACKET_HEADER_CONTEXT_FLAG_MASK;
    }
    if (packet->propagation_type == RNS_PACKET_PROPAGATION_TRANSPORT) {
        header |= RNS_PACKET_HEADER_PROPAGATION_MASK;
    }
    header |= ((uint8_t)packet->destination_type & 0x03u) << 2;
    header |= ((uint8_t)packet->packet_type & 0x03u);
    return header;
}

size_t rns_packet_packed_size(const rns_packet_t *packet)
{
    if (packet == NULL) {
        return 0;
    }

    size_t address_len = RNS_PACKET_ADDRESS_SIZE;
    if (packet->header_type == RNS_PACKET_HEADER_TYPE_2) {
        address_len += RNS_PACKET_ADDRESS_SIZE;
    }

    return RNS_PACKET_HEADER_SIZE + address_len + RNS_PACKET_CONTEXT_SIZE + packet->data_len;
}

static bool enum_values_valid(const rns_packet_t *packet)
{
    return (packet->header_type == RNS_PACKET_HEADER_TYPE_1 ||
            packet->header_type == RNS_PACKET_HEADER_TYPE_2) &&
           (packet->propagation_type == RNS_PACKET_PROPAGATION_BROADCAST ||
            packet->propagation_type == RNS_PACKET_PROPAGATION_TRANSPORT) &&
           packet->destination_type <= RNS_DESTINATION_TYPE_LINK &&
           packet->packet_type <= RNS_PACKET_TYPE_PROOF;
}

esp_err_t rns_packet_pack(const rns_packet_t *packet, uint8_t *out,
                          size_t out_len, size_t *written)
{
    if (packet == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enum_values_valid(packet)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->data_len > RNS_PACKET_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t needed = rns_packet_packed_size(packet);
    if (needed > RNS_PACKET_MTU || out_len < needed) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    out[pos++] = rns_packet_encode_header_byte(packet);
    out[pos++] = packet->hops;
    if (packet->header_type == RNS_PACKET_HEADER_TYPE_2) {
        memcpy(out + pos, packet->transport_id, RNS_PACKET_ADDRESS_SIZE);
        pos += RNS_PACKET_ADDRESS_SIZE;
    }
    memcpy(out + pos, packet->destination_hash, RNS_PACKET_ADDRESS_SIZE);
    pos += RNS_PACKET_ADDRESS_SIZE;
    out[pos++] = packet->context;
    if (packet->data_len > 0) {
        memcpy(out + pos, packet->data, packet->data_len);
        pos += packet->data_len;
    }

    if (written != NULL) {
        *written = pos;
    }
    return ESP_OK;
}

esp_err_t rns_packet_unpack(const uint8_t *wire, size_t wire_len,
                            rns_packet_t *out)
{
    if (wire == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wire_len > RNS_PACKET_MTU || wire_len < RNS_PACKET_HEADER_SIZE + RNS_PACKET_ADDRESS_SIZE + RNS_PACKET_CONTEXT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t header = wire[0];
    if ((header & RNS_PACKET_HEADER_IFAC_MASK) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    rns_packet_clear(out);
    out->header_type = (header & RNS_PACKET_HEADER_TYPE_MASK)
                           ? RNS_PACKET_HEADER_TYPE_2
                           : RNS_PACKET_HEADER_TYPE_1;
    out->context_flag = (header & RNS_PACKET_HEADER_CONTEXT_FLAG_MASK) != 0;
    out->propagation_type = (header & RNS_PACKET_HEADER_PROPAGATION_MASK)
                                ? RNS_PACKET_PROPAGATION_TRANSPORT
                                : RNS_PACKET_PROPAGATION_BROADCAST;
    out->destination_type = (rns_destination_type_t)((header & RNS_PACKET_HEADER_DESTINATION_MASK) >> 2);
    out->packet_type = (rns_packet_type_t)(header & RNS_PACKET_HEADER_PACKET_MASK);
    out->hops = wire[1];

    size_t needed = RNS_PACKET_HEADER_SIZE + RNS_PACKET_ADDRESS_SIZE + RNS_PACKET_CONTEXT_SIZE;
    if (out->header_type == RNS_PACKET_HEADER_TYPE_2) {
        needed += RNS_PACKET_ADDRESS_SIZE;
    }
    if (wire_len < needed) {
        rns_packet_clear(out);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = RNS_PACKET_HEADER_SIZE;
    if (out->header_type == RNS_PACKET_HEADER_TYPE_2) {
        memcpy(out->transport_id, wire + pos, RNS_PACKET_ADDRESS_SIZE);
        pos += RNS_PACKET_ADDRESS_SIZE;
    }
    memcpy(out->destination_hash, wire + pos, RNS_PACKET_ADDRESS_SIZE);
    pos += RNS_PACKET_ADDRESS_SIZE;
    out->context = wire[pos++];
    out->data_len = wire_len - pos;
    if (out->data_len > RNS_PACKET_MAX_DATA_SIZE) {
        rns_packet_clear(out);
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->data_len > 0) {
        memcpy(out->data, wire + pos, out->data_len);
    }
    return ESP_OK;
}

esp_err_t rns_packet_increment_hops(rns_packet_t *packet)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->hops == UINT8_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    packet->hops++;
    return ESP_OK;
}

esp_err_t rns_packet_hash(const rns_packet_t *packet,
                          uint8_t out[RNS_CRYPTO_SHA256_SIZE])
{
    if (packet == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[RNS_PACKET_MTU];
    size_t raw_len = 0;
    esp_err_t err = rns_packet_pack(packet, raw, sizeof(raw), &raw_len);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(raw, sizeof(raw));
        return err;
    }

    uint8_t hashable[RNS_PACKET_MTU];
    size_t pos = 0;
    hashable[pos++] = raw[0] & 0x0f;

    size_t raw_offset = RNS_PACKET_HEADER_SIZE;
    if (packet->header_type == RNS_PACKET_HEADER_TYPE_2) {
        raw_offset += RNS_PACKET_ADDRESS_SIZE;
    }
    if (raw_len < raw_offset) {
        rns_crypto_secure_zero(raw, sizeof(raw));
        rns_crypto_secure_zero(hashable, sizeof(hashable));
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(hashable + pos, raw + raw_offset, raw_len - raw_offset);
    pos += raw_len - raw_offset;
    err = rns_crypto_sha256(hashable, pos, out);

    rns_crypto_secure_zero(raw, sizeof(raw));
    rns_crypto_secure_zero(hashable, sizeof(hashable));
    return err;
}

esp_err_t rns_packet_truncated_hash(const rns_packet_t *packet,
                                    uint8_t out[RNS_DESTINATION_HASH_SIZE])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_packet_hash(packet, full_hash);
    if (err == ESP_OK) {
        memcpy(out, full_hash, RNS_DESTINATION_HASH_SIZE);
    }
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return err;
}
