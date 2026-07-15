#include "meshpay/dag_store.h"

#include "meshpay/dag.h"
#include "meshpay/meshpay_tx.h"
#include "unity.h"
#include "test_pool.h"
#include <stdlib.h>
#include <string.h>

#define STORE_SIZE (64 * 1024) /* 2 slots de 32 Ko : tient ~140 tx/slot */

static void fill_seq(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

/* MINT « bien formée » (sans parent) : suffit pour peupler une DAG de test ;
 * dag_merge_tx ne valide que la forme (la règle monnaie est ailleurs). */
static void make_mint(meshpay_tx_t *tx, uint8_t seed, uint32_t amount)
{
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_MINT;
    fill_seq(tx->id, MESHPAY_TX_ID_SIZE, seed);
    fill_seq(tx->from, MESHPAY_TX_DESTINATION_HASH_SIZE, (uint8_t)(seed + 0x10));
    fill_seq(tx->to, MESHPAY_TX_DESTINATION_HASH_SIZE, (uint8_t)(seed + 0x20));
    tx->amount = amount;
    tx->seq = 0;
    tx->fee = 0;
    tx->currency_id = 1;
    tx->timestamp_ms = 1;
    fill_seq(tx->signature, MESHPAY_TX_SIGNATURE_SIZE, (uint8_t)(seed + 0x60));
}

/* Peuple une DAG déjà allouée/initialisée (par le pool) avec n MINT. */
static void build_dag(meshpay_dag_t *dag, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        meshpay_tx_t tx;
        make_mint(&tx, (uint8_t)(0x21 + i), (uint32_t)(100 + i));
        TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                          meshpay_dag_merge_tx(dag, &tx));
    }
    TEST_ASSERT_EQUAL_UINT32(n, meshpay_dag_count(dag));
}

TEST_CASE("dag store save then load restores identical dag", "[dag_store]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE);
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    meshpay_dag_t *src = test_pool_dag(0);
    build_dag(src, 5);
    uint8_t digest_src[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(src, digest_src));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, src, "test"));

    meshpay_dag_t *dst = test_pool_dag(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load(&be, dst));

    TEST_ASSERT_EQUAL_UINT32(5, meshpay_dag_count(dst));
    uint8_t digest_dst[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(dst, digest_dst));
    TEST_ASSERT_EQUAL_MEMORY(digest_src, digest_dst, RNS_CRYPTO_SHA256_SIZE);
    for (size_t i = 0; i < meshpay_dag_count(src); ++i) {
        const meshpay_tx_t *t = meshpay_dag_at(src, i);
        TEST_ASSERT_TRUE(meshpay_dag_contains(dst, t->id));
    }
    free(buf);
}

TEST_CASE("dag store load from blank partition returns not found",
          "[dag_store]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE); /* tout à 0xFF */
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    meshpay_dag_t *dst = test_pool_dag(0);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, meshpay_dag_store_load(&be, dst));
    free(buf);
}

TEST_CASE("dag store rejects corrupted slot", "[dag_store]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE);
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    meshpay_dag_t *src = test_pool_dag(0);
    build_dag(src, 3);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, src, "test"));

    /* Corrompt un octet dans la zone des enregistrements du slot A (offset 0). */
    buf[64] ^= 0xFF;

    meshpay_dag_t *dst = test_pool_dag(1);
    /* Un seul slot écrit, désormais corrompu => aucun slot valide. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, meshpay_dag_store_load(&be, dst));
    free(buf);
}

TEST_CASE("dag store double buffer loads newest and falls back on corruption",
          "[dag_store]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE);
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    /* Slot source (0) réutilisé : une fois sauvegardée, la DAG vit dans `buf`,
     * on peut donc réemployer le slot pour la génération suivante. Cela limite
     * le pic à 2 DAG simultanées (slot 0 = source, slot 1 = destination). */
    meshpay_dag_t *src = test_pool_dag(0);
    build_dag(src, 2);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, src, "v1"));

    src = test_pool_dag(0); /* ré-emprunt : slot remis à zéro pour la v2 */
    build_dag(src, 3);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, src, "v2"));

    /* Le chargement prend la génération la plus récente (v2, count 3). */
    meshpay_dag_t *dst = test_pool_dag(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load(&be, dst));
    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(dst));

    /* Corrompt le slot le plus récent (slot B à size/2) => repli sur v1. */
    buf[(STORE_SIZE / 2) + 64] ^= 0xFF;
    dst = test_pool_dag(1); /* ré-emprunt pour le second chargement */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load(&be, dst));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dst));
    free(buf);
}

