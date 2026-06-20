#pragma once

#include "esp_err.h"
#include "meshpay/dag_sync.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_iface_lora.h"
#include "meshpay/rns/rns_packet.h"
#include "meshpay/rns/rns_resource.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_DAG_MONITOR_MAX_PEERS 16
#define MESHPAY_DAG_MONITOR_ALERT_MAX 12
#define MESHPAY_DAG_MONITOR_DUP_CACHE 32

typedef enum {
    MESHPAY_DAG_MONITOR_ALERT_INFO = 0,
    MESHPAY_DAG_MONITOR_ALERT_WARN,
    MESHPAY_DAG_MONITOR_ALERT_CRIT,
} meshpay_dag_monitor_alert_level_t;

typedef enum {
    MESHPAY_DAG_MONITOR_ALERT_MALFORMED_LORA = 0,
    MESHPAY_DAG_MONITOR_ALERT_MALFORMED_PACKET,
    MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC,
    MESHPAY_DAG_MONITOR_ALERT_PEER_TX_COUNT_REGRESSED,
    MESHPAY_DAG_MONITOR_ALERT_PEER_SUMMARY_WITHOUT_TIPS,
} meshpay_dag_monitor_alert_type_t;

typedef struct {
    bool in_use;
    uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint64_t last_seen_ms;
    uint16_t tx_count;
    uint8_t tip_count;
    uint8_t tips[MESHPAY_DAG_SYNC_MAX_TIPS][MESHPAY_TX_ID_SIZE];
    uint32_t announces;
    uint32_t summaries;
    uint32_t requests;
} meshpay_dag_monitor_peer_t;

typedef struct {
    meshpay_dag_monitor_alert_level_t level;
    meshpay_dag_monitor_alert_type_t type;
    uint64_t seen_ms;
    uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint32_t value;
} meshpay_dag_monitor_alert_t;

typedef struct {
    uint32_t lora_frames;
    uint32_t rns_packets;
    uint32_t malformed_lora_frames;
    uint32_t malformed_rns_packets;
    uint32_t malformed_dag_sync;
    uint32_t duplicate_packets;
    uint32_t announces;
    uint32_t dag_summaries;
    uint32_t dag_requests;
    uint32_t resource_frames;
    uint32_t dag_batches;
    uint32_t tx_advertised;
    uint32_t tx_observed;
    uint32_t unknown_packets;
    uint32_t peer_regressions;
    uint32_t peer_summary_without_tips;
    uint8_t health_score;
    size_t peer_count;
    meshpay_dag_monitor_peer_t peers[MESHPAY_DAG_MONITOR_MAX_PEERS];
    size_t alert_count;
    size_t alert_next;
    meshpay_dag_monitor_alert_t alerts[MESHPAY_DAG_MONITOR_ALERT_MAX];
} meshpay_dag_monitor_snapshot_t;

typedef struct {
    rns_lora_reassembler_t lora_reassembler;
    rns_resource_reassembler_t resource_reassembler;
    uint8_t duplicate_hashes[MESHPAY_DAG_MONITOR_DUP_CACHE][RNS_CRYPTO_SHA256_SIZE];
    size_t duplicate_count;
    size_t duplicate_next;
    meshpay_dag_monitor_snapshot_t snapshot;
    size_t alert_next;
} meshpay_dag_monitor_t;

void meshpay_dag_monitor_init(meshpay_dag_monitor_t *monitor);
esp_err_t meshpay_dag_monitor_record_lora_frame(
    meshpay_dag_monitor_t *monitor,
    const uint8_t *frame,
    size_t frame_len,
    uint64_t now_ms);
esp_err_t meshpay_dag_monitor_record_packet(
    meshpay_dag_monitor_t *monitor,
    const rns_packet_t *packet,
    uint64_t now_ms);
esp_err_t meshpay_dag_monitor_snapshot(
    const meshpay_dag_monitor_t *monitor,
    meshpay_dag_monitor_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
