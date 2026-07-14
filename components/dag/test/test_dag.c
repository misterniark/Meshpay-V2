#include "meshpay/dag.h"
#include "unity.h"
#include "test_pool.h"
#include <stdlib.h>
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void make_tx(meshpay_tx_t *tx,
                    meshpay_tx_type_t type,
                    uint8_t id_seed,
                    uint8_t from_seed,
                    uint8_t to_seed,
                    uint32_t seq,
                    uint32_t amount,
                    uint64_t timestamp_ms,
                    const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                    uint8_t parent_count)
{
    meshpay_tx_clear(tx);
    tx->type = type;
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    fill_sequence(tx->from, sizeof(tx->from), from_seed);
    fill_sequence(tx->to, sizeof(tx->to), to_seed);
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = type == MESHPAY_TX_TYPE_TRANSFER ? 1 : 0;
    tx->currency_id = 0x4d505632;
    tx->timestamp_ms = timestamp_ms;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x20));
}

TEST_CASE("dag merges valid txs and returns newest tip", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t genesis;
    make_tx(&genesis, MESHPAY_TX_TYPE_MINT, 0x10, 0x01, 0x31,
            0, 1000, 100, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &genesis));

    uint8_t parent_refs[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parent_refs[0], genesis.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t child;
    make_tx(&child, MESHPAY_TX_TYPE_TRANSFER, 0x40, 0x01, 0x51,
            1, 250, 200, parent_refs, 1);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &child));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));

    const meshpay_tx_t *tips[MESHPAY_DAG_MAX_TIPS];
    size_t tip_count = 0;
    size_t total_tips = 0;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_get_tips(dag, tips, 2,
                                           &tip_count, &total_tips));
    TEST_ASSERT_EQUAL_UINT32(1, tip_count);
    TEST_ASSERT_EQUAL_UINT32(1, total_tips);
    TEST_ASSERT_EQUAL_MEMORY(child.id, tips[0]->id, MESHPAY_TX_ID_SIZE);
}

TEST_CASE("dag rejects duplicate tx id", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t tx;
    make_tx(&tx, MESHPAY_TX_TYPE_MINT, 0x11, 0x02, 0x32,
            0, 1000, 100, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &tx));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_DUPLICATE,
                      meshpay_dag_merge_tx(dag, &tx));
}

TEST_CASE("dag rejects conflict on from and seq", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t parent;
    make_tx(&parent, MESHPAY_TX_TYPE_MINT, 0x12, 0x03, 0x33,
            0, 1000, 100, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &parent));

    uint8_t parent_refs[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parent_refs[0], parent.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t tx_a;
    meshpay_tx_t tx_b;
    make_tx(&tx_a, MESHPAY_TX_TYPE_TRANSFER, 0x41, 0x09, 0x44,
            7, 100, 200, parent_refs, 1);
    make_tx(&tx_b, MESHPAY_TX_TYPE_TRANSFER, 0x55, 0x09, 0x45,
            7, 120, 201, parent_refs, 1);

    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &tx_a));

    const meshpay_tx_t *existing = NULL;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_CONFLICT,
                      meshpay_dag_validate_merge(dag, &tx_b, &existing));
    TEST_ASSERT_NOT_NULL(existing);
    TEST_ASSERT_EQUAL_MEMORY(tx_a.id, existing->id, MESHPAY_TX_ID_SIZE);
}

TEST_CASE("dag rejects missing parent", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    uint8_t missing[1][MESHPAY_TX_PARENT_ID_SIZE];
    fill_sequence(missing[0], MESHPAY_TX_PARENT_ID_SIZE, 0xaa);

    meshpay_tx_t tx;
    make_tx(&tx, MESHPAY_TX_TYPE_TRANSFER, 0x42, 0x0a, 0x46,
            1, 10, 100, missing, 1);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_MISSING_PARENT,
                      meshpay_dag_merge_tx(dag, &tx));
}

TEST_CASE("dag rejects unsigned transaction shape", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t tx;
    make_tx(&tx, MESHPAY_TX_TYPE_MINT, 0x13, 0x04, 0x34,
            0, 1000, 100, NULL, 0);
    memset(tx.signature, 0, sizeof(tx.signature));

    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_INVALID,
                      meshpay_dag_merge_tx(dag, &tx));
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_dag_count(dag));
}

