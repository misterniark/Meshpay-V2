#include "meshpay/descriptor_sync.h"

#include "esp_check.h"
#include <string.h>

static const char *TAG = "descriptor_sync";

/* Écrit un entier 32 bits en big-endian (réseau). */
static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/* Relit un entier 32 bits big-endian. */
static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

/* Vrai ssi les `len` octets sont tous nuls (adresse invalide). */
static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

/*
 * Initialise l'en-tête commun d'un paquet DATA adressé à `destination`. Même
 * base que dag_sync : header type 1, paquet DATA, contexte NONE. Le type de
 * destination (SINGLE ciblé / PLAIN broadcast) est fixé par l'appelant.
 */
static void packet_base(rns_packet_t *packet,
                        const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    rns_packet_clear(packet);
    packet->header_type = RNS_PACKET_HEADER_TYPE_1;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    packet->context = RNS_PACKET_CONTEXT_NONE;
    memcpy(packet->destination_hash, destination, RNS_PACKET_ADDRESS_SIZE);
}

esp_err_t meshpay_descriptor_sync_build_request(
    uint32_t currency_id,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    if (source == NULL || packet == NULL ||
        bytes_zero(source, MESHPAY_TX_DESTINATION_HASH_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Diffusé : le membre ne sait pas encore quel pair détient la monnaie. Le
     * champ destination porte l'adresse de réponse (source), comme dag_sync. */
    packet_base(packet, source);
    packet->propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet->destination_type = RNS_DESTINATION_TYPE_PLAIN;

    packet->data[0] = MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST;
    put_u32(packet->data + 1, currency_id);
    memcpy(packet->data + 5, source, MESHPAY_TX_DESTINATION_HASH_SIZE);
    packet->data_len = MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE;
    return ESP_OK;
}

esp_err_t meshpay_descriptor_sync_parse_request(
    const rns_packet_t *packet,
    uint32_t *currency_id,
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (packet == NULL ||
        packet->data_len != MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE ||
        packet->data[0] != MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (currency_id != NULL) {
        *currency_id = get_u32(packet->data + 1);
    }
    if (source != NULL) {
        memcpy(source, packet->data + 5, MESHPAY_TX_DESTINATION_HASH_SIZE);
    }
    return ESP_OK;
}

esp_err_t meshpay_descriptor_sync_build_offer(
    const meshpay_currency_descriptor_signed_t *signed_desc,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    if (signed_desc == NULL || source == NULL || packet == NULL ||
        bytes_zero(source, MESHPAY_TX_DESTINATION_HASH_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Diffusé (PLAIN broadcast) : le descripteur est public et signé ; tout
     * membre en cours de rejointe le capte et filtre par l'ancre. destination
     * porte l'adresse de l'ÉMETTEUR (provenance), comme un SUMMARY dag_sync. */
    packet_base(packet, source);
    packet->propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet->destination_type = RNS_DESTINATION_TYPE_PLAIN;

    packet->data[0] = MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER;
    /* Encode le descripteur directement après l'octet de type. La borne de
     * sortie = espace restant dans data[] après cet octet. */
    size_t wire_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_currency_descriptor_encode(
                            signed_desc,
                            packet->data + 1,
                            sizeof(packet->data) - 1,
                            &wire_len),
                        TAG, "");
    packet->data_len = 1 + wire_len;
    return ESP_OK;
}

esp_err_t meshpay_descriptor_sync_parse_offer(
    const rns_packet_t *packet,
    meshpay_currency_descriptor_signed_t *out_signed)
{
    if (packet == NULL || out_signed == NULL ||
        packet->data_len < 1 ||
        packet->data[0] != MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Décode le wire du descripteur (tout ce qui suit l'octet de type). Le
     * décodeur valide entièrement le CBOR (map complète, buffer consommé). */
    return meshpay_currency_descriptor_decode(packet->data + 1,
                                              packet->data_len - 1,
                                              out_signed);
}
