#pragma once

#include "esp_err.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/dag_store.h"
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

#define MESHPAY_APP_QUEUE_DEFAULT_LENGTH 16
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

/*
 * État de la machine à états de REJOINTE de monnaie (Palier B4). Dérivé (non
 * stocké) depuis runtime->join_armed et app->currency.has_descriptor : la
 * vérité durable reste has_descriptor (survit au reboot via le storage).
 */
typedef enum {
    MESHPAY_APP_JOIN_IDLE = 0, /* ni armé ni membre : aucune rejointe en cours */
    MESHPAY_APP_JOIN_ARMED,    /* ancre posée, en attente d'un OFFER matchant  */
    MESHPAY_APP_JOIN_MEMBER,   /* membre actif d'une monnaie à descripteur     */
} meshpay_app_join_state_t;

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
    rns_resource_reassembler_pool_t dag_sync_reassembler_pool;
    uint32_t dag_sync_merged;
    uint16_t dag_sync_send_offset; /* rotation de la fenetre d'envoi de batch */
    meshpay_dag_store_backend_t dag_store; /* persistance durable de la DAG */
    bool dag_store_ready;                   /* backend dag_store installe */
    bool dag_dirty;                         /* DAG modifiee depuis le dernier save */
    uint64_t dag_saved_ms;                  /* horodatage du dernier save reussi */
    meshpay_storage_backend_t storage_backend;
    meshpay_storage_record_t storage_record;
    bool has_storage;
    meshpay_app_runtime_packet_tx_cb_t packet_tx;
    void *packet_tx_ctx;
    uint64_t dag_sync_quiet_until_ms;
    /* --- Rejointe de monnaie (Palier B4) : état d'orchestration RX transitoire.
     * pending_anchor = préfixe de genèse saisi hors-bande (code d'invitation).
     * join_armed_until_ms est posé mais le désarmement sur timeout est différé. */
    bool join_armed;
    uint8_t pending_anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t pending_anchor_len;
    uint64_t join_armed_until_ms;
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

/*
 * Palier A5 — détermine la config monnaie EFFECTIVE au boot, avec repli sûr :
 *  - si le record porte un descripteur de monnaie valide (blob décodé PUIS
 *    vérifié), out_config est dérivé du descripteur (durcissement MINT actif) ;
 *  - sinon (pas de descripteur, ou blob illisible/invalide), out_config = repli
 *    (la config fournie, p.ex. codée en dur) — aucune régression du wallet.
 * out_from_descriptor (optionnel) indique le chemin pris. Renvoie toujours une
 * config utilisable (ESP_OK), sauf arguments NULL (ESP_ERR_INVALID_ARG).
 */
esp_err_t meshpay_app_currency_from_record(
    const meshpay_storage_record_t *record,
    const meshpay_currency_config_t *fallback,
    meshpay_currency_config_t *out_config,
    bool *out_from_descriptor);
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
/* Installe le backend de persistance DAG. Active la sauvegarde durable de la
 * fenetre DAG (flush debounce + flush force apres un commit local). */
esp_err_t meshpay_app_runtime_set_dag_store(
    meshpay_app_runtime_t *runtime,
    const meshpay_dag_store_backend_t *backend);
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

/* ======================================================================== */
/* Palier B4 — machine à états de rejointe de monnaie (headless)            */
/* ======================================================================== */

/*
 * Arme la rejointe depuis une ANCRE brute, qui DOIT faire exactement
 * MESHPAY_CURRENCY_INVITE_ANCHOR_LEN octets (l'ancre du code d'invitation) —
 * toute autre longueur est rejetée (ESP_ERR_INVALID_ARG). Mono-monnaie STRICT :
 * rejette avec ESP_ERR_INVALID_STATE si le device est déjà membre
 * (has_descriptor). Sur succès, l'état passe à ARMED : le prochain OFFER matchant
 * l'ancre sera importé. now_ms horodate la fenêtre (désarmement sur timeout
 * différé).
 */
esp_err_t meshpay_app_runtime_arm_join_anchor(meshpay_app_runtime_t *runtime,
                                              const uint8_t *anchor,
                                              size_t anchor_len,
                                              uint64_t now_ms);

/*
 * Arme la rejointe depuis un CODE d'invitation (base32 Crockford). Décode le
 * code (meshpay_currency_invite_decode) puis délègue à _arm_join_anchor. Propage
 * l'erreur de décodage (checksum/alphabet/longueur) et le rejet mono-monnaie.
 */
esp_err_t meshpay_app_runtime_arm_join(meshpay_app_runtime_t *runtime,
                                       const char *invite_code,
                                       uint64_t now_ms);

/*
 * Émet UNE requête de descripteur (REQUEST 0x33, broadcast) via le callback
 * packet_tx : currency_id dérivé de l'ancre armée, source = adresse locale.
 * Rejette (ESP_ERR_INVALID_STATE) si non armé, déjà membre, ou packet_tx absent.
 * La cadence périodique (retries) est du ressort du câblage FreeRTOS (main/).
 */
esp_err_t meshpay_app_runtime_emit_join_request(meshpay_app_runtime_t *runtime,
                                                uint64_t now_ms);

/* État courant de la rejointe (dérivé de join_armed + has_descriptor). */
meshpay_app_join_state_t meshpay_app_runtime_join_state(
    const meshpay_app_runtime_t *runtime);

/*
 * Palier B5 — produit le CODE D'INVITATION affichable de la monnaie détenue par
 * ce device (décode le descripteur stocké puis meshpay_currency_invite_encode).
 * Destiné à l'UI (le fondateur l'affiche). Rejette ESP_ERR_INVALID_STATE si le
 * device n'est membre d'aucune monnaie (pas de descripteur en storage).
 * `out` doit pouvoir contenir MESHPAY_CURRENCY_INVITE_CODE_BUF octets.
 */
esp_err_t meshpay_app_runtime_invite_code(meshpay_app_runtime_t *runtime,
                                          char *out,
                                          size_t out_size);

/*
 * Palier C4 — auto-crédit initial (CLAIM). Si le device est membre d'une monnaie
 * à descripteur dont initial_credit > 0 et que sa DAG ne contient AUCUNE CLAIM
 * `from == moi` (la garde « déjà réclamé » est le DAG lui-même, persisté via
 * dag_store), construit la CLAIM réflexive (montant = initial_credit, seq = 0,
 * parents = tips courants), la valide (plafond max_supply), la committe
 * localement (commit-on-send) et force la persistance. La propagation vers les
 * pairs passe par la sync DAG (SUMMARY/REQUEST/BATCH), pas par un envoi direct.
 *
 * Idempotent : ESP_OK sans effet si non-membre, crédit nul, ou CLAIM déjà
 * présente. ESP_ERR_INVALID_STATE si la validation refuse (plafond épuisé).
 * À appeler au boot (main/) après la restauration de la DAG ; appelée aussi
 * automatiquement à la rejointe (import réussi du descripteur).
 */
esp_err_t meshpay_app_runtime_claim_initial_credit(meshpay_app_runtime_t *runtime,
                                                   uint64_t now_ms);

#ifdef __cplusplus
}
#endif
