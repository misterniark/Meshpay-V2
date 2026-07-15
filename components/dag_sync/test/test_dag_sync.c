#include "meshpay/dag_sync.h"
#include "test_pool.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void make_active_link(rns_link_t *link)
{
    rns_link_clear(link);
    link->status = RNS_LINK_STATUS_ACTIVE;
    link->mtu = RNS_PACKET_MTU;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    fill_sequence(link->link_id, sizeof(link->link_id), 0x90);
}

static void make_tx(meshpay_tx_t *tx,
                    meshpay_tx_type_t type,
                    uint8_t id_seed,
                    const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                    uint32_t amount,
                    uint32_t seq,
                    const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                    uint8_t parent_count)
{
    meshpay_tx_clear(tx);
    tx->type = type;
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    memcpy(tx->from, from, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = type == MESHPAY_TX_TYPE_TRANSFER ? 1 : 0;
    tx->currency_id = 1;
    tx->timestamp_ms = 1000 + seq;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x40));
}

static void build_dags(meshpay_dag_t *full,
                       meshpay_dag_t *slow,
                       meshpay_tx_t *tx0,
                       meshpay_tx_t *tx1,
                       meshpay_tx_t *tx2)
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);

    meshpay_dag_init(full);
    meshpay_dag_init(slow);

    make_tx(tx0, MESHPAY_TX_TYPE_MINT, 0x20, master, alice,
            1000, 0, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(full, tx0));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(slow, tx0));

    uint8_t parent1[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parent1[0], tx0->id, MESHPAY_TX_PARENT_ID_SIZE);
    make_tx(tx1, MESHPAY_TX_TYPE_TRANSFER, 0x50, alice, bob,
            100, 1, parent1, 1);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(full, tx1));

    uint8_t parent2[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parent2[0], tx1->id, MESHPAY_TX_PARENT_ID_SIZE);
    make_tx(tx2, MESHPAY_TX_TYPE_TRANSFER, 0x60, bob, alice,
            40, 1, parent2, 1);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(full, tx2));
}

TEST_CASE("dag sync summary exposes count and current tip", "[dag_sync]")
{
    meshpay_dag_t *full = test_pool_dag(0);
    meshpay_dag_t *slow = test_pool_dag(1);
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(full, slow, &tx0, &tx1, &tx2);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0xaa);
    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(full, source, &packet));
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_DAG_SYNC_MSG_SUMMARY, packet.data[0]);

    meshpay_dag_sync_summary_t summary;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_parse_summary(&packet, &summary));
    TEST_ASSERT_EQUAL_UINT16(3, summary.tx_count);
    TEST_ASSERT_EQUAL_UINT8(1, summary.tip_count);
    TEST_ASSERT_EQUAL_MEMORY(tx2.id, summary.tips[0], MESHPAY_TX_ID_SIZE);
}

TEST_CASE("dag sync rejects zero routing identifiers", "[dag_sync]")
{
    meshpay_dag_t *full = test_pool_dag(0);
    meshpay_dag_t *slow = test_pool_dag(1);
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(full, slow, &tx0, &tx1, &tx2);

    uint8_t zero[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(peer, sizeof(peer), 0xbb);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_summary(full,
                                                     zero,
                                                     &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_request(slow,
                                                     zero,
                                                     &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_request_from(slow,
                                                          peer,
                                                          zero,
                                                          &packet));
}

TEST_CASE("dag sync catches up missing transactions through resource batch", "[dag_sync]")
{
    meshpay_dag_t *full = test_pool_dag(0);
    meshpay_dag_t *slow = test_pool_dag(1);
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(full, slow, &tx0, &tx1, &tx2);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(slow));

    uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(peer, sizeof(peer), 0xbb);
    rns_packet_t request;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_request(slow, peer, &request));
    TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_REQUEST, request.context);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_LINK, request.destination_type);
    TEST_ASSERT_EQUAL_MEMORY(peer, request.destination_hash, sizeof(peer));
    uint16_t known_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_request_known_count(&request,
                                                           &known_count));
    TEST_ASSERT_EQUAL_UINT16(1, known_count);

    uint8_t requester[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(requester, sizeof(requester), 0xcc);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_request_from(slow,
                                                          peer,
                                                          requester,
                                                          &request));
    TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_REQUEST, request.context);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_LINK, request.destination_type);
    bool has_source = false;
    uint8_t parsed_source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_request_source(&request,
                                                      parsed_source,
                                                      &has_source));
    TEST_ASSERT_TRUE(has_source);
    TEST_ASSERT_EQUAL_MEMORY(requester, parsed_source, sizeof(requester));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_request_known_count(&request,
                                                           &known_count));
    TEST_ASSERT_EQUAL_UINT16(1, known_count);

    rns_link_t link;
    make_active_link(&link);
    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_batch_resource(full,
                                                            known_count,
                                                            &link,
                                                            packets,
                                                            RNS_RESOURCE_MAX_FRAGMENTS,
                                                            &packet_count));
    TEST_ASSERT_TRUE(packet_count >= 1);

    rns_resource_reassembler_t reassembler;
    rns_resource_reassembler_init(&reassembler);
    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    bool complete = false;
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_resource_reassembler_accept(&reassembler,
                                                          &packets[i],
                                                          batch,
                                                          sizeof(batch),
                                                          &batch_len,
                                                          &complete));
    }
    TEST_ASSERT_TRUE(complete);

    size_t merged = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch(slow, batch, batch_len,
                                                   &merged));
    TEST_ASSERT_EQUAL_UINT32(2, merged);
    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(slow));
    TEST_ASSERT_TRUE(meshpay_dag_contains(slow, tx2.id));
}

