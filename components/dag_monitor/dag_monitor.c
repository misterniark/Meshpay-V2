#include "meshpay/dag_monitor.h"

#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_crypto.h"
#include <string.h>

static uint16_t get_u16(const uint8_t *in)
{
    return ((uint16_t)in[0] << 8) | in[1];
}

static bool hash_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    return rns_crypto_constant_equal(a, b, len);
}

static bool hash_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static void update_health(meshpay_dag_monitor_t *monitor)
{
    int score = 100;
    const meshpay_dag_monitor_snapshot_t *s = &monitor->snapshot;

    score -= (int)s->malformed_lora_frames * 4;
    score -= (int)s->malformed_rns_packets * 6;
    score -= (int)s->malformed_dag_sync * 5;
    score -= (int)s->peer_regressions * 12;
    score -= (int)s->peer_summary_without_tips * 4;
    score -= (int)s->duplicate_packets;
    if (score < 0) {
        score = 0;
    }
    monitor->snapshot.health_score = (uint8_t)score;
}

static void push_alert(meshpay_dag_monitor_t *monitor,
                       meshpay_dag_monitor_alert_level_t level,
                       meshpay_dag_monitor_alert_type_t type,
                       const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
                       uint32_t value,
                       uint64_t now_ms)
{
    meshpay_dag_monitor_alert_t *alert =
        &monitor->snapshot.alerts[monitor->alert_next];
    memset(alert, 0, sizeof(*alert));
    alert->level = level;
    alert->type = type;
    alert->seen_ms = now_ms;
    alert->value = value;
    if (destination != NULL) {
        memcpy(alert->destination,
               destination,
               MESHPAY_TX_DESTINATION_HASH_SIZE);
    }

    monitor->alert_next =
        (monitor->alert_next + 1U) % MESHPAY_DAG_MONITOR_ALERT_MAX;
    if (monitor->snapshot.alert_count < MESHPAY_DAG_MONITOR_ALERT_MAX) {
        monitor->snapshot.alert_count++;
    }
}

static bool duplicate_seen(const meshpay_dag_monitor_t *monitor,
                           const uint8_t hash[RNS_CRYPTO_SHA256_SIZE])
{
    for (size_t i = 0; i < monitor->duplicate_count; ++i) {
        if (hash_equal(monitor->duplicate_hashes[i],
                       hash,
                       RNS_CRYPTO_SHA256_SIZE)) {
            return true;
        }
    }
    return false;
}

static void duplicate_remember(meshpay_dag_monitor_t *monitor,
                               const uint8_t hash[RNS_CRYPTO_SHA256_SIZE])
{
    memcpy(monitor->duplicate_hashes[monitor->duplicate_next],
           hash,
           RNS_CRYPTO_SHA256_SIZE);
    monitor->duplicate_next =
        (monitor->duplicate_next + 1U) % MESHPAY_DAG_MONITOR_DUP_CACHE;
    if (monitor->duplicate_count < MESHPAY_DAG_MONITOR_DUP_CACHE) {
        monitor->duplicate_count++;
    }
}

static bool packet_can_repeat_identically(const rns_packet_t *packet)
{
    if (packet == NULL) {
        return false;
    }
    if (packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE) {
        return true;
    }
    return packet->packet_type == RNS_PACKET_TYPE_DATA &&
           packet->data_len > 0 &&
           packet->data[0] == MESHPAY_DAG_SYNC_MSG_SUMMARY;
}