TEST_CASE("dag checkpoint threshold is reached at two hundred txs", "[dag]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    for (uint32_t i = 0; i < MESHPAY_DAG_CHECKPOINT_THRESHOLD; ++i) {
        meshpay_tx_t tx;
        make_tx(&tx, MESHPAY_TX_TYPE_MINT, (uint8_t)(0x20 + i),
                0x66, 0x77, i, 100 + i, i, NULL, 0);
        TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                          meshpay_dag_merge_tx(dag, &tx));
        if (i + 1U < MESHPAY_DAG_CHECKPOINT_THRESHOLD) {
            TEST_ASSERT_FALSE(meshpay_dag_needs_checkpoint(dag));
        }
    }

    TEST_ASSERT_EQUAL_UINT32(MESHPAY_DAG_CHECKPOINT_THRESHOLD,
                             meshpay_dag_count(dag));
    TEST_ASSERT_TRUE(meshpay_dag_needs_checkpoint(dag));
}

/* Helper : TX minimale identifiée par un octet de graine (digest ne lit que id). */
static meshpay_tx_t mp_tx_with_id(uint8_t seed)
{
    meshpay_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    memset(tx.id, seed, MESHPAY_TX_ID_SIZE);
    return tx;
}

TEST_CASE("dag digest is identical for the same tx set in different order", "[dag]")
{
    meshpay_dag_t *a = test_pool_dag(0);
    meshpay_dag_t *b = test_pool_dag(1);
    meshpay_tx_t t1 = mp_tx_with_id(0x11);
    meshpay_tx_t t2 = mp_tx_with_id(0x22);
    meshpay_tx_t t3 = mp_tx_with_id(0x33);
    a->transactions[0] = t1; a->transactions[1] = t2; a->transactions[2] = t3; a->count = 3;
    b->transactions[0] = t3; b->transactions[1] = t1; b->transactions[2] = t2; b->count = 3;

    uint8_t da[RNS_CRYPTO_SHA256_SIZE];
    uint8_t db[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(a, da));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(b, db));
    TEST_ASSERT_EQUAL_MEMORY(da, db, RNS_CRYPTO_SHA256_SIZE);
}

TEST_CASE("dag digest differs when one tx differs", "[dag]")
{
    meshpay_dag_t *a = test_pool_dag(0);
    meshpay_dag_t *b = test_pool_dag(1);
    a->transactions[0] = mp_tx_with_id(0x11); a->transactions[1] = mp_tx_with_id(0x22); a->count = 2;
    b->transactions[0] = mp_tx_with_id(0x11); b->transactions[1] = mp_tx_with_id(0x99); b->count = 2;

    uint8_t da[RNS_CRYPTO_SHA256_SIZE];
    uint8_t db[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(a, da));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(b, db));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(da, db, RNS_CRYPTO_SHA256_SIZE));
}

TEST_CASE("dag digest of empty dag is deterministic", "[dag]")
{
    meshpay_dag_t *a = test_pool_dag(0);
    uint8_t d1[RNS_CRYPTO_SHA256_SIZE];
    uint8_t d2[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(a, d1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(a, d2));
    TEST_ASSERT_EQUAL_MEMORY(d1, d2, RNS_CRYPTO_SHA256_SIZE);
}

/* --- Palier C3 : la CLAIM (crédit initial réflexif) dans le DAG --- */

/* Le DAG accepte une CLAIM bien formée (from==to, fee==0, seq==0, 0 parent =
 * genesis local) ET rejette une 2e CLAIM du MÊME membre : même (from, seq==0),
 * id différent -> CONFLICT. C'est le pivot « une réclamation par membre ». */
TEST_CASE("dag accepts claim and rejects second claim from same member", "[dag][c3]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t claim1;
    make_tx(&claim1, MESHPAY_TX_TYPE_CLAIM, 0x60, 0x30, 0x30,
            0, 100, 500, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim1));
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(dag));

    /* 2e CLAIM du même membre (même from=0x30, seq=0) mais id différent. */
    meshpay_tx_t claim2;
    make_tx(&claim2, MESHPAY_TX_TYPE_CLAIM, 0x90, 0x30, 0x30,
            0, 100, 501, NULL, 0);
    const meshpay_tx_t *existing = NULL;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_CONFLICT,
                      meshpay_dag_validate_merge(dag, &claim2, &existing));
    TEST_ASSERT_NOT_NULL(existing);
    TEST_ASSERT_EQUAL_MEMORY(claim1.id, existing->id, MESHPAY_TX_ID_SIZE);
    /* Le merge effectif renvoie aussi CONFLICT et laisse le compte inchangé. */
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_CONFLICT,
                      meshpay_dag_merge_tx(dag, &claim2));
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(dag));
}

/* Les CLAIM de membres DIFFÉRENTS coexistent : même seq==0 mais from distincts
 * -> pas de conflit. */
