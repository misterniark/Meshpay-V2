#include "meshpay/dag_sync.h"
#include "unity.h"
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
    meshpay_dag_t full;
    meshpay_dag_t slow;
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(&full, &slow, &tx0, &tx1, &tx2);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0xaa);
    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(&full, source, &packet));
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
    meshpay_dag_t full;
    meshpay_dag_t slow;
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(&full, &slow, &tx0, &tx1, &tx2);

    uint8_t zero[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(peer, sizeof(peer), 0xbb);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_summary(&full,
                                                     zero,
                                                     &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_request(&slow,
                                                     zero,
                                                     &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_sync_build_request_from(&slow,
                                                          peer,
                                                          zero,
                                                          &packet));
}

TEST_CASE("dag sync catches up missing transactions through resource batch", "[dag_sync]")
{
    meshpay_dag_t full;
    meshpay_dag_t slow;
    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    build_dags(&full, &slow, &tx0, &tx1, &tx2);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(&slow));

    uint8_t peer[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(peer, sizeof(peer), 0xbb);
    rns_packet_t request;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_request(&slow, peer, &request));
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
                      meshpay_dag_sync_build_request_from(&slow,
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
                      meshpay_dag_sync_build_batch_resource(&full,
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
                      meshpay_dag_sync_apply_batch(&slow, batch, batch_len,
                                                   &merged));
    TEST_ASSERT_EQUAL_UINT32(2, merged);
    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(&slow));
    TEST_ASSERT_TRUE(meshpay_dag_contains(&slow, tx2.id));
}