static meshpay_dag_monitor_peer_t *find_or_add_peer(
    meshpay_dag_monitor_t *monitor,
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint64_t now_ms)
{
    if (destination == NULL ||
        hash_zero(destination, MESHPAY_TX_DESTINATION_HASH_SIZE)) {
        return NULL;
    }

    meshpay_dag_monitor_peer_t *free_slot = NULL;
    meshpay_dag_monitor_peer_t *oldest = &monitor->snapshot.peers[0];
    for (size_t i = 0; i < MESHPAY_DAG_MONITOR_MAX_PEERS; ++i) {
        meshpay_dag_monitor_peer_t *peer = &monitor->snapshot.peers[i];
        if (peer->in_use &&
            hash_equal(peer->destination,
                       destination,
                       MESHPAY_TX_DESTINATION_HASH_SIZE)) {
            peer->last_seen_ms = now_ms;
            return peer;
        }
        if (!peer->in_use && free_slot == NULL) {
            free_slot = peer;
        }
        if (peer->last_seen_ms < oldest->last_seen_ms) {
            oldest = peer;
        }
    }

    meshpay_dag_monitor_peer_t *slot = free_slot != NULL ? free_slot : oldest;
    if (!slot->in_use &&
        monitor->snapshot.peer_count < MESHPAY_DAG_MONITOR_MAX_PEERS) {
        monitor->snapshot.peer_count++;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->last_seen_ms = now_ms;
    memcpy(slot->destination, destination, MESHPAY_TX_DESTINATION_HASH_SIZE);
    return slot;
}

static void record_announce(meshpay_dag_monitor_t *monitor,
                            const rns_packet_t *packet,
                            uint64_t now_ms)
{
    monitor->snapshot.announces++;
    meshpay_dag_monitor_peer_t *peer = find_or_add_peer(
        monitor,
        packet->destination_hash,
        now_ms);
    if (peer != NULL) {
        peer->announces++;
    }
}

static void record_summary(meshpay_dag_monitor_t *monitor,
                           const rns_packet_t *packet,
                           uint64_t now_ms)
{
    meshpay_dag_sync_summary_t summary;
    esp_err_t err = meshpay_dag_sync_parse_summary(packet, &summary);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_dag_sync++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_WARN,
                   MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC,
                   packet->destination_hash,
                   (uint32_t)packet->data_len,
                   now_ms);
        return;
    }

    monitor->snapshot.dag_summaries++;
    meshpay_dag_monitor_peer_t *peer = find_or_add_peer(
        monitor,
        packet->destination_hash,
        now_ms);
    if (peer == NULL) {
        return;
    }

    if (peer->summaries > 0 && summary.tx_count < peer->tx_count) {
        monitor->snapshot.peer_regressions++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_WARN,
                   MESHPAY_DAG_MONITOR_ALERT_PEER_TX_COUNT_REGRESSED,
                   packet->destination_hash,
                   summary.tx_count,
                   now_ms);
    }
    if (summary.tx_count > 0 && summary.tip_count == 0) {
        monitor->snapshot.peer_summary_without_tips++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_WARN,
                   MESHPAY_DAG_MONITOR_ALERT_PEER_SUMMARY_WITHOUT_TIPS,
                   packet->destination_hash,
                   summary.tx_count,
                   now_ms);
    }

    peer->summaries++;
    peer->tx_count = summary.tx_count;
    peer->tip_count = summary.tip_count;
    memcpy(peer->tips, summary.tips, sizeof(peer->tips));
    if (summary.tx_count > monitor->snapshot.tx_advertised) {
        monitor->snapshot.tx_advertised = summary.tx_count;
    }
}

static void record_request(meshpay_dag_monitor_t *monitor,
                           const rns_packet_t *packet,
                           uint64_t now_ms)
{
    uint16_t known_count = 0;
    esp_err_t err = meshpay_dag_sync_request_known_count(packet, &known_count);
    if (err != ESP_OK) {
        monitor->snapshot.unknown_packets++;
        return;
    }

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    bool has_source = false;
    (void)meshpay_dag_sync_request_source(packet, source, &has_source);

    monitor->snapshot.dag_requests++;
    meshpay_dag_monitor_peer_t *peer = find_or_add_peer(
        monitor,
        has_source ? source : packet->destination_hash,
        now_ms);
    if (peer != NULL) {
        peer->requests++;
        if (known_count > peer->tx_count) {
            peer->tx_count = known_count;
        }
    }
}

static esp_err_t inspect_dag_batch(const uint8_t *batch,
                                   size_t batch_len,
                                   uint16_t *tx_count)
{
    if (batch == NULL || batch_len < 2 || tx_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *tx_count = 0;
    uint16_t count = get_u16(batch);
    size_t pos = 2;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 2U > batch_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint16_t encoded_len = get_u16(batch + pos);
        pos += 2;
        if (encoded_len == 0 || pos + encoded_len > batch_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        meshpay_tx_t tx;
        esp_err_t err = meshpay_tx_decode(batch + pos, encoded_len, &tx);
        if (err != ESP_OK) {
            return err;
        }
        pos += encoded_len;
    }
    if (pos != batch_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    *tx_count = count;
    return ESP_OK;
}

static void record_resource(meshpay_dag_monitor_t *monitor,
                            const rns_packet_t *packet,
                            uint64_t now_ms)
{
    monitor->snapshot.resource_frames++;

    uint8_t batch[MESHPAY_DAG_SYNC_BATCH_MAX_SIZE];
    size_t batch_len = 0;
    bool complete = false;
    esp_err_t err = rns_resource_reassembler_accept(
        &monitor->resource_reassembler,
        packet,
        batch,
        sizeof(batch),
        &batch_len,
        &complete);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_dag_sync++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_WARN,
                   MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC,
                   packet->destination_hash,
                   (uint32_t)packet->data_len,
                   now_ms);
        return;
    }
    if (!complete) {
        return;
    }

    uint16_t batch_tx_count = 0;
    err = inspect_dag_batch(batch, batch_len, &batch_tx_count);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_dag_sync++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_WARN,
                   MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC,
                   packet->destination_hash,
                   (uint32_t)batch_len,
                   now_ms);
        return;
    }
    monitor->snapshot.dag_batches++;
    monitor->snapshot.tx_observed += batch_tx_count;
}

