#pragma once

#include "meshpay/meshpay_tx.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
