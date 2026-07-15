#pragma once

#include "meshpay/meshpay_tx.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_DAG_MAX_TRANSACTIONS 250
#define MESHPAY_DAG_CHECKPOINT_THRESHOLD 200
#define MESHPAY_DAG_MAX_TIPS 32

typedef enum {
    MESHPAY_DAG_MERGE_OK = 0,
    MESHPAY_DAG_MERGE_DUPLICATE,
    MESHPAY_DAG_MERGE_CONFLICT,
    MESHPAY_DAG_MERGE_MISSING_PARENT,
    MESHPAY_DAG_MERGE_FULL,
    MESHPAY_DAG_MERGE_INVALID,
} meshpay_dag_merge_result_t;

typedef struct {
    meshpay_tx_t transactions[MESHPAY_DAG_MAX_TRANSACTIONS];
    size_t count;
} meshpay_dag_t;

void meshpay_dag_init(meshpay_dag_t *dag);
size_t meshpay_dag_count(const meshpay_dag_t *dag);
bool meshpay_dag_needs_checkpoint(const meshpay_dag_t *dag);

const meshpay_tx_t *meshpay_dag_find(const meshpay_dag_t *dag,
                                     const uint8_t id[MESHPAY_TX_ID_SIZE]);
const meshpay_tx_t *meshpay_dag_at(const meshpay_dag_t *dag, size_t index);
bool meshpay_dag_contains(const meshpay_dag_t *dag,
                          const uint8_t id[MESHPAY_TX_ID_SIZE]);

meshpay_dag_merge_result_t meshpay_dag_merge_tx(meshpay_dag_t *dag,
                                                const meshpay_tx_t *tx);

meshpay_dag_merge_result_t meshpay_dag_validate_merge(const meshpay_dag_t *dag,
                                                      const meshpay_tx_t *tx,
                                                      const meshpay_tx_t **existing);

meshpay_dag_merge_result_t meshpay_dag_get_tips(const meshpay_dag_t *dag,
                                                const meshpay_tx_t **tips,
                                                size_t max_tips,
                                                size_t *tip_count,
                                                size_t *total_tips);

/* Digest stable de l'ENSEMBLE des transactions (SHA-256 des id triés).
 * Indépendant de l'ordre d'insertion : deux DAG de même contenu => même digest.
 * Sert de critère de convergence pour les tests multi-devices. */
esp_err_t meshpay_dag_digest(const meshpay_dag_t *dag,
                             uint8_t out[RNS_CRYPTO_SHA256_SIZE]);

/*
 * Purge de la fenêtre toutes les tx dont le currency_id diffère de celui donné
 * (nettoyage des registres morts, ex. les boot-credits de la config de repli
 * d'avant les descripteurs). Compactage en place, ordre relatif des
 * survivantes préservé, slots libérés remis à zéro. Retourne le nombre purgé.
 *
 * Des survivantes peuvent référencer un parent purgé : référence pendante
 * TOLÉRÉE (cf. la note fenêtre glissante dans meshpay_dag_validate_merge) —
 * les parents ne sont jamais déréférencés. Ne PAS appeler en config de repli
 * (le registre de repli y est la monnaie légitime) ni sur un monitor.
 */
size_t meshpay_dag_purge_foreign(meshpay_dag_t *dag, uint32_t currency_id);

#ifdef __cplusplus
}
#endif
