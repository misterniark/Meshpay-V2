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
