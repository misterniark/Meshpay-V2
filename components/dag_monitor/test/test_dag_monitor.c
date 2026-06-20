#include "meshpay/dag_monitor.h"

#include "meshpay/dag_sync.h"
#include "meshpay/rns/rns_link_request.h"
#include "unity.h"

#include <string.h>

static meshpay_dag_monitor_t s_monitor;
static meshpay_dag_t s_dag;
static meshpay_tx_t s_tx0;
static meshpay_tx_t s_tx1;
static rns_packet_t s_packet;
static rns_packet_t s_packets[RNS_RESOURCE_MAX_FRAGMENTS];
static meshpay_dag_monitor_snapshot_t s_snapshot;
static uint8_t s_wire[RNS_PACKET_MTU];
static rns_lora_fragment_t s_fragments[RNS_LORA_MAX_FRAGMENTS];
static uint8_t s_frame[RNS_LORA_MAX_FRAME_SIZE];

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void make_tx(meshpay_tx_t *tx,
                    uint8_t id_seed,
                    uint32_t seq,
                    const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                    uint8_t parent_count)
{
    meshpay_tx_clear(tx);
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    tx->type = seq == 0 ? MESHPAY_TX_TYPE_MINT : MESHPAY_TX_TYPE_TRANSFER;
    fill_sequence(tx->from, sizeof(tx->from), 0x10);
    fill_sequence(tx->to, sizeof(tx->to), 0x40);
    tx->amount = 10 + seq;
    tx->seq = seq;
    tx->currency_id = 1;
    tx->timestamp_ms = 1000 + seq;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x40));
}

static void record_packet_over_lora(meshpay_dag_monitor_t *monitor,
                                    const rns_packet_t *packet,
                                    uint64_t now_ms)
{
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_packet_pack(packet,
                                      s_wire,
                                      sizeof(s_wire),
                                      &wire_len));

    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_iface_lora_fragment_packet(s_wire,
                                                     wire_len,
                                                     RNS_LORA_MAX_FRAME_SIZE,
                                                     s_fragments,
                                                     RNS_LORA_MAX_FRAGMENTS,
                                                     &fragment_count));
    for (size_t i = 0; i < fragment_count; ++i) {
        size_t frame_len = 0;
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_iface_lora_pack_fragment(&s_fragments[i],
                                                       s_frame,
                                                       sizeof(s_frame),
                                                       &frame_len));
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_dag_monitor_record_lora_frame(monitor,
                                                                s_frame,
                                                                frame_len,
                                                                now_ms + i));
    }
}

TEST_CASE("dag monitor records lora-only summaries without transmitting",
          "[dag_monitor]")
{
    meshpay_dag_monitor_t *monitor = &s_monitor;
    meshpay_dag_monitor_init(monitor);

    meshpay_dag_t *dag = &s_dag;
    meshpay_dag_init(dag);
    meshpay_tx_t *tx = &s_tx0;
    make_tx(tx, 0x21, 0, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, tx));

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0x90);
    rns_packet_t *summary = &s_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(dag, source, summary));

    record_packet_over_lora(monitor, summary, 1000);

    meshpay_dag_monitor_snapshot_t *snapshot = &s_snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_monitor_snapshot(monitor, snapshot));
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->lora_frames);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->rns_packets);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->dag_summaries);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->peer_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->tx_advertised);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot->tx_observed);
    TEST_ASSERT_EQUAL_UINT16(1, snapshot->peers[0].tx_count);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot->peers[0].tip_count);
    TEST_ASSERT_EQUAL_UINT8(100, snapshot->health_score);
}

TEST_CASE("dag monitor counts repeated summary transmissions as fresh",
          "[dag_monitor]")
{
    meshpay_dag_monitor_t *monitor = &s_monitor;
    meshpay_dag_monitor_init(monitor);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0x92);
    rns_packet_t *packet = &s_packet;
    rns_packet_clear(packet);
    packet->destination_type = RNS_DESTINATION_TYPE_PLAIN;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    memcpy(packet->destination_hash, source, sizeof(source));
    packet->data[0] = MESHPAY_DAG_SYNC_MSG_SUMMARY;
    packet->data[1] = 0;
    packet->data[2] = 2;
    packet->data[3] = 1;
    fill_sequence(&packet->data[4], MESHPAY_TX_ID_SIZE, 0xc0);
    packet->data_len = 4 + MESHPAY_TX_ID_SIZE;

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_monitor_record_packet(monitor, packet, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_monitor_record_packet(monitor, packet, 16000));

    meshpay_dag_monitor_snapshot_t *snapshot = &s_snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_monitor_snapshot(monitor, snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->rns_packets);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->dag_summaries);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot->duplicate_packets);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->tx_advertised);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->peers[0].summaries);
}