/* Encode un batch DAG-sync a la main, dans l'ordre exact du tableau fourni.
 * Reproduit le format de encode_batch() (dag_sync.c) : [count u16 BE] puis,
 * pour chaque tx, [len u16 BE][tx CBOR]. Sert a fabriquer un batch volontairement
 * DESORDONNE (enfant avant parent), ce que build_batch_resource ne peut pas
 * produire car une DAG est toujours topologiquement ordonnee. */
static void encode_batch_manual(const meshpay_tx_t *txs, uint16_t n,
                                uint8_t *batch, size_t batch_size,
                                size_t *batch_len)
{
    size_t pos = 2;
    for (uint16_t i = 0; i < n; ++i) {
        uint8_t enc[MESHPAY_TX_CBOR_MAX_SIZE];
        size_t enc_len = 0;
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_tx_encode(&txs[i], enc, sizeof(enc), &enc_len));
        TEST_ASSERT_TRUE(pos + 2U + enc_len <= batch_size);
        batch[pos] = (uint8_t)(enc_len >> 8);
        batch[pos + 1] = (uint8_t)enc_len;
        pos += 2;
        memcpy(batch + pos, enc, enc_len);
        pos += enc_len;
    }
    batch[0] = (uint8_t)(n >> 8);
    batch[1] = (uint8_t)n;
    *batch_len = pos;
}

/* Bug Phase 3 : sous emission concurrente, un noeud recoit un batch ou une tx
 * enfant precede son parent (autre branche / ordre non topologique). L'ancien
 * apply_batch abandonnait au 1er MISSING_PARENT (return ESP_ERR_INVALID_STATE),
 * 0 tx integree, blocage. Le fix multi-passes doit appliquer les deux tx. */
TEST_CASE("dag sync apply_batch applies out-of-order batch (child before parent)", "[dag_sync]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);

    meshpay_tx_t parent_tx;
    meshpay_tx_t child_tx;
    make_tx(&parent_tx, MESHPAY_TX_TYPE_MINT, 0x20, master, alice, 1000, 0, NULL, 0);
    uint8_t pref[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(pref[0], parent_tx.id, MESHPAY_TX_PARENT_ID_SIZE);
    make_tx(&child_tx, MESHPAY_TX_TYPE_TRANSFER, 0x50, alice, bob, 100, 1, pref, 1);

    /* Batch volontairement desordonne : enfant AVANT parent. */
    meshpay_tx_t ordered[2] = { child_tx, parent_tx };
    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    encode_batch_manual(ordered, 2, batch, sizeof(batch), &batch_len);

    meshpay_dag_t *dag = test_pool_dag(0);
    size_t merged = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch(dag, batch, batch_len, &merged));
    TEST_ASSERT_EQUAL_UINT32(2, merged);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, parent_tx.id));
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, child_tx.id));
}

/* Garde anti-regression : le fix multi-passes ne doit PAS rendre le merge
 * permissif. Une tx en CONFLICT (meme from+seq qu'une tx locale, id different =
 * double-depense) reste fatale : apply_batch doit echouer, pas l'integrer. */
