#include "meshpay/dag_sync.h"

#include "esp_check.h"
#include "esp_log.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_request_response.h"
#include <stdlib.h>
#include <string.h>

#define MESHPAY_DAG_SYNC_REQUEST_PATH "/meshpay/dag/request"
#define MESHPAY_DAG_SYNC_REQUEST_TIMEOUT_MS 30000U

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *in)
{
    return ((uint16_t)in[0] << 8) | in[1];
}

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static void packet_base(rns_packet_t *packet,
                        const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    rns_packet_clear(packet);
    packet->header_type = RNS_PACKET_HEADER_TYPE_1;
    packet->propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet->destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    packet->context = RNS_PACKET_CONTEXT_NONE;
    memcpy(packet->destination_hash, destination, RNS_PACKET_ADDRESS_SIZE);
}

esp_err_t meshpay_dag_sync_build_summary(
    const meshpay_dag_t *dag,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    if (dag == NULL || source == NULL || packet == NULL ||
        bytes_zero(source, MESHPAY_TX_DESTINATION_HASH_SIZE) ||
        meshpay_dag_count(dag) > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    packet_base(packet, source);
    packet->destination_type = RNS_DESTINATION_TYPE_PLAIN;
    packet->data[0] = MESHPAY_DAG_SYNC_MSG_SUMMARY;
    put_u16(packet->data + 1, (uint16_t)meshpay_dag_count(dag));

    const meshpay_tx_t *tips[MESHPAY_DAG_SYNC_MAX_TIPS];
    size_t tip_count = 0;
    (void)meshpay_dag_get_tips(dag, tips, MESHPAY_DAG_SYNC_MAX_TIPS,
                               &tip_count, NULL);
    packet->data[3] = (uint8_t)tip_count;
    size_t pos = 4;
    for (size_t i = 0; i < tip_count; ++i) {
        memcpy(packet->data + pos, tips[i]->id, MESHPAY_TX_ID_SIZE);
        pos += MESHPAY_TX_ID_SIZE;
    }
    /* Digest (8 o du dag_digest) pour la detection de convergence cote pair :
     * deux DAG de meme contenu => meme digest => sync inutile. Independant de
     * l'ordre d'insertion (le digest trie les id). */
    uint8_t digest[RNS_CRYPTO_SHA256_SIZE];
    if (meshpay_dag_digest(dag, digest) == ESP_OK) {
        memcpy(packet->data + pos, digest, MESHPAY_DAG_SYNC_DIGEST_SIZE);
        pos += MESHPAY_DAG_SYNC_DIGEST_SIZE;
    }
    packet->data_len = pos;
    return ESP_OK;
}

esp_err_t meshpay_dag_sync_parse_summary(const rns_packet_t *packet,
                                         meshpay_dag_sync_summary_t *summary)
{
    if (packet == NULL || summary == NULL ||
        packet->data_len < 4 ||
        packet->data[0] != MESHPAY_DAG_SYNC_MSG_SUMMARY) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary, 0, sizeof(*summary));
    summary->tx_count = get_u16(packet->data + 1);
    summary->tip_count = packet->data[3];
    size_t base = 4U + (size_t)summary->tip_count * MESHPAY_TX_ID_SIZE;
    if (summary->tip_count > MESHPAY_DAG_SYNC_MAX_TIPS ||
        (packet->data_len != base &&
         packet->data_len != base + MESHPAY_DAG_SYNC_DIGEST_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t pos = 4;
    for (uint8_t i = 0; i < summary->tip_count; ++i) {
        memcpy(summary->tips[i], packet->data + pos, MESHPAY_TX_ID_SIZE);
        pos += MESHPAY_TX_ID_SIZE;
    }
    /* Digest optionnel (compat : ancien format sans digest). */
    if (packet->data_len == base + MESHPAY_DAG_SYNC_DIGEST_SIZE) {
        memcpy(summary->digest, packet->data + pos, MESHPAY_DAG_SYNC_DIGEST_SIZE);
        summary->has_digest = true;
    }
    return ESP_OK;
}

esp_err_t meshpay_dag_sync_build_request(
    const meshpay_dag_t *local_dag,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    return meshpay_dag_sync_build_request_from(local_dag, peer, NULL, packet);
}

esp_err_t meshpay_dag_sync_build_request_from(
    const meshpay_dag_t *local_dag,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    if (local_dag == NULL || peer == NULL || packet == NULL ||
        meshpay_dag_count(local_dag) > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return meshpay_dag_sync_build_request_from_count(
        (uint16_t)meshpay_dag_count(local_dag),
        peer,
        source,
        packet);
}

esp_err_t meshpay_dag_sync_build_request_from_count(
    uint16_t known_count,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet)
{
    if (peer == NULL || packet == NULL ||
        bytes_zero(peer, MESHPAY_TX_DESTINATION_HASH_SIZE) ||
        (source != NULL &&
         bytes_zero(source, MESHPAY_TX_DESTINATION_HASH_SIZE))) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t payload[MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE] = {0};
    payload[0] = MESHPAY_DAG_SYNC_MSG_REQUEST;
    put_u16(payload + 1, known_count);
    size_t payload_len = MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE;
    if (source != NULL) {
        memcpy(payload + payload_len,
               source,
               MESHPAY_TX_DESTINATION_HASH_SIZE);
        payload_len += MESHPAY_TX_DESTINATION_HASH_SIZE;
    }

    rns_link_t link;
    rns_link_clear(&link);
    link.status = RNS_LINK_STATUS_ACTIVE;
    link.mtu = RNS_PACKET_MTU;
    link.mode = RNS_LINK_MODE_AES256_CBC;
    memcpy(link.link_id, peer, RNS_DESTINATION_HASH_SIZE);

    rns_request_receipt_t receipt;
    esp_err_t err = rns_request_create(&link,
                                       MESHPAY_DAG_SYNC_REQUEST_PATH,
                                       payload,
                                       payload_len,
                                       0,
                                       MESHPAY_DAG_SYNC_REQUEST_TIMEOUT_MS,
                                       packet,
                                       &receipt);
    rns_crypto_secure_zero(&receipt, sizeof(receipt));
    return err;
}

static esp_err_t dag_request_payload(const rns_packet_t *packet,
                                     uint8_t *payload,
                                     size_t payload_size,
                                     size_t *payload_len)
{
    if (packet == NULL || payload == NULL || payload_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->data_len > 0 &&
        packet->data[0] == MESHPAY_DAG_SYNC_MSG_REQUEST) {
        if (packet->data_len > payload_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(payload, packet->data, packet->data_len);
        *payload_len = packet->data_len;
        return ESP_OK;
    }

    rns_request_t request;
    ESP_RETURN_ON_ERROR(rns_request_decode(packet, &request),
                        "dag_sync", "");

    uint8_t expected_path[RNS_REQUEST_PATH_HASH_SIZE];
    ESP_RETURN_ON_ERROR(rns_request_path_hash(MESHPAY_DAG_SYNC_REQUEST_PATH,
                                              expected_path),
                        "dag_sync", "");
    if (!rns_crypto_constant_equal(expected_path,
                                   request.path_hash,
                                   RNS_REQUEST_PATH_HASH_SIZE)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (request.data_len > payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(payload, request.data, request.data_len);
    *payload_len = request.data_len;
    return ESP_OK;
}

esp_err_t meshpay_dag_sync_request_known_count(const rns_packet_t *packet,
                                               uint16_t *known_count)
{
    if (packet == NULL || known_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE];
    size_t payload_len = 0;
    ESP_RETURN_ON_ERROR(dag_request_payload(packet,
                                            payload,
                                            sizeof(payload),
                                            &payload_len),
                        "dag_sync", "");
    if ((payload_len != MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE &&
         payload_len != MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE) ||
        payload[0] != MESHPAY_DAG_SYNC_MSG_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }

    *known_count = get_u16(payload + 1);
    return ESP_OK;
}

esp_err_t meshpay_dag_sync_request_source(
    const rns_packet_t *packet,
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    bool *has_source)
{
    if (packet == NULL || source == NULL || has_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE];
    size_t payload_len = 0;
    ESP_RETURN_ON_ERROR(dag_request_payload(packet,
                                            payload,
                                            sizeof(payload),
                                            &payload_len),
                        "dag_sync", "");
    if (payload_len == 0 || payload[0] != MESHPAY_DAG_SYNC_MSG_REQUEST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len == MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE) {
        memset(source, 0, MESHPAY_TX_DESTINATION_HASH_SIZE);
        *has_source = false;
        return ESP_OK;
    }
    if (payload_len != MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(source,
           payload + MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE,
           MESHPAY_TX_DESTINATION_HASH_SIZE);
    if (bytes_zero(source, MESHPAY_TX_DESTINATION_HASH_SIZE)) {
        *has_source = false;
        return ESP_ERR_INVALID_ARG;
    }
    *has_source = true;
    return ESP_OK;
}

static esp_err_t encode_batch(const meshpay_dag_t *source_dag,
                              uint16_t start_index,
                              uint8_t *batch,
                              size_t batch_size,
                              size_t *batch_len,
                              uint16_t *next_index)
{
    if (start_index > meshpay_dag_count(source_dag)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pos = 2;
    uint16_t count = 0;
    size_t i = start_index;
    for (; i < meshpay_dag_count(source_dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(source_dag, i);
        uint8_t encoded[MESHPAY_TX_CBOR_MAX_SIZE];
        size_t encoded_len = 0;
        ESP_RETURN_ON_ERROR(meshpay_tx_encode(tx, encoded, sizeof(encoded),
                                              &encoded_len),
                            "dag_sync", "");
        if (encoded_len > UINT16_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* Capacite du batch atteinte : on emet un chunk PARTIEL et on s'arrete.
         * Le reste sera transfere par des Resource suivantes (pagination), ce
         * qui leve le plafond ~29 tx/batch alors que la fenetre DAG est 250.
         * Au moins 1 tx par chunk (une tx unique > capacite est impossible :
         * CBOR <= 320 o << batch). */
        if (pos + 2U + encoded_len > batch_size) {
            if (count == 0) {
                return ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        put_u16(batch + pos, (uint16_t)encoded_len);
        pos += 2;
        memcpy(batch + pos, encoded, encoded_len);
        pos += encoded_len;
        count++;
    }

    put_u16(batch, count);
    *batch_len = pos;
    if (next_index != NULL) {
        *next_index = (uint16_t)i; /* 1re tx non encore envoyee (= count si fin) */
    }
    return count == 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t meshpay_dag_sync_build_batch_resource_from(
    const meshpay_dag_t *source_dag,
    uint16_t start_index,
    const rns_link_t *link,
    rns_packet_t *packets,
    size_t max_packets,
    size_t *packet_count,
    uint16_t *next_index)
{
    if (source_dag == NULL || link == NULL || packets == NULL ||
        packet_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *batch = malloc(MESHPAY_DAG_SYNC_BATCH_MAX_SIZE);
    if (batch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t batch_len = 0;
    esp_err_t err = encode_batch(source_dag,
                                 start_index,
                                 batch,
                                 MESHPAY_DAG_SYNC_BATCH_MAX_SIZE,
                                 &batch_len,
                                 next_index);
    if (err == ESP_OK) {
        err = rns_resource_create_packets(link,
                                          batch,
                                          batch_len,
                                          packets,
                                          max_packets,
                                          packet_count);
    }
    free(batch);
    return err;
}

esp_err_t meshpay_dag_sync_build_batch_resource(
    const meshpay_dag_t *source_dag,
    uint16_t start_index,
    const rns_link_t *link,
    rns_packet_t *packets,
    size_t max_packets,
    size_t *packet_count)
{
    return meshpay_dag_sync_build_batch_resource_from(source_dag,
                                                      start_index,
                                                      link,
                                                      packets,
                                                      max_packets,
                                                      packet_count,
                                                      NULL);
}

esp_err_t meshpay_dag_sync_apply_batch(meshpay_dag_t *target_dag,
                                       const uint8_t *batch,
                                       size_t batch_len,
                                       size_t *merged_count)
{
    if (target_dag == NULL || batch == NULL || batch_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (merged_count != NULL) {
        *merged_count = 0;
    }

    uint16_t count = get_u16(batch);
    size_t merged = 0;

    /* Application MULTI-PASSES. Un batch peut contenir des tx dans un ordre non
     * topologique du point de vue du recepteur (emission concurrente, branches
     * de fork) : une tx enfant peut preceder son parent, ou un parent peut etre
     * une tx d'une autre branche situee plus loin dans le batch. Appliquer en une
     * seule passe et abandonner au premier MISSING_PARENT (ancien comportement)
     * bloquait toute la reconciliation (0 tx integree, boucle infinie de re-sync).
     *
     * On reboucle donc tant qu'une passe parvient a integrer au moins une tx, en
     * re-parsant le batch a chaque passe (evite un gros tampon de tx sur la pile).
     * Au pire 'count' passes (1 tx applicable de plus par passe). Semantique des
     * resultats de merge :
     *   - OK            : nouvellement integree => progres.
     *   - DUPLICATE     : deja presente => non bloquant, pas un progres.
     *   - MISSING_PARENT/FULL : non fatals => retentes a la passe suivante (le
     *     parent peut avoir ete integre entre-temps). Les MISSING_PARENT residuels
     *     apres stabilisation sont laisses (seront combles par un futur batch).
     *   - CONFLICT/INVALID : tx illegitime (double-depense, forme invalide) =>
     *     fatal, on rejette tout le batch comme avant. */
    bool progress = true;
    for (uint16_t pass = 0; pass < count && progress; ++pass) {
        progress = false;
        size_t pos = 2;
        for (uint16_t i = 0; i < count; ++i) {
            if (pos + 2U > batch_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t encoded_len = get_u16(batch + pos);
            pos += 2;
            if (encoded_len == 0 || pos + encoded_len > batch_len) {
                return ESP_ERR_INVALID_SIZE;
            }

            meshpay_tx_t tx;
            ESP_RETURN_ON_ERROR(meshpay_tx_decode(batch + pos, encoded_len, &tx),
                                "dag_sync", "");
            pos += encoded_len;

            meshpay_dag_merge_result_t result =
                meshpay_dag_merge_tx(target_dag, &tx);
            if (result == MESHPAY_DAG_MERGE_OK) {
                merged++;
                progress = true;
            } else if (result == MESHPAY_DAG_MERGE_CONFLICT ||
                       result == MESHPAY_DAG_MERGE_INVALID) {
                /* Niveau DEBUG (et non WARN) : ce log est un outil de diagnostic
                 * de la reconciliation, pas un evenement de prod. Un pair distant
                 * peut emettre a volonte des batches en conflit ; logguer en WARN
                 * a chaque tx fautive serait un vecteur de spam de logs via radio.
                 * Compile hors des builds par defaut (LOG niveau INFO). */
                ESP_LOGD("dag_sync",
                         "apply_batch fatal result=%d seq=%u from=%02x%02x%02x%02x",
                         (int)result, (unsigned)tx.seq,
                         tx.from[0], tx.from[1], tx.from[2], tx.from[3]);
                return ESP_ERR_INVALID_STATE;
            }
            /* DUPLICATE / MISSING_PARENT / FULL : non fatals (cf. ci-dessus). */
        }
        /* Validation de format faite une fois, sur la passe complete initiale :
         * tout le batch doit etre consomme exactement (pas d'octets en trop). */
        if (pass == 0 && pos != batch_len) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (merged_count != NULL) {
        *merged_count = merged;
    }
    return ESP_OK;
}