void meshpay_dag_monitor_init(meshpay_dag_monitor_t *monitor)
{
    if (monitor == NULL) {
        return;
    }
    memset(monitor, 0, sizeof(*monitor));
    rns_iface_lora_reassembler_init(&monitor->lora_reassembler);
    rns_resource_reassembler_init(&monitor->resource_reassembler);
    monitor->snapshot.health_score = 100;
}

esp_err_t meshpay_dag_monitor_record_packet(
    meshpay_dag_monitor_t *monitor,
    const rns_packet_t *packet,
    uint64_t now_ms)
{
    if (monitor == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_packet_hash(packet, packet_hash);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_rns_packets++;
        update_health(monitor);
        return err;
    }
    const bool repeatable = packet_can_repeat_identically(packet);
    if (!repeatable && duplicate_seen(monitor, packet_hash)) {
        monitor->snapshot.duplicate_packets++;
        update_health(monitor);
        return ESP_OK;
    }
    if (!repeatable) {
        duplicate_remember(monitor, packet_hash);
    }

    monitor->snapshot.rns_packets++;
    if (packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE) {
        record_announce(monitor, packet, now_ms);
    } else if (packet->packet_type == RNS_PACKET_TYPE_DATA &&
               packet->data_len > 0 &&
               packet->data[0] == MESHPAY_DAG_SYNC_MSG_SUMMARY) {
        record_summary(monitor, packet, now_ms);
    } else if (packet->packet_type == RNS_PACKET_TYPE_DATA &&
               packet->context == RNS_PACKET_CONTEXT_REQUEST) {
        record_request(monitor, packet, now_ms);
    } else if (packet->packet_type == RNS_PACKET_TYPE_DATA &&
               packet->context == RNS_PACKET_CONTEXT_RESOURCE) {
        record_resource(monitor, packet, now_ms);
    } else {
        monitor->snapshot.unknown_packets++;
    }

    update_health(monitor);
    return ESP_OK;
}

esp_err_t meshpay_dag_monitor_record_lora_frame(
    meshpay_dag_monitor_t *monitor,
    const uint8_t *frame,
    size_t frame_len,
    uint64_t now_ms)
{
    if (monitor == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    monitor->snapshot.lora_frames++;
    uint8_t wire[RNS_PACKET_MTU];
    size_t wire_len = 0;
    bool complete = false;
    esp_err_t err = rns_iface_lora_reassembler_accept(
        &monitor->lora_reassembler,
        frame,
        frame_len,
        wire,
        sizeof(wire),
        &wire_len,
        &complete);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_lora_frames++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_CRIT,
                   MESHPAY_DAG_MONITOR_ALERT_MALFORMED_LORA,
                   NULL,
                   (uint32_t)frame_len,
                   now_ms);
        update_health(monitor);
        return err;
    }
    if (!complete) {
        update_health(monitor);
        return ESP_OK;
    }

    rns_packet_t packet;
    err = rns_packet_unpack(wire, wire_len, &packet);
    if (err != ESP_OK) {
        monitor->snapshot.malformed_rns_packets++;
        push_alert(monitor,
                   MESHPAY_DAG_MONITOR_ALERT_CRIT,
                   MESHPAY_DAG_MONITOR_ALERT_MALFORMED_PACKET,
                   NULL,
                   (uint32_t)wire_len,
                   now_ms);
        update_health(monitor);
        return err;
    }
    return meshpay_dag_monitor_record_packet(monitor, &packet, now_ms);
}

esp_err_t meshpay_dag_monitor_snapshot(
    const meshpay_dag_monitor_t *monitor,
    meshpay_dag_monitor_snapshot_t *snapshot)
{
    if (monitor == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = monitor->snapshot;
    snapshot->alert_next = monitor->alert_next;
    return ESP_OK;
}