TEST_CASE("dag sync apply_batch still rejects a conflicting tx", "[dag_sync]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t carol[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);
    fill_sequence(carol, sizeof(carol), 0xa0);

    /* DAG cible : MINT puis un TRANSFER alice (seq=5). */
    meshpay_tx_t tx0;
    meshpay_tx_t tx_a;
    make_tx(&tx0, MESHPAY_TX_TYPE_MINT, 0x20, master, alice, 1000, 0, NULL, 0);
    uint8_t p0[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(p0[0], tx0.id, MESHPAY_TX_PARENT_ID_SIZE);
    make_tx(&tx_a, MESHPAY_TX_TYPE_TRANSFER, 0x50, alice, bob, 100, 5, p0, 1);

    meshpay_dag_t *dag = test_pool_dag(0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &tx0));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &tx_a));

    /* Batch contenant tx_b : meme from=alice + meme seq=5, id different => CONFLICT. */
    meshpay_tx_t tx_b;
    make_tx(&tx_b, MESHPAY_TX_TYPE_TRANSFER, 0x80, alice, carol, 120, 5, p0, 1);
    meshpay_tx_t one[1] = { tx_b };
    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    encode_batch_manual(one, 1, batch, sizeof(batch), &batch_len);

    size_t merged = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_dag_sync_apply_batch(dag, batch, batch_len, &merged));
    TEST_ASSERT_FALSE(meshpay_dag_contains(dag, tx_b.id));
}

/* Chantier nettoyage currency legacy : sous une monnaie à descripteur, les tx
 * d'un autre registre (boot-credits du repli...) sont skippées à l'ingestion —
 * jamais mergées, jamais fatales — tandis qu'une tx active dont le PARENT est
 * une legacy filtrée passe quand même (référence pendante tolérée : c'est la
 * topologie réelle observée au dump du 2026-07-15). Filtre NULL = tout passe. */
TEST_CASE("dag sync apply_batch filters foreign currency txs", "[dag_sync][n2]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);

    const uint32_t active = 0xC5C42609u;

    /* 2 legacy (currency_id=1, celui de make_tx) + 2 actives, la première
     * active ayant la première legacy pour parent. */
    meshpay_tx_t legacy_a;
    meshpay_tx_t legacy_b;
    make_tx(&legacy_a, MESHPAY_TX_TYPE_MINT, 0x20, master, master, 10, 0,
            NULL, 0);
    make_tx(&legacy_b, MESHPAY_TX_TYPE_MINT, 0x21, alice, alice, 10, 0,
            NULL, 0);

    uint8_t on_legacy[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(on_legacy[0], legacy_a.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x30, alice, alice, 8, 0,
            on_legacy, 1);
    claim.currency_id = active;
    /* Wire v2 : une CLAIM sans clé embarquée ne s'ENCODE plus (forme) — ce
     * test n'exerce pas le gate, une clé factice non nulle suffit. */
    fill_sequence(claim.member_public, sizeof(claim.member_public), 0xC4);

    uint8_t on_claim[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(on_claim[0], claim.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t pay;
    make_tx(&pay, MESHPAY_TX_TYPE_TRANSFER, 0x31, alice, master, 3, 1,
            on_claim, 1);
    pay.currency_id = active;

    meshpay_tx_t all[4] = { legacy_a, claim, legacy_b, pay };
    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    encode_batch_manual(all, 4, batch, sizeof(batch), &batch_len);

    /* Avec filtre : seules les 2 actives entrent, les 2 legacy sont comptées
     * skipped (une seule fois malgré le multi-passes). */
    meshpay_dag_t *dag = test_pool_dag(0);
    size_t merged = 0;
    size_t skipped = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch_filtered(
                          dag, batch, batch_len, &active, &merged, &skipped));
    TEST_ASSERT_EQUAL_UINT32(2, merged);
    TEST_ASSERT_EQUAL_UINT32(2, skipped);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(dag));
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, claim.id));
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, pay.id));
    TEST_ASSERT_FALSE(meshpay_dag_contains(dag, legacy_a.id));
    TEST_ASSERT_FALSE(meshpay_dag_contains(dag, legacy_b.id));

    /* Sans filtre (repli/monitor) : tout passe. */
    meshpay_dag_t *open_dag = test_pool_dag(1);
    merged = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch(open_dag, batch, batch_len,
                                                   &merged));
    TEST_ASSERT_EQUAL_UINT32(4, merged);
    TEST_ASSERT_EQUAL_UINT32(4, meshpay_dag_count(open_dag));
}


/* PAGINATION (revue #5) : une DAG dont l'encodage depasse un seul batch doit
 * etre transferee en PLUSIEURS chunks Resource couvrant exactement [0, count)
 * sans trou ni doublon ; un recepteur vide qui applique tous les chunks atteint
 * le compte complet. Valide encode_batch (chunk partiel + next_index) et le
 * chainage cursor->next. */
