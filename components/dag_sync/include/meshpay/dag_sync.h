#pragma once

#include "esp_err.h"
#include "meshpay/dag.h"
#include "meshpay/rns/rns_resource.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_DAG_SYNC_MSG_SUMMARY 0x31
#define MESHPAY_DAG_SYNC_MSG_REQUEST 0x32
#define MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE 3
#define MESHPAY_DAG_SYNC_REQUEST_WITH_SOURCE_SIZE \
    (MESHPAY_DAG_SYNC_REQUEST_MIN_SIZE + MESHPAY_TX_DESTINATION_HASH_SIZE)
#define MESHPAY_DAG_SYNC_MAX_TIPS 2
#define MESHPAY_DAG_SYNC_DIGEST_SIZE 8 /* 8 premiers octets du dag_digest */
#define MESHPAY_DAG_SYNC_BATCH_MAX_SIZE RNS_RESOURCE_MAX_DATA_SIZE
/* Nb max de chunks (Resource) emis pour UNE requete : borne la rafale radio
 * quand la DAG depasse la capacite d'un batch (~29 tx) ; le reste suit aux
 * cycles de sync suivants. */
#define MESHPAY_DAG_SYNC_MAX_CHUNKS_PER_REQUEST 8

typedef struct {
    uint16_t tx_count;
    uint8_t tip_count;
    uint8_t tips[MESHPAY_DAG_SYNC_MAX_TIPS][MESHPAY_TX_ID_SIZE];
    uint8_t digest[MESHPAY_DAG_SYNC_DIGEST_SIZE]; /* valide ssi has_digest */
    bool has_digest;
} meshpay_dag_sync_summary_t;

esp_err_t meshpay_dag_sync_build_summary(
    const meshpay_dag_t *dag,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);
esp_err_t meshpay_dag_sync_parse_summary(const rns_packet_t *packet,
                                         meshpay_dag_sync_summary_t *summary);

esp_err_t meshpay_dag_sync_build_request(
    const meshpay_dag_t *local_dag,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);
esp_err_t meshpay_dag_sync_build_request_from(
    const meshpay_dag_t *local_dag,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);
esp_err_t meshpay_dag_sync_build_request_from_count(
    uint16_t known_count,
    const uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);
esp_err_t meshpay_dag_sync_request_known_count(const rns_packet_t *packet,
                                               uint16_t *known_count);
esp_err_t meshpay_dag_sync_request_source(
    const rns_packet_t *packet,
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    bool *has_source);

esp_err_t meshpay_dag_sync_build_batch_resource(
    const meshpay_dag_t *source_dag,
    uint16_t start_index,
    const rns_link_t *link,
    rns_packet_t *packets,
    size_t max_packets,
    size_t *packet_count);
/* Variante paginee : *next_index = index de la 1re tx non incluse (= count si
 * tout tient) -> permet d'enchainer les chunks suivants. */
esp_err_t meshpay_dag_sync_build_batch_resource_from(
    const meshpay_dag_t *source_dag,
    uint16_t start_index,
    const rns_link_t *link,
    rns_packet_t *packets,
    size_t max_packets,
    size_t *packet_count,
    uint16_t *next_index);

esp_err_t meshpay_dag_sync_apply_batch(meshpay_dag_t *target_dag,
                                       const uint8_t *batch,
                                       size_t batch_len,
                                       size_t *merged_count);

/*
 * Variante avec filtre d'ingestion par registre (chantier nettoyage currency
 * legacy) : si `allowed_currency_id` est non NULL, toute tx du batch dont le
 * currency_id diffère est SKIPPÉE avant merge (comptée dans
 * *skipped_foreign_count, optionnel) — elle n'est ni jugée (pas de
 * CONFLICT/INVALID fatal sur du refusé) ni intégrée. NULL = tout accepter
 * (config de repli, monitor). meshpay_dag_sync_apply_batch == filtre NULL.
 */
esp_err_t meshpay_dag_sync_apply_batch_filtered(
    meshpay_dag_t *target_dag,
    const uint8_t *batch,
    size_t batch_len,
    const uint32_t *allowed_currency_id,
    size_t *merged_count,
    size_t *skipped_foreign_count);

#ifdef __cplusplus
}
#endif