TEST_CASE("dag monitor flags peer summary count regression", "[dag_monitor]")
{
    meshpay_dag_monitor_t *monitor = &s_monitor;
    meshpay_dag_monitor_init(monitor);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0x91);
    rns_packet_t *packet = &s_packet;
    rns_packet_clear(packet);
    packet->destination_type = RNS_DESTINATION_TYPE_PLAIN;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    memcpy(packet->destination_hash, source, sizeof(source));
    packet->data[0] = MESHPAY_DAG_SYNC_MSG_SUMMARY;
    packet->data[1] = 0;
    packet->data[2] = 3;
    packet->data[3] = 1;
    fill_sequence(&packet->data[4], MESHPAY_TX_ID_SIZE, 0xb0);
    packet->data_len = 4 + MESHPAY_TX_ID_SIZE;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_monitor_record_packet(monitor, packet, 1000));

    packet->data[2] = 2;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_monitor_record_packet(monitor, packet, 2000));

    meshpay_dag_monitor_snapshot_t *snapshot = &s_snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_monitor_snapshot(monitor, snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->dag_summaries);
    TEST_ASSERT_EQUAL_UINT32(3, snapshot->tx_advertised);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->peer_regressions);
    TEST_ASSERT_TRUE(snapshot->health_score < 100);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->alert_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->alert_next);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MONITOR_ALERT_PEER_TX_COUNT_REGRESSED,
                      snapshot->alerts[0].type);
}

TEST_CASE("dag monitor inspects resource batches without merging into a dag",
          "[dag_monitor]")
{
    meshpay_dag_monitor_t *monitor = &s_monitor;
    meshpay_dag_monitor_init(monitor);

    meshpay_dag_t *dag = &s_dag;
    meshpay_dag_init(dag);
    meshpay_tx_t *tx0 = &s_tx0;
    make_tx(tx0, 0x31, 0, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, tx0));
    uint8_t parent[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parent[0], tx0->id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t *tx1 = &s_tx1;
    make_tx(tx1, 0x51, 1, parent, 1);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, tx1));

    rns_link_t link;
    rns_link_clear(&link);
    link.status = RNS_LINK_STATUS_ACTIVE;
    link.mtu = RNS_PACKET_MTU;
    fill_sequence(link.link_id, sizeof(link.link_id), 0xa0);

    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_batch_resource(dag,
                                                            0,
                                                            &link,
                                                            s_packets,
                                                            RNS_RESOURCE_MAX_FRAGMENTS,
                                                            &packet_count));
    TEST_ASSERT_TRUE(packet_count > 0);
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_dag_monitor_record_packet(monitor,
                                                            &s_packets[i],
                                                            3000 + i));
    }

    meshpay_dag_monitor_snapshot_t *snapshot = &s_snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_monitor_snapshot(monitor, snapshot));
    TEST_ASSERT_EQUAL_UINT32(packet_count, snapshot->resource_frames);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->dag_batches);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot->tx_observed);
}

TEST_CASE("dag monitor counts malformed lora frames", "[dag_monitor]")
{
    meshpay_dag_monitor_t *monitor = &s_monitor;
    meshpay_dag_monitor_init(monitor);

    const uint8_t bad_frame[] = {0x00, 0x01, 0x02};
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_dag_monitor_record_lora_frame(monitor,
                                                                bad_frame,
                                                                sizeof(bad_frame),
                                                                4000));

    meshpay_dag_monitor_snapshot_t *snapshot = &s_snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_monitor_snapshot(monitor, snapshot));
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->lora_frames);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->malformed_lora_frames);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->alert_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot->alert_next);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MONITOR_ALERT_MALFORMED_LORA,
                      snapshot->alerts[0].type);
}
