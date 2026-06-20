#include "meshpay/dag.h"

#include "meshpay/rns/rns_crypto.h"
#include <string.h>

static bool bytes_zero(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return true;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static bool id_equal(const uint8_t a[MESHPAY_TX_ID_SIZE],
                     const uint8_t b[MESHPAY_TX_ID_SIZE])
{
    return rns_crypto_constant_equal(a, b, MESHPAY_TX_ID_SIZE);
}

static bool account_equal(const uint8_t a[MESHPAY_TX_DESTINATION_HASH_SIZE],
                          const uint8_t b[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    return rns_crypto_constant_equal(a, b, MESHPAY_TX_DESTINATION_HASH_SIZE);
}

static bool tx_shape_valid(const meshpay_tx_t *tx)
{
    if (tx == NULL) {
        return false;
    }
    if (tx->type != MESHPAY_TX_TYPE_TRANSFER &&
        tx->type != MESHPAY_TX_TYPE_MINT) {
        return false;
    }
    if (bytes_zero(tx->id, sizeof(tx->id)) ||
        bytes_zero(tx->from, sizeof(tx->from)) ||
        bytes_zero(tx->to, sizeof(tx->to)) ||
        bytes_zero(tx->signature, sizeof(tx->signature)) ||
        tx->amount == 0 ||
        tx->parent_count > MESHPAY_TX_MAX_PARENTS) {
        return false;
    }
    if (tx->type == MESHPAY_TX_TYPE_TRANSFER && tx->fee >= tx->amount) {
        return false;
    }
    if (tx->type == MESHPAY_TX_TYPE_MINT && tx->fee != 0) {
        return false;
    }
    return true;
}

void meshpay_dag_init(meshpay_dag_t *dag)
{
    if (dag != NULL) {
        memset(dag, 0, sizeof(*dag));
    }
}

size_t meshpay_dag_count(const meshpay_dag_t *dag)
{
    return dag == NULL ? 0 : dag->count;
}

bool meshpay_dag_needs_checkpoint(const meshpay_dag_t *dag)
{
    return dag != NULL && dag->count >= MESHPAY_DAG_CHECKPOINT_THRESHOLD;
}

const meshpay_tx_t *meshpay_dag_find(const meshpay_dag_t *dag,
                                     const uint8_t id[MESHPAY_TX_ID_SIZE])
{
    if (dag == NULL || id == NULL || bytes_zero(id, MESHPAY_TX_ID_SIZE)) {
        return NULL;
    }
    for (size_t i = 0; i < dag->count; ++i) {
        if (id_equal(dag->transactions[i].id, id)) {
            return &dag->transactions[i];
        }
    }
    return NULL;
}

const meshpay_tx_t *meshpay_dag_at(const meshpay_dag_t *dag, size_t index)
{
    if (dag == NULL || index >= dag->count) {
        return NULL;
    }
    return &dag->transactions[index];
}

bool meshpay_dag_contains(const meshpay_dag_t *dag,
                          const uint8_t id[MESHPAY_TX_ID_SIZE])
{
    return meshpay_dag_find(dag, id) != NULL;
}

meshpay_dag_merge_result_t meshpay_dag_validate_merge(const meshpay_dag_t *dag,
                                                      const meshpay_tx_t *tx,
                                                      const meshpay_tx_t **existing)
{
    if (existing != NULL) {
        *existing = NULL;
    }
    if (dag == NULL || !tx_shape_valid(tx)) {
        return MESHPAY_DAG_MERGE_INVALID;
    }
    if (dag->count >= MESHPAY_DAG_MAX_TRANSACTIONS) {
        return MESHPAY_DAG_MERGE_FULL;
    }

    const meshpay_tx_t *same_id = meshpay_dag_find(dag, tx->id);
    if (same_id != NULL) {
        if (existing != NULL) {
            *existing = same_id;
        }
        return MESHPAY_DAG_MERGE_DUPLICATE;
    }

    for (uint8_t i = 0; i < tx->parent_count; ++i) {
        if (bytes_zero(tx->parents[i], MESHPAY_TX_PARENT_ID_SIZE)) {
            return MESHPAY_DAG_MERGE_INVALID;
        }
        if (!meshpay_dag_contains(dag, tx->parents[i])) {
            return MESHPAY_DAG_MERGE_MISSING_PARENT;
        }
    }

    for (size_t i = 0; i < dag->count; ++i) {
        const meshpay_tx_t *candidate = &dag->transactions[i];
        if (candidate->seq == tx->seq &&
            account_equal(candidate->from, tx->from) &&
            !id_equal(candidate->id, tx->id)) {
            if (existing != NULL) {
                *existing = candidate;
            }
            return MESHPAY_DAG_MERGE_CONFLICT;
        }
    }

    return MESHPAY_DAG_MERGE_OK;
}

meshpay_dag_merge_result_t meshpay_dag_merge_tx(meshpay_dag_t *dag,
                                                const meshpay_tx_t *tx)
{
    meshpay_dag_merge_result_t result =
        meshpay_dag_validate_merge(dag, tx, NULL);
    if (result != MESHPAY_DAG_MERGE_OK) {
        return result;
    }

    memcpy(&dag->transactions[dag->count], tx, sizeof(*tx));
    dag->count++;
    return MESHPAY_DAG_MERGE_OK;
}

static bool is_parent_reference(const meshpay_tx_t *child,
                                const uint8_t parent_id[MESHPAY_TX_ID_SIZE])
{
    for (uint8_t i = 0; i < child->parent_count; ++i) {
        if (id_equal(child->parents[i], parent_id)) {
            return true;
        }
    }
    return false;
}

meshpay_dag_merge_result_t meshpay_dag_get_tips(const meshpay_dag_t *dag,
                                                const meshpay_tx_t **tips,
                                                size_t max_tips,
                                                size_t *tip_count,
                                                size_t *total_tips)
{
    if (dag == NULL || tips == NULL || tip_count == NULL) {
        return MESHPAY_DAG_MERGE_INVALID;
    }

    *tip_count = 0;
    size_t total = 0;

    for (size_t i = 0; i < dag->count; ++i) {
        const meshpay_tx_t *candidate = &dag->transactions[i];
        bool referenced = false;

        for (size_t j = 0; j < dag->count && !referenced; ++j) {
            if (i == j) {
                continue;
            }
            referenced = is_parent_reference(&dag->transactions[j],
                                             candidate->id);
        }
        if (referenced) {
            continue;
        }

        total++;
        if (max_tips == 0) {
            continue;
        }

        if (*tip_count < max_tips) {
            size_t pos = *tip_count;
            while (pos > 0 &&
                   tips[pos - 1]->timestamp_ms < candidate->timestamp_ms) {
                tips[pos] = tips[pos - 1];
                pos--;
            }
            tips[pos] = candidate;
            (*tip_count)++;
        } else if (tips[max_tips - 1]->timestamp_ms < candidate->timestamp_ms) {
            size_t pos = max_tips - 1;
            while (pos > 0 &&
                   tips[pos - 1]->timestamp_ms < candidate->timestamp_ms) {
                tips[pos] = tips[pos - 1];
                pos--;
            }
            tips[pos] = candidate;
        }
    }

    if (total_tips != NULL) {
        *total_tips = total;
    }
    return MESHPAY_DAG_MERGE_OK;
}
