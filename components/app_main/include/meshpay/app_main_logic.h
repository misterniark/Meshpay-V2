#pragma once

#include "esp_err.h"
#include "meshpay/dag_sync.h"
#include "meshpay/payment_engine.h"
#include "meshpay/storage.h"
#include "meshpay/ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_APP_UI_TASK_NAME "ui_task"
#define MESHPAY_APP_RETICULUM_TASK_NAME "reticulum_task"
#define MESHPAY_APP_CORE_TASK_NAME "core_task"

#define MESHPAY_APP_QUEUE_DEFAULT_LENGTH 8
#define MESHPAY_APP_TASK_STACK_WORDS 8192
#define MESHPAY_APP_TASK_PRIORITY 5
#define MESHPAY_APP_LEGACY_ALIAS "MeshPayV2"

typedef enum {
    MESHPAY_APP_QUEUE_UI = 0,
    MESHPAY_APP_QUEUE_RETICULUM,
    MESHPAY_APP_QUEUE_CORE,
} meshpay_app_queue_id_t;

typedef enum {
    MESHPAY_APP_EVENT_NONE = 0,
    MESHPAY_APP_EVENT_STOP,
    MESHPAY_APP_EVENT_UI_REFRESH,
    MESHPAY_APP_EVENT_RETICULUM_RX,
    MESHPAY_APP_EVENT_RETICULUM_TX,
    MESHPAY_APP_EVENT_CORE_ANNOUNCE,
    MESHPAY_APP_EVENT_CORE_PAYMENT,
    MESHPAY_APP_EVENT_CORE_DAG_SUMMARY,
} meshpay_app_event_type_t;

typedef struct {
    meshpay_app_event_type_t type;
    uint64_t now_ms;
    uint32_t amount;
    uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE];
    rns_packet_t packet;
} meshpay_app_event_t;

typedef esp_err_t (*meshpay_app_runtime_packet_tx_cb_t)(
    const rns_packet_t *packet,
    void *ctx);

typedef struct {
    meshpay_currency_config_t currency;
    meshpay_dag_t dag;
    meshpay_wallet_t wallet;
    meshpay_ui_state_t ui;
    meshpay_payment_engine_t payments;
    rns_identity_t identity;
    bool announced;
    uint8_t local_destination[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t queue_depth_core;
    uint8_t queue_depth_reticulum;
    uint8_t queue_depth_ui;
} meshpay_app_t;

typedef struct {
    uint8_t ui_queue_length;
    uint8_t reticulum_queue_length;
    uint8_t core_queue_length;
    uint32_t ui_stack_words;
    uint32_t reticulum_stack_words;
    uint32_t core_stack_words;
    UBaseType_t ui_priority;
    UBaseType_t reticulum_priority;
    UBaseType_t core_priority;
} meshpay_app_runtime_config_t;

typedef struct {
    meshpay_app_t *app;
    meshpay_app_runtime_config_t config;
    QueueHandle_t ui_queue;
    QueueHandle_t reticulum_queue;
    QueueHandle_t core_queue;
    SemaphoreHandle_t lock;
    TaskHandle_t ui_task;
    TaskHandle_t reticulum_task;
    TaskHandle_t core_task;
    bool tasks_started;
    rns_resource_reassembler_t dag_sync_reassembler;
    uint32_t dag_sync_merged;
    meshpay_storage_backend_t storage_backend;
    meshpay_storage_record_t storage_record;
    bool has_storage;
    meshpay_app_runtime_packet_tx_cb_t packet_tx;
    void *packet_tx_ctx;
    uint64_t dag_sync_quiet_until_ms;
    uint32_t processed_ui;
    uint32_t processed_reticulum;
    uint32_t processed_core;
} meshpay_app_runtime_t;

esp_err_t meshpay_app_init(meshpay_app_t *app,
                           const uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE],
                           const rns_identity_t *identity,
                           const meshpay_currency_config_t *currency,
                           uint32_t next_seq,
                           bool has_pin);
esp_err_t meshpay_app_seed_tx(meshpay_app_t *app, const meshpay_tx_t *tx);
esp_err_t meshpay_app_announce(meshpay_app_t *app);
esp_err_t meshpay_app_pay(meshpay_app_t *sender,
                          meshpay_app_t *receiver,
                          uint32_t amount,
                          uint64_t now_ms);
esp_err_t meshpay_app_bootstrap_identity(
    const meshpay_storage_backend_t *backend,
    const char *default_alias,
    rns_identity_t *identity,
    meshpay_storage_record_t *record,
    bool *created);
esp_err_t meshpay_app_generate_alias(char *out, size_t out_len);
bool meshpay_app_alias_needs_generation(const char *alias);
esp_err_t meshpay_app_ensure_record_alias(
    const meshpay_storage_backend_t *backend,
    meshpay_storage_record_t *record);

meshpay_app_runtime_config_t meshpay_app_runtime_default_config(void);
esp_err_t meshpay_app_runtime_init(meshpay_app_runtime_t *runtime,
                                   meshpay_app_t *app,
                                   const meshpay_app_runtime_config_t *config);
void meshpay_app_runtime_destroy(meshpay_app_runtime_t *runtime);
esp_err_t meshpay_app_runtime_set_packet_tx(
    meshpay_app_runtime_t *runtime,
    meshpay_app_runtime_packet_tx_cb_t packet_tx,
    void *ctx);
esp_err_t meshpay_app_runtime_set_storage(
    meshpay_app_runtime_t *runtime,
    const meshpay_storage_backend_t *backend,
    const meshpay_storage_record_t *record);
esp_err_t meshpay_app_runtime_start_tasks(meshpay_app_runtime_t *runtime);
esp_err_t meshpay_app_runtime_stop_tasks(meshpay_app_runtime_t *runtime);
esp_err_t meshpay_app_runtime_post(meshpay_app_runtime_t *runtime,
                                   meshpay_app_queue_id_t queue_id,
                                   const meshpay_app_event_t *event,
                                   TickType_t timeout_ticks);
esp_err_t meshpay_app_runtime_process_one(meshpay_app_runtime_t *runtime,
                                          meshpay_app_queue_id_t queue_id,
                                          TickType_t timeout_ticks);
UBaseType_t meshpay_app_runtime_queue_depth(const meshpay_app_runtime_t *runtime,
                                            meshpay_app_queue_id_t queue_id);

#ifdef __cplusplus
}
#endif