TEST_CASE("dag sync build_batch_resource_from paginates a large dag", "[dag_sync]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);

    /* 100 MINT independants (sans parent) -> encodage tres au-dessus de la
     * capacite d'un batch (~29 tx) => plusieurs chunks garantis. (from,seq)
     * distincts (seq=i) => aucun conflit ; id distinct via id_seed. */
    meshpay_dag_t *src = test_pool_dag(0);
    const uint32_t N = 100;
    for (uint32_t i = 0; i < N; ++i) {
        meshpay_tx_t tx;
        make_tx(&tx, MESHPAY_TX_TYPE_MINT, (uint8_t)(0x10 + i), master, alice,
                1000 + i, i, NULL, 0);
        TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(src, &tx));
    }
    TEST_ASSERT_EQUAL_UINT32(N, meshpay_dag_count(src));

    rns_link_t link;
    make_active_link(&link);
    meshpay_dag_t *dst = test_pool_dag(1); /* recepteur vide */

    uint16_t cursor = 0;
    size_t chunk_count = 0;
    while (cursor < meshpay_dag_count(src)) {
        rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
        size_t pc = 0;
        uint16_t next = cursor;
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_dag_sync_build_batch_resource_from(
                              src, cursor, &link, packets,
                              RNS_RESOURCE_MAX_FRAGMENTS, &pc, &next));
        TEST_ASSERT_TRUE(pc >= 1);
        TEST_ASSERT_TRUE(next > cursor); /* progression stricte (>=1 tx/chunk) */

        /* Reassemblage du chunk puis application chez le recepteur. */
        rns_resource_reassembler_t re;
        rns_resource_reassembler_init(&re);
        uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
        size_t bl = 0;
        bool complete = false;
        for (size_t i = 0; i < pc; ++i) {
            TEST_ASSERT_EQUAL(ESP_OK,
                              rns_resource_reassembler_accept(
                                  &re, &packets[i], batch, sizeof(batch),
                                  &bl, &complete));
        }
        TEST_ASSERT_TRUE(complete);
        size_t merged = 0;
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_dag_sync_apply_batch(dst, batch, bl, &merged));
        cursor = next;
        chunk_count++;
    }

    TEST_ASSERT_TRUE(chunk_count >= 2);                    /* pagination effective */
    TEST_ASSERT_EQUAL_UINT16(meshpay_dag_count(src), cursor); /* couverture [0,count) */
    TEST_ASSERT_EQUAL_UINT32(meshpay_dag_count(src),
                             meshpay_dag_count(dst));     /* recepteur complet */
}


/* Le SUMMARY porte les 8 premiers octets du dag_digest (detection de
 * convergence). Round-trip build->parse + deux DAG de contenus differents
 * donnent des digests differents. */
TEST_CASE("dag sync summary carries the dag digest", "[dag_sync]")
{
    meshpay_dag_t *full = test_pool_dag(0);
    meshpay_dag_t *slow = test_pool_dag(1);
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(full, slow, &tx0, &tx1, &tx2);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0xaa);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(full, source, &packet));
    meshpay_dag_sync_summary_t summary;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_parse_summary(&packet, &summary));
    TEST_ASSERT_TRUE(summary.has_digest);

    uint8_t expected[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(full, expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, summary.digest,
                             MESHPAY_DAG_SYNC_DIGEST_SIZE);

    /* DAG de contenu different (slow = 1 tx) => digest different. */
    rns_packet_t packet_slow;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(slow, source, &packet_slow));
    meshpay_dag_sync_summary_t summary_slow;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_parse_summary(&packet_slow, &summary_slow));
    TEST_ASSERT_TRUE(summary_slow.has_digest);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(summary.digest, summary_slow.digest,
                                    MESHPAY_DAG_SYNC_DIGEST_SIZE));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Durcissement ingestion (I3) — gate injecté dans apply_batch
 * ══════════════════════════════════════════════════════════════════════════ */

#include "meshpay/currency.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/rns/rns_destination.h"

typedef struct {
    meshpay_currency_config_t *config;
    meshpay_dag_t *dag;
    unsigned calls;
} gate_ctx_t;

/* Gate de test : la vraie politique currency, plus un compteur d'appels
 * (vérifie que la table des verdicts évite les re-vérifications inutiles). */
