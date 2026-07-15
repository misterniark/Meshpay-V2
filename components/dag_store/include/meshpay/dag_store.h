#pragma once

/*
 * dag_store — persistance durable de la fenêtre DAG sur une partition flash.
 *
 * La DAG (registre des transactions) est en RAM ; sans persistance, elle est
 * perdue si TOUTES les cartes s'éteignent. Ce composant sauvegarde un
 * instantané (snapshot) de la fenêtre DAG et la restaure au boot, AVANT la
 * synchro Reticulum. Comme `meshpay_dag_t` n'a aucun état dérivé
 * (juste `transactions[]` + `count`), le chargement est un memcpy fidèle :
 * la DAG restaurée est byte-identique (même `dag_digest`).
 *
 * Robustesse : double-buffer A/B + marqueur de commit en footer (CRC32). Une
 * coupure pendant l'écriture laisse l'ancien slot intact → rechargé.
 *
 * Le backend d'E/S est injectable (offsets absolus dans la partition) :
 * impl `esp_partition` en firmware, mock RAM en test (comme `storage`).
 */

#include "esp_err.h"
#include "meshpay/dag.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lecture/écriture/effacement bruts à un offset absolu dans la partition. */
typedef esp_err_t (*meshpay_dag_store_read_fn_t)(void *ctx, size_t offset,
                                                 void *buf, size_t len);
typedef esp_err_t (*meshpay_dag_store_write_fn_t)(void *ctx, size_t offset,
                                                  const void *data, size_t len);
typedef esp_err_t (*meshpay_dag_store_erase_fn_t)(void *ctx, size_t offset,
                                                  size_t len);

typedef struct {
    meshpay_dag_store_read_fn_t read;
    meshpay_dag_store_write_fn_t write;
    meshpay_dag_store_erase_fn_t erase;
    size_t size;       /* taille totale de la partition (octets) */
    size_t erase_size; /* granularité d'effacement (secteur flash, ex. 4096) */
    void *ctx;
} meshpay_dag_store_backend_t;

/*
 * Sauvegarde la fenêtre DAG dans le slot INACTIF (double-buffer). Le footer
 * (CRC32 + digest + magic) n'est écrit qu'en dernier : il fait office de
 * marqueur de commit, de sorte qu'une écriture interrompue n'invalide pas le
 * slot précédent. `reason` n'est utilisé que pour la trace de log.
 */
esp_err_t meshpay_dag_store_save(const meshpay_dag_store_backend_t *backend,
                                 const meshpay_dag_t *dag,
                                 const char *reason);

/*
 * Restaure la DAG depuis le slot VALIDE le plus récent (génération la plus
 * haute). `dag` est réinitialisé puis rempli par snapshot. Retourne :
 *  - ESP_OK              : DAG restaurée ;
 *  - ESP_ERR_NOT_FOUND   : aucun slot valide (premier boot / partition vierge) ;
 *  - autre               : erreur d'E/S backend.
 */
esp_err_t meshpay_dag_store_load(const meshpay_dag_store_backend_t *backend,
                                 meshpay_dag_t *dag);

/*
 * Phase B — persistance du CHECKPOINT signé, dans une zone dédiée en QUEUE de
 * partition (double-buffer à part : le checkpoint change rarement — à
 * l'émission/adoption — alors que la fenêtre se sauve au débounce ; les slots
 * fenêtre gardent leurs offsets historiques, seul le slot 1 perd la queue en
 * capacité théorique — marge réelle très large). Le blob stocké est le WIRE
 * CBOR signé tel quel : au boot, l'appelant DOIT le vérifier
 * (meshpay_checkpoint_verify contre la clé du descripteur) AVANT de l'adopter
 * (meshpay_dag_adopt_checkpoint) — le store ne connaît pas la racine de
 * confiance. save : slot inactif + footer-commit (coupure = ancien conservé).
 * load : ESP_ERR_NOT_FOUND si aucun checkpoint persisté.
 */
esp_err_t meshpay_dag_store_save_checkpoint(
    const meshpay_dag_store_backend_t *backend,
    const meshpay_checkpoint_t *cp,
    const char *reason);
esp_err_t meshpay_dag_store_load_checkpoint(
    const meshpay_dag_store_backend_t *backend,
    meshpay_checkpoint_t *out_cp);

/* ------------------------------------------------------------------ */
/* Mock RAM pour les tests (simule une flash : erase=0xFF, write=copie). */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t size;
    size_t erase_size;
    uint32_t write_count;
    uint32_t erase_count;
} meshpay_dag_store_mock_t;

void meshpay_dag_store_mock_init(meshpay_dag_store_mock_t *mock,
                                 uint8_t *buf, size_t size);
meshpay_dag_store_backend_t
meshpay_dag_store_mock_backend(meshpay_dag_store_mock_t *mock);

/* ------------------------------------------------------------------ */
/* Backend esp_partition (firmware). Cherche la partition `label` (type   */
/* data) et remplit `out`. ESP_ERR_NOT_FOUND si la partition est absente. */
/* ------------------------------------------------------------------ */
esp_err_t meshpay_dag_store_partition_backend(const char *label,
                                              meshpay_dag_store_backend_t *out);

#ifdef __cplusplus
}
#endif