TEST_CASE("dag store round trips a larger dag", "[dag_store]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE);
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    meshpay_dag_t *src = test_pool_dag(0);
    build_dag(src, 100);
    uint8_t digest_src[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(src, digest_src));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, src, "big"));

    meshpay_dag_t *dst = test_pool_dag(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load(&be, dst));
    TEST_ASSERT_EQUAL_UINT32(100, meshpay_dag_count(dst));
    uint8_t digest_dst[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(dst, digest_dst));
    TEST_ASSERT_EQUAL_MEMORY(digest_src, digest_dst, RNS_CRYPTO_SHA256_SIZE);
    free(buf);
}

/* --- Phase B (P3) : persistance du checkpoint en queue de partition --- */

TEST_CASE("dag store checkpoint roundtrip and generations", "[dag_store][p3]")
{
    uint8_t *buf = malloc(STORE_SIZE);
    TEST_ASSERT_NOT_NULL(buf);
    meshpay_dag_store_mock_t mock;
    meshpay_dag_store_mock_init(&mock, buf, STORE_SIZE);
    meshpay_dag_store_backend_t be = meshpay_dag_store_mock_backend(&mock);

    /* Partition vierge : aucun checkpoint. */
    meshpay_checkpoint_t *loaded = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_dag_store_load_checkpoint(&be, loaded));

    /* Un checkpoint signé plausible (2 comptes). */
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_checkpoint_t *cp = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(cp);
    meshpay_checkpoint_init(cp);
    cp->currency_id = 0xc5c42609;
    cp->generation = 1;
    cp->created_at_ms = 42;
    cp->account_count = 1;
    fill_seq(cp->accounts[0].account, RNS_IDENTITY_HASH_SIZE, 0x21);
    cp->accounts[0].balance = 8;
    cp->accounts[0].seq_floor = 3;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(cp, &founder));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_store_save_checkpoint(&be, cp, "test"));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load_checkpoint(&be, loaded));
    TEST_ASSERT_EQUAL_UINT32(1, loaded->generation);
    TEST_ASSERT_EQUAL_UINT32(8, loaded->accounts[0].balance);
    /* Le wire relu est toujours VÉRIFIABLE (signature intacte). */
    uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&founder, founder_public));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_checkpoint_verify(loaded, founder_public));

    /* Génération 2 dans l'AUTRE slot : le load élit la plus haute ; le
     * slot gen 1 survit comme repli (double-buffer). */
    cp->generation = 2;
    cp->accounts[0].balance = 6;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(cp, &founder));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_store_save_checkpoint(&be, cp, "test2"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load_checkpoint(&be, loaded));
    TEST_ASSERT_EQUAL_UINT32(2, loaded->generation);
    TEST_ASSERT_EQUAL_UINT32(6, loaded->accounts[0].balance);

    /* Coexistence : la FENÊTRE se sauve toujours sans écraser la zone
     * checkpoint (et réciproquement). */
    meshpay_dag_t *dag = test_pool_dag(0);
    meshpay_dag_init(dag);
    meshpay_tx_t tx;
    make_mint(&tx, 0x31, 5);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &tx));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_save(&be, dag, "fenetre"));
    meshpay_dag_t *reloaded = test_pool_dag(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load(&be, reloaded));
    TEST_ASSERT_EQUAL_size_t(1, meshpay_dag_count(reloaded));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_store_load_checkpoint(&be, loaded));
    TEST_ASSERT_EQUAL_UINT32(2, loaded->generation);

    free(cp);
    free(loaded);
    free(buf);
}