static meshpay_dag_sync_gate_verdict_t test_tx_gate(void *raw,
                                                    const meshpay_tx_t *tx)
{
    gate_ctx_t *ctx = raw;
    ctx->calls++;
    meshpay_currency_result_t result =
        meshpay_currency_ingest_check(ctx->config, ctx->dag, tx);
    if (result == MESHPAY_CURRENCY_OK) {
        return MESHPAY_DAG_SYNC_GATE_ACCEPT;
    }
    if (result == MESHPAY_CURRENCY_ERR_UNKNOWN_MEMBER) {
        return MESHPAY_DAG_SYNC_GATE_RETRY;
    }
    return MESHPAY_DAG_SYNC_GATE_REJECT;
}

/* Batch volontairement retors : le TRANSFER du membre PRÉCÈDE sa CLAIM
 * (annuaire vide à la première passe → RETRY), et une tx forgée s'y glisse.
 * Attendu : les deux tx authentiques finissent mergées (multi-passes), la
 * forge est comptée skipped_invalid, rien n'est fatal. */
TEST_CASE("dag sync gated batch retries transfer until claim lands and drops forgeries",
          "[dag_sync][i3]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    strncpy(body.name, "Gate", sizeof(body.name) - 1);
    strncpy(body.symbol, "GAT", sizeof(body.symbol) - 1);
    body.initial_credit = 8;
    body.transfer_fee = 0;
    body.created_at_ms = 1000;
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    rns_identity_t member;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&member));
    rns_destination_t member_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&member,
                                                            &member_wallet));
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder,
                                                            &founder_wallet));

    meshpay_tx_t claim;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_claim(&claim, &member,
                                              member_wallet.hash, 8,
                                              config.currency_id, NULL, 0,
                                              2000));
    meshpay_tx_t pay;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&pay, &member,
                                                 member_wallet.hash,
                                                 founder_wallet.hash, 3, 1, 0,
                                                 config.currency_id, NULL, 0,
                                                 3000));
    /* Forge : TRANSFER depuis le compte du membre, signature bidon. */
    meshpay_tx_t forged = pay;
    forged.seq = 2;
    fill_sequence(forged.id, sizeof(forged.id), 0xE0);
    fill_sequence(forged.signature, sizeof(forged.signature), 0xE8);

    meshpay_tx_t ordered[3] = { pay, forged, claim };
    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    encode_batch_manual(ordered, 3, batch, sizeof(batch), &batch_len);

    meshpay_dag_t *dag = test_pool_dag(0);
    gate_ctx_t ctx = { .config = &config, .dag = dag, .calls = 0 };
    size_t merged = 0;
    size_t skipped_foreign = 0;
    size_t skipped_invalid = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch_gated(
                          dag, batch, batch_len, &config.currency_id,
                          test_tx_gate, &ctx, &merged, &skipped_foreign,
                          &skipped_invalid));
    TEST_ASSERT_EQUAL_UINT32(2, merged);
    TEST_ASSERT_EQUAL_UINT32(0, skipped_foreign);
    TEST_ASSERT_EQUAL_UINT32(1, skipped_invalid);
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, claim.id));
    TEST_ASSERT_TRUE(meshpay_dag_contains(dag, pay.id));
    TEST_ASSERT_FALSE(meshpay_dag_contains(dag, forged.id));
    /* Économie des verdicts : pay 2× (RETRY sans annuaire, puis ACCEPT),
     * forge 2× (RETRY tant que la clé de M est inconnue — indécidable —,
     * puis REJECT une fois la CLAIM mergée), claim 1× → 5 appels, pas
     * 3 × nombre de passes. */
    TEST_ASSERT_EQUAL_UINT32(5, ctx.calls);

    /* Le résidu transitoire est compté : un TRANSFER orphelin (membre jamais
     * réclamé dans CE batch) reste dehors sans bloquer. */
    rns_identity_t ghost;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&ghost));
    rns_destination_t ghost_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&ghost,
                                                            &ghost_wallet));
    meshpay_tx_t orphan;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&orphan, &ghost,
                                                 ghost_wallet.hash,
                                                 founder_wallet.hash, 2, 1, 0,
                                                 config.currency_id, NULL, 0,
                                                 4000));
    meshpay_tx_t only[1] = { orphan };
    encode_batch_manual(only, 1, batch, sizeof(batch), &batch_len);
    merged = 0;
    skipped_invalid = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_apply_batch_gated(
                          dag, batch, batch_len, &config.currency_id,
                          test_tx_gate, &ctx, &merged, NULL,
                          &skipped_invalid));
    TEST_ASSERT_EQUAL_UINT32(0, merged);
    TEST_ASSERT_EQUAL_UINT32(1, skipped_invalid);
    TEST_ASSERT_FALSE(meshpay_dag_contains(dag, orphan.id));
}