TEST_CASE("dag lets claims from different members coexist", "[dag][c3]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t claim_b;
    meshpay_tx_t claim_c;
    make_tx(&claim_b, MESHPAY_TX_TYPE_CLAIM, 0x61, 0x30, 0x30, 0, 100, 500, NULL, 0);
    make_tx(&claim_c, MESHPAY_TX_TYPE_CLAIM, 0x91, 0x50, 0x50, 0, 100, 501, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim_b));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim_c));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));
}

/* Convergence : deux CLAIM de membres différents mergées dans des ORDRES opposés
 * donnent le même digest (elles commutent). */
TEST_CASE("dag digest converges regardless of claim merge order", "[dag][c3]")
{
    meshpay_dag_t *a = test_pool_dag(0);
    meshpay_dag_t *b = test_pool_dag(1);

    meshpay_tx_t claim_b;
    meshpay_tx_t claim_c;
    make_tx(&claim_b, MESHPAY_TX_TYPE_CLAIM, 0x62, 0x30, 0x30, 0, 100, 500, NULL, 0);
    make_tx(&claim_c, MESHPAY_TX_TYPE_CLAIM, 0x92, 0x50, 0x50, 0, 100, 501, NULL, 0);

    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(a, &claim_b));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(a, &claim_c));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(b, &claim_c));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(b, &claim_b));

    uint8_t da[RNS_CRYPTO_SHA256_SIZE];
    uint8_t db[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(a, da));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(b, db));
    TEST_ASSERT_EQUAL_MEMORY(da, db, RNS_CRYPTO_SHA256_SIZE);
}

/* Le conflit (from, seq) est SCOPÉ PAR currency_id : le boot-credit MINT legacy
 * (from=X, seq=0, cur=1) et la CLAIM de crédit initial (from=X, seq=0, cur=2)
 * coexistent — sinon le membre ne recevrait jamais son crédit (constat critique
 * #2 de la revue). Deux registres distincts réutilisent (from, seq==0). */
TEST_CASE("dag scopes the from/seq conflict by currency", "[dag][c3]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    /* MINT réflexif legacy dans la monnaie de repli (cur=1). */
    meshpay_tx_t legacy_mint;
    make_tx(&legacy_mint, MESHPAY_TX_TYPE_MINT, 0x66, 0x30, 0x30, 0, 500, 100, NULL, 0);
    legacy_mint.currency_id = 1;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &legacy_mint));

    /* CLAIM du même membre, même seq==0, mais AUTRE monnaie (cur=2) -> pas de
     * conflit grâce au scope par currency_id. */
    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x96, 0x30, 0x30, 0, 250, 101, NULL, 0);
    claim.currency_id = 2;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));

    /* Contrôle : une 2e CLAIM même membre / MÊME monnaie (cur=2) conflit bien. */
    meshpay_tx_t claim_dup;
    make_tx(&claim_dup, MESHPAY_TX_TYPE_CLAIM, 0xA6, 0x30, 0x30, 0, 250, 102, NULL, 0);
    claim_dup.currency_id = 2;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_CONFLICT, meshpay_dag_merge_tx(dag, &claim_dup));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));
}

/* Le DAG rejette une CLAIM MAL FORMÉE (défense en profondeur, indépendante de
 * meshpay_tx) : from!=to, seq!=0 (réservé), ou fee!=0 -> INVALID. */
TEST_CASE("dag rejects malformed claim shape", "[dag][c3]")
{
    meshpay_dag_t *dag = test_pool_dag(0);

    /* from != to : viole la réflexivité. */
    meshpay_tx_t bad_reflexive;
    make_tx(&bad_reflexive, MESHPAY_TX_TYPE_CLAIM, 0x63, 0x30, 0x50, 0, 100, 500, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_INVALID,
                      meshpay_dag_merge_tx(dag, &bad_reflexive));

    /* seq != 0 : casserait l'unicité (from, 0). */
    meshpay_tx_t bad_seq;
    make_tx(&bad_seq, MESHPAY_TX_TYPE_CLAIM, 0x64, 0x30, 0x30, 1, 100, 500, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_INVALID,
                      meshpay_dag_merge_tx(dag, &bad_seq));

    /* fee != 0 : une CLAIM ne porte pas de frais. */
    meshpay_tx_t bad_fee;
    make_tx(&bad_fee, MESHPAY_TX_TYPE_CLAIM, 0x65, 0x30, 0x30, 0, 100, 500, NULL, 0);
    bad_fee.fee = 5;
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_INVALID,
                      meshpay_dag_merge_tx(dag, &bad_fee));

    TEST_ASSERT_EQUAL_UINT32(0, meshpay_dag_count(dag));
}
