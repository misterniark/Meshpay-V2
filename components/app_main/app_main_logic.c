#include "meshpay/app_main_logic.h"

#include "esp_check.h"
#include "esp_log.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/descriptor_sync.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ALIAS_ANIMALS[] = {
    "renard",  "loup",    "lynx",    "ours",    "cerf",
    "aigle",   "faucon",  "castor",  "bison",   "hibou",
    "heron",   "otarie",  "orque",   "dauphin", "panda",
    "tigre",   "lion",    "panthere","lama",    "koala",
    "toucan",  "lezard",  "phoque",  "beluga",  "chamois",
    "morse",   "zebre",   "tapir",   "yak",     "corbeau",
};

static const char *const ALIAS_QUALITIES[] = {
    "malin",   "sage",    "vif",       "calme",    "brave",
    "doux",    "fier",    "alerte",    "loyal",    "joyeux",
    "rapide",  "patient", "curieux",   "solide",   "agile",
    "clair",   "ruse",    "vaillant",  "paisible", "precis",
    "sobre",   "tenace",  "lucide",    "stable",   "franc",
    "habile",  "ardent",  "discret",   "fiable",   "radieux",
};

static const char *APP_RUNTIME_TAG = "app_runtime";

#define ALIAS_ANIMAL_COUNT (sizeof(ALIAS_ANIMALS) / sizeof(ALIAS_ANIMALS[0]))
#define ALIAS_QUALITY_COUNT \
    (sizeof(ALIAS_QUALITIES) / sizeof(ALIAS_QUALITIES[0]))

static QueueHandle_t runtime_select_queue(const meshpay_app_runtime_t *runtime,
                                          meshpay_app_queue_id_t queue_id)
{
    if (runtime == NULL) {
        return NULL;
    }
    switch (queue_id) {
    case MESHPAY_APP_QUEUE_UI:
        return runtime->ui_queue;
    case MESHPAY_APP_QUEUE_RETICULUM:
        return runtime->reticulum_queue;
    case MESHPAY_APP_QUEUE_CORE:
        return runtime->core_queue;
    default:
        return NULL;
    }
}

static void runtime_refresh_depths(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->app == NULL) {
        return;
    }
    runtime->app->queue_depth_ui =
        (uint8_t)uxQueueMessagesWaiting(runtime->ui_queue);
    runtime->app->queue_depth_reticulum =
        (uint8_t)uxQueueMessagesWaiting(runtime->reticulum_queue);
    runtime->app->queue_depth_core =
        (uint8_t)uxQueueMessagesWaiting(runtime->core_queue);
}

static bool runtime_known_is_local(const meshpay_app_t *app,
                                   const rns_announce_known_destination_t *known)
{
    return app == NULL || known == NULL ||
           rns_destination_hash_equal(known->destination_hash,
                                      app->local_destination);
}

static void runtime_peer_label_from_hash(
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
    char out[MESHPAY_UI_PEER_LABEL_MAX])
{
    if (out == NULL) {
        return;
    }
    if (destination == NULL) {
        (void)snprintf(out, MESHPAY_UI_PEER_LABEL_MAX, "pair inconnu");
        return;
    }
    (void)snprintf(out,
                   MESHPAY_UI_PEER_LABEL_MAX,
                   "pair %02x%02x",
                   destination[0],
                   destination[1]);
}

static void runtime_peer_label_from_known(
    const rns_announce_known_destination_t *known,
    char out[MESHPAY_UI_PEER_LABEL_MAX])
{
    if (out == NULL) {
        return;
    }
    out[0] = '\0';
    if (known == NULL) {
        runtime_peer_label_from_hash(NULL, out);
        return;
    }

    if (known->app_data_len > 0) {
        size_t len = known->app_data_len;
        if (len >= MESHPAY_UI_PEER_LABEL_MAX) {
            len = MESHPAY_UI_PEER_LABEL_MAX - 1U;
        }
        for (size_t i = 0; i < len; ++i) {
            uint8_t ch = known->app_data[i];
            out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        }
        out[len] = '\0';
    }

    if (out[0] == '\0') {
        runtime_peer_label_from_hash(known->destination_hash, out);
    }
}

static const rns_announce_known_destination_t *runtime_known_peer_at(
    const meshpay_app_t *app,
    uint8_t selected_index,
    uint8_t *peer_count)
{
    uint8_t count = 0;
    const rns_announce_known_destination_t *selected = NULL;
    size_t known_count = rns_announce_known_count();
    for (size_t i = 0; i < known_count; ++i) {
        const rns_announce_known_destination_t *known =
            rns_announce_known_get(i);
        if (runtime_known_is_local(app, known)) {
            continue;
        }
        if (count == selected_index) {
            selected = known;
        }
        count++;
    }
    if (peer_count != NULL) {
        *peer_count = count;
    }
    return selected;
}

static void runtime_refresh_payment_peer(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->app == NULL) {
        return;
    }

    uint8_t peer_count = 0;
    uint8_t selected_index = runtime->app->ui.selected_payment_peer;
    const rns_announce_known_destination_t *known =
        runtime_known_peer_at(runtime->app, selected_index, &peer_count);
    if (peer_count == 0) {
        (void)meshpay_ui_set_payment_peer(&runtime->app->ui, "", 0, 0);
        return;
    }
    if (known == NULL) {
        selected_index = 0;
        known = runtime_known_peer_at(runtime->app, selected_index, NULL);
    }

    char label[MESHPAY_UI_PEER_LABEL_MAX];
    runtime_peer_label_from_known(known, label);
    (void)meshpay_ui_set_payment_peer(&runtime->app->ui,
                                      label,
                                      selected_index,
                                      peer_count);
}

static void runtime_set_history_peer(
    meshpay_app_runtime_t *runtime,
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (runtime == NULL || runtime->app == NULL || destination == NULL) {
        return;
    }

    char label[MESHPAY_UI_PEER_LABEL_MAX];
    const rns_announce_known_destination_t *known =
        rns_announce_recall(destination);
    if (known != NULL) {
        runtime_peer_label_from_known(known, label);
    } else {
        runtime_peer_label_from_hash(destination, label);
    }
    (void)meshpay_ui_set_history_peer(&runtime->app->ui, label);
}

static void runtime_refresh_known_peers(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->app == NULL) {
        return;
    }

    size_t peers = 0;
    size_t known_count = rns_announce_known_count();
    for (size_t i = 0; i < known_count; ++i) {
        const rns_announce_known_destination_t *known =
            rns_announce_known_get(i);
        if (known != NULL &&
            !rns_destination_hash_equal(known->destination_hash,
                                        runtime->app->local_destination)) {
            peers++;
            /* Sous une config à DESCRIPTEUR, l'autorité MINT est figée (fondateur
             * unique) : ne JAMAIS ajouter un pair annoncé comme autorité, sinon
             * le durcissement single-authority serait annulé au 1er announce
             * (Palier B4). Sans descripteur (config de repli), on garde l'ancien
             * comportement « maillage ouvert ». */
            if (!runtime->app->currency.has_descriptor) {
                (void)meshpay_currency_add_mint_authority(
                    &runtime->app->currency,
                    known->destination_hash);
            }
        }
    }
    runtime->app->ui.network_peers =
        peers > UINT8_MAX ? UINT8_MAX : (uint8_t)peers;
    runtime_refresh_payment_peer(runtime);
}

esp_err_t meshpay_app_generate_alias(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t random[2] = {0};
    ESP_RETURN_ON_ERROR(rns_crypto_random(random, sizeof(random)),
                        "app_main",
                        "");
    int written = snprintf(out,
                           out_len,
                           "%s %s",
                           ALIAS_ANIMALS[random[0] % ALIAS_ANIMAL_COUNT],
                           ALIAS_QUALITIES[random[1] % ALIAS_QUALITY_COUNT]);
    rns_crypto_secure_zero(random, sizeof(random));
    if (written < 0 || (size_t)written >= out_len) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bool meshpay_app_alias_needs_generation(const char *alias)
{
    return alias == NULL || alias[0] == '\0' ||
           strcmp(alias, MESHPAY_APP_LEGACY_ALIAS) == 0;
}

esp_err_t meshpay_app_ensure_record_alias(
    const meshpay_storage_backend_t *backend,
    meshpay_storage_record_t *record)
{
    if (backend == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!meshpay_app_alias_needs_generation(record->alias)) {
        return ESP_OK;
    }

    char alias[MESHPAY_STORAGE_ALIAS_MAX] = {0};
    ESP_RETURN_ON_ERROR(meshpay_app_generate_alias(alias, sizeof(alias)),
                        "app_main",
                        "");
    ESP_RETURN_ON_ERROR(meshpay_storage_record_set_alias(record, alias),
                        "app_main",
                        "");
    return meshpay_storage_save(backend, record);
}

esp_err_t meshpay_app_init(meshpay_app_t *app,
                           const uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE],
                           const rns_identity_t *identity,
                           const meshpay_currency_config_t *currency,
                           uint32_t next_seq,
                           bool has_pin)
{
    if (app == NULL || owner == NULL || identity == NULL || currency == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(app, 0, sizeof(*app));
    memcpy(&app->identity, identity, sizeof(app->identity));
    memcpy(&app->currency, currency, sizeof(app->currency));
    memcpy(app->local_destination, owner, sizeof(app->local_destination));

    meshpay_dag_init(&app->dag);
    ESP_RETURN_ON_ERROR(meshpay_wallet_init(&app->wallet, owner, next_seq),
                        "app_main", "");
    meshpay_ui_init(&app->ui, has_pin);
    ESP_RETURN_ON_ERROR(meshpay_payment_engine_init(&app->payments,
                                                   &app->wallet,
                                                   &app->dag,
                                                   &app->currency,
                                                   &app->identity),
                        "app_main", "");
    return ESP_OK;
}

esp_err_t meshpay_app_currency_from_record(
    const meshpay_storage_record_t *record,
    const meshpay_currency_config_t *fallback,
    meshpay_currency_config_t *out_config,
    bool *out_from_descriptor)
{
    if (record == NULL || fallback == NULL || out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_from_descriptor != NULL) {
        *out_from_descriptor = false;
    }
    /* Repli par défaut : la config fournie (codée en dur). On la pose d'emblée
     * pour qu'un chemin d'échec laisse toujours une config utilisable. */
    *out_config = *fallback;

    /* Pas de descripteur stocké -> device « vierge » -> on garde le repli. */
    if (!record->has_currency_descriptor ||
        record->currency_descriptor_len == 0) {
        return ESP_OK;
    }

    /* Décodage du blob CBOR opaque, puis vérification cryptographique. Tout
     * échec retombe sur le repli (descripteur corrompu/illisible ne doit pas
     * empêcher le boot). */
    meshpay_currency_descriptor_signed_t signed_desc;
    if (meshpay_currency_descriptor_decode(record->currency_descriptor,
                                           record->currency_descriptor_len,
                                           &signed_desc) != ESP_OK) {
        ESP_LOGW("app_main", "descripteur monnaie stocké illisible -> repli");
        return ESP_OK;
    }
    if (meshpay_currency_descriptor_verify(&signed_desc) != ESP_OK) {
        ESP_LOGW("app_main", "descripteur monnaie stocké invalide -> repli");
        return ESP_OK;
    }

    meshpay_currency_config_t derived;
    if (meshpay_currency_config_from_descriptor(&derived, &signed_desc) !=
        ESP_OK) {
        ESP_LOGW("app_main", "dérivation config depuis descripteur échouée -> repli");
        return ESP_OK;
    }

    *out_config = derived;
    if (out_from_descriptor != NULL) {
        *out_from_descriptor = true;
    }
    return ESP_OK;
}

esp_err_t meshpay_app_seed_tx(meshpay_app_t *app, const meshpay_tx_t *tx)
{
    if (app == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_dag_merge_result_t result = meshpay_dag_merge_tx(&app->dag, tx);
    return (result == MESHPAY_DAG_MERGE_OK ||
            result == MESHPAY_DAG_MERGE_DUPLICATE)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

esp_err_t meshpay_app_announce(meshpay_app_t *app)
{
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    app->announced = true;
    app->queue_depth_reticulum++;
    return ESP_OK;
}

esp_err_t meshpay_app_pay(meshpay_app_t *sender,
                          meshpay_app_t *receiver,
                          uint32_t amount,
                          uint64_t now_ms)
{
    if (sender == NULL || receiver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_packet_t payment_packet;
    ESP_RETURN_ON_ERROR(meshpay_payment_engine_create_payment(
                            &sender->payments,
                            receiver->local_destination,
                            amount, now_ms, &payment_packet),
                        "app_main", "");
    sender->queue_depth_core++;
    sender->queue_depth_reticulum++;
    (void)meshpay_ui_on_payment_feedback(&sender->ui,
                                         sender->payments.feedback,
                                         amount);

    rns_packet_t ack_packet;
    ESP_RETURN_ON_ERROR(meshpay_payment_engine_receive_payment(
                            &receiver->payments,
                            &payment_packet,
                            now_ms + 1,
                            &ack_packet),
                        "app_main", "");
    receiver->queue_depth_core++;
    receiver->queue_depth_reticulum++;
    (void)meshpay_ui_on_payment_feedback(&receiver->ui,
                                         receiver->payments.feedback,
                                         amount);

    ESP_RETURN_ON_ERROR(meshpay_payment_engine_receive_ack(&sender->payments,
                                                           &ack_packet),
                        "app_main", "");
    (void)meshpay_ui_on_payment_feedback(&sender->ui,
                                         sender->payments.feedback,
                                         amount);
    return ESP_OK;
}

esp_err_t meshpay_app_bootstrap_identity(
    const meshpay_storage_backend_t *backend,
    const char *default_alias,
    rns_identity_t *identity,
    meshpay_storage_record_t *record,
    bool *created)
{
    if (backend == NULL || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_storage_record_t loaded;
    esp_err_t err = meshpay_storage_load(backend, &loaded);
    if (err == ESP_OK) {
        if (!loaded.has_identity) {
            rns_crypto_secure_zero(&loaded, sizeof(loaded));
            return ESP_ERR_INVALID_STATE;
        }
        err = rns_identity_load_private(identity, loaded.identity_private);
        if (err == ESP_OK && record != NULL) {
            memcpy(record, &loaded, sizeof(*record));
        }
        if (created != NULL) {
            *created = false;
        }
        rns_crypto_secure_zero(&loaded, sizeof(loaded));
        return err;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    meshpay_storage_record_t fresh;
    meshpay_storage_record_init(&fresh);
    fresh.next_seq = 1;
    err = rns_identity_generate(identity);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(&fresh, sizeof(fresh));
        return err;
    }

    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    err = rns_identity_get_private_key(identity, private_key);
    if (err == ESP_OK) {
        err = meshpay_storage_record_set_identity(&fresh, private_key);
    }
    rns_crypto_secure_zero(private_key, sizeof(private_key));
    if (err == ESP_OK && default_alias != NULL && default_alias[0] != '\0') {
        err = meshpay_storage_record_set_alias(&fresh, default_alias);
    }
    if (err == ESP_OK) {
        err = meshpay_storage_save(backend, &fresh);
    }
    if (err != ESP_OK) {
        rns_crypto_secure_zero(&fresh, sizeof(fresh));
        rns_identity_clear(identity);
        return err;
    }

    if (record != NULL) {
        memcpy(record, &fresh, sizeof(*record));
    }
    if (created != NULL) {
        *created = true;
    }
    rns_crypto_secure_zero(&fresh, sizeof(fresh));
    return ESP_OK;
}

meshpay_app_runtime_config_t meshpay_app_runtime_default_config(void)
{
    return (meshpay_app_runtime_config_t){
        .ui_queue_length = MESHPAY_APP_QUEUE_DEFAULT_LENGTH,
        .reticulum_queue_length = MESHPAY_APP_QUEUE_DEFAULT_LENGTH,
        .core_queue_length = MESHPAY_APP_QUEUE_DEFAULT_LENGTH,
        .ui_stack_words = MESHPAY_APP_TASK_STACK_WORDS,
        .reticulum_stack_words = MESHPAY_APP_TASK_STACK_WORDS,
        .core_stack_words = MESHPAY_APP_TASK_STACK_WORDS,
        .ui_priority = MESHPAY_APP_TASK_PRIORITY,
        .reticulum_priority = MESHPAY_APP_TASK_PRIORITY,
        .core_priority = MESHPAY_APP_TASK_PRIORITY,
    };
}

static esp_err_t runtime_config_effective(
    meshpay_app_runtime_config_t *out,
    const meshpay_app_runtime_config_t *config)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = config == NULL ? meshpay_app_runtime_default_config() : *config;
    if (out->ui_queue_length == 0 || out->reticulum_queue_length == 0 ||
        out->core_queue_length == 0 || out->ui_stack_words == 0 ||
        out->reticulum_stack_words == 0 || out->core_stack_words == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t runtime_persist_wallet_state(meshpay_app_runtime_t *runtime);
/* Adaptateur du hook de persistance de l'engine de paiement (void* -> runtime). */
static esp_err_t runtime_persist_cb(void *ctx)
{
    return runtime_persist_wallet_state((meshpay_app_runtime_t *)ctx);
}

esp_err_t meshpay_app_runtime_init(meshpay_app_runtime_t *runtime,
                                   meshpay_app_t *app,
                                   const meshpay_app_runtime_config_t *config)
{
    if (runtime == NULL || app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_app_runtime_config_t effective;
    ESP_RETURN_ON_ERROR(runtime_config_effective(&effective, config),
                        "app_runtime", "");

    memset(runtime, 0, sizeof(*runtime));
    runtime->app = app;
    runtime->config = effective;
    runtime->lock = xSemaphoreCreateMutex();
    runtime->ui_queue = xQueueCreate(effective.ui_queue_length,
                                     sizeof(meshpay_app_event_t));
    runtime->reticulum_queue = xQueueCreate(effective.reticulum_queue_length,
                                            sizeof(meshpay_app_event_t));
    runtime->core_queue = xQueueCreate(effective.core_queue_length,
                                       sizeof(meshpay_app_event_t));
    if (runtime->lock == NULL || runtime->ui_queue == NULL ||
        runtime->reticulum_queue == NULL || runtime->core_queue == NULL) {
        meshpay_app_runtime_destroy(runtime);
        return ESP_ERR_NO_MEM;
    }
    rns_resource_reassembler_pool_init(&runtime->dag_sync_reassembler_pool);
    runtime->dag_sync_send_offset = 0;

    /* Option A : la persistance de next_seq doit précéder tout commit DAG d'un
     * paiement. On branche le hook de l'engine sur le runtime (anti-réutilisation
     * de seq au reboot, la DAG étant en RAM). */
    meshpay_payment_engine_set_persist(&runtime->app->payments,
                                       runtime_persist_cb, runtime);

    runtime_refresh_depths(runtime);
    return ESP_OK;
}

void meshpay_app_runtime_destroy(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    if (runtime->tasks_started) {
        (void)meshpay_app_runtime_stop_tasks(runtime);
    }
    if (runtime->ui_queue != NULL) {
        vQueueDelete(runtime->ui_queue);
    }
    if (runtime->reticulum_queue != NULL) {
        vQueueDelete(runtime->reticulum_queue);
    }
    if (runtime->core_queue != NULL) {
        vQueueDelete(runtime->core_queue);
    }
    if (runtime->lock != NULL) {
        vSemaphoreDelete(runtime->lock);
    }
    memset(runtime, 0, sizeof(*runtime));
}

esp_err_t meshpay_app_runtime_set_packet_tx(
    meshpay_app_runtime_t *runtime,
    meshpay_app_runtime_packet_tx_cb_t packet_tx,
    void *ctx)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime->packet_tx = packet_tx;
    runtime->packet_tx_ctx = ctx;
    return ESP_OK;
}

esp_err_t meshpay_app_runtime_set_storage(
    meshpay_app_runtime_t *runtime,
    const meshpay_storage_backend_t *backend,
    const meshpay_storage_record_t *record)
{
    if (runtime == NULL || backend == NULL || record == NULL ||
        backend->write_blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime->storage_backend = *backend;
    memcpy(&runtime->storage_record, record, sizeof(runtime->storage_record));
    runtime->has_storage = true;
    return ESP_OK;
}

esp_err_t meshpay_app_runtime_set_dag_store(
    meshpay_app_runtime_t *runtime,
    const meshpay_dag_store_backend_t *backend)
{
    if (runtime == NULL || backend == NULL || backend->read == NULL ||
        backend->write == NULL || backend->erase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime->dag_store = *backend;
    runtime->dag_store_ready = true;
    runtime->dag_dirty = false;
    runtime->dag_saved_ms = 0;
    return ESP_OK;
}

/* ======================================================================== */
/* Palier B4 — API publique de rejointe de monnaie                          */
/* ======================================================================== */

esp_err_t meshpay_app_runtime_arm_join_anchor(meshpay_app_runtime_t *runtime,
                                              const uint8_t *anchor,
                                              size_t anchor_len,
                                              uint64_t now_ms)
{
    /* L'ancre EST par définition celle du code d'invitation : exactement
     * ANCHOR_LEN octets. Refuser toute longueur partielle ferme d'emblée la
     * dérivation du currency_id sur des octets non fournis et empêche un préfixe
     * de correspondance trop court (matches_anchor). */
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL ||
        anchor == NULL || anchor_len != MESHPAY_CURRENCY_INVITE_ANCHOR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    /* Mono-monnaie STRICT : refuser d'armer une rejointe si déjà membre. */
    if (runtime->app->currency.has_descriptor) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        memcpy(runtime->pending_anchor, anchor, anchor_len);
        runtime->pending_anchor_len = anchor_len;
        runtime->join_armed = true;
        runtime->join_armed_until_ms = now_ms; /* fenêtre : désarmement différé */
    }
    xSemaphoreGive(runtime->lock);
    return err;
}

esp_err_t meshpay_app_runtime_arm_join(meshpay_app_runtime_t *runtime,
                                       const char *invite_code,
                                       uint64_t now_ms)
{
    if (runtime == NULL || invite_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Décode le code hors verrou (logique pure), puis arme sous verrou. */
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_currency_invite_decode(invite_code, anchor,
                                                       sizeof(anchor), &anchor_len),
                        "app_runtime", "");
    return meshpay_app_runtime_arm_join_anchor(runtime, anchor, anchor_len, now_ms);
}

esp_err_t meshpay_app_runtime_emit_join_request(meshpay_app_runtime_t *runtime,
                                                uint64_t now_ms)
{
    (void)now_ms; /* réservé pour un futur backoff de retries */
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (!runtime->join_armed || runtime->app->currency.has_descriptor ||
        runtime->packet_tx == NULL) {
        /* Rien à demander : pas armé, déjà membre, ou pas d'émetteur. */
        err = ESP_ERR_INVALID_STATE;
    } else {
        /* currency_id = 4 octets de tête de l'ancre (= genèse[0..3]). */
        uint32_t currency_id =
            ((uint32_t)runtime->pending_anchor[0] << 24) |
            ((uint32_t)runtime->pending_anchor[1] << 16) |
            ((uint32_t)runtime->pending_anchor[2] << 8) |
            ((uint32_t)runtime->pending_anchor[3]);
        rns_packet_t request;
        err = meshpay_descriptor_sync_build_request(
            currency_id, runtime->app->local_destination, &request);
        if (err == ESP_OK) {
            err = runtime->packet_tx(&request, runtime->packet_tx_ctx);
        }
    }
    xSemaphoreGive(runtime->lock);
    return err;
}

meshpay_app_join_state_t meshpay_app_runtime_join_state(
    const meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->app == NULL) {
        return MESHPAY_APP_JOIN_IDLE;
    }
    /* Vérité durable d'abord : membre si la config vient d'un descripteur. */
    if (runtime->app->currency.has_descriptor) {
        return MESHPAY_APP_JOIN_MEMBER;
    }
    return runtime->join_armed ? MESHPAY_APP_JOIN_ARMED : MESHPAY_APP_JOIN_IDLE;
}

esp_err_t meshpay_app_runtime_invite_code(meshpay_app_runtime_t *runtime,
                                          char *out,
                                          size_t out_size)
{
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL ||
        out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    /* Le code dérive du descripteur détenu : aucune monnaie -> aucun code. */
    if (!runtime->has_storage ||
        !runtime->storage_record.has_currency_descriptor) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        meshpay_currency_descriptor_signed_t signed_desc;
        err = meshpay_currency_descriptor_decode(
            runtime->storage_record.currency_descriptor,
            runtime->storage_record.currency_descriptor_len, &signed_desc);
        if (err == ESP_OK) {
            /* NB : encode ne prend pas le lock -> pas de re-entrée. */
            err = meshpay_currency_invite_encode(&signed_desc, out, out_size);
        }
    }
    xSemaphoreGive(runtime->lock);
    return err;
}

esp_err_t meshpay_app_runtime_post(meshpay_app_runtime_t *runtime,
                                   meshpay_app_queue_id_t queue_id,
                                   const meshpay_app_event_t *event,
                                   TickType_t timeout_ticks)
{
    if (runtime == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = runtime_select_queue(runtime, queue_id);
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(queue, event, timeout_ticks) != pdTRUE) {
        runtime_refresh_depths(runtime);
        return ESP_ERR_TIMEOUT;
    }
    runtime_refresh_depths(runtime);
    return ESP_OK;
}

UBaseType_t meshpay_app_runtime_queue_depth(const meshpay_app_runtime_t *runtime,
                                            meshpay_app_queue_id_t queue_id)
{
    QueueHandle_t queue = runtime_select_queue(runtime, queue_id);
    return queue == NULL ? 0 : uxQueueMessagesWaiting(queue);
}

static esp_err_t runtime_persist_wallet_state(meshpay_app_runtime_t *runtime);

/* Intervalle minimal entre deux sauvegardes DAG « débouncées » (anti-usure
 * flash). Un commit de paiement LOCAL force une sauvegarde immédiate hors de ce
 * débounce (cf. runtime_dag_flush(..., force=true)). */
#define MESHPAY_DAG_STORE_FLUSH_INTERVAL_MS 15000ULL

/* Marque la DAG comme modifiée : sera persistée au prochain flush (tick UI) ou
 * immédiatement si un commit local force le flush. */
static void runtime_dag_mark_dirty(meshpay_app_runtime_t *runtime)
{
    runtime->dag_dirty = true;
}

/* Persiste la fenêtre DAG si nécessaire. `force` contourne le débounce (utilisé
 * après un commit de paiement local, le cas le plus critique à ne pas perdre). */
static void runtime_dag_flush(meshpay_app_runtime_t *runtime, uint64_t now_ms,
                              bool force, const char *reason)
{
    if (!runtime->dag_store_ready || !runtime->dag_dirty) {
        return;
    }
    if (!force && now_ms >= runtime->dag_saved_ms &&
        now_ms - runtime->dag_saved_ms < MESHPAY_DAG_STORE_FLUSH_INTERVAL_MS) {
        return;
    }
    esp_err_t err =
        meshpay_dag_store_save(&runtime->dag_store, &runtime->app->dag, reason);
    if (err == ESP_OK) {
        runtime->dag_dirty = false;
        runtime->dag_saved_ms = now_ms;
    } else {
        ESP_LOGW(APP_RUNTIME_TAG, "dag store save err=%s",
                 esp_err_to_name(err));
    }
}

static esp_err_t runtime_process_ui(meshpay_app_runtime_t *runtime,
                                    const meshpay_app_event_t *event)
{
    if (event->type != MESHPAY_APP_EVENT_UI_REFRESH) {
        return event->type == MESHPAY_APP_EVENT_STOP ? ESP_ERR_INVALID_STATE
                                                     : ESP_OK;
    }

    uint32_t expired_amount = 0;
    bool expired = meshpay_payment_engine_expire_pending(
        &runtime->app->payments,
        event->now_ms,
        &expired_amount);
    if (expired) {
        (void)runtime_persist_wallet_state(runtime);
    }

    uint32_t balance = 0;
    esp_err_t err = meshpay_wallet_get_available_balance(
        &runtime->app->wallet,
        &runtime->app->currency,
        &runtime->app->dag,
        event->now_ms,
        &balance);
    if (err == ESP_OK) {
        err = meshpay_ui_set_balance(&runtime->app->ui, balance);
    }
    if (err == ESP_OK && expired) {
        err = meshpay_ui_on_payment_feedback(&runtime->app->ui,
                                             runtime->app->payments.feedback,
                                             expired_amount);
    }
    /* Flush débouncé de la DAG : persiste les tx accumulées (sync entrante)
     * sans écrire à chaque transaction (usure flash). */
    runtime_dag_flush(runtime, event->now_ms, false, "tick");
    if (err == ESP_OK) {
        runtime->processed_ui++;
    }
    return err;
}

static void runtime_make_dag_sync_link(
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_link_t *link)
{
    rns_link_clear(link);
    link->status = RNS_LINK_STATUS_ACTIVE;
    link->mtu = RNS_PACKET_MTU;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    memcpy(link->link_id, destination, RNS_DESTINATION_HASH_SIZE);
}

static esp_err_t runtime_refresh_balance(meshpay_app_runtime_t *runtime,
                                         uint64_t now_ms)
{
    uint32_t balance = 0;
    ESP_RETURN_ON_ERROR(meshpay_wallet_get_available_balance(
                            &runtime->app->wallet,
                            &runtime->app->currency,
                            &runtime->app->dag,
                            now_ms,
                            &balance),
                        "app_runtime", "");
    return meshpay_ui_set_balance(&runtime->app->ui, balance);
}

static esp_err_t runtime_persist_wallet_state(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->app == NULL || !runtime->has_storage) {
        return ESP_OK;
    }
    runtime->storage_record.next_seq = runtime->app->wallet.next_seq;
    if (runtime->app->wallet.has_pin) {
        ESP_RETURN_ON_ERROR(meshpay_storage_record_set_pin_hash(
                                &runtime->storage_record,
                                runtime->app->wallet.pin_hash),
                            "app_runtime", "");
    }
    return meshpay_storage_save(&runtime->storage_backend,
                                &runtime->storage_record);
}

static void runtime_report_payment_rejected(meshpay_app_runtime_t *runtime,
                                            uint32_t amount,
                                            uint64_t now_ms,
                                            esp_err_t reason)
{
    if (runtime == NULL || runtime->app == NULL) {
        return;
    }

    runtime->app->payments.feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
    (void)runtime_refresh_balance(runtime, now_ms);
    (void)meshpay_ui_on_payment_feedback(&runtime->app->ui,
                                         MESHPAY_PAYMENT_FEEDBACK_REJECTED,
                                         amount);
    ESP_LOGW(APP_RUNTIME_TAG,
             "payment rejected reason=%s amount=%u pending=%u lock=%u",
             esp_err_to_name(reason),
             (unsigned)amount,
             runtime->app->payments.has_pending ? 1U : 0U,
             meshpay_wallet_lock_active(&runtime->app->wallet, now_ms)
                 ? 1U
                 : 0U);
}

static esp_err_t runtime_handle_dag_summary(meshpay_app_runtime_t *runtime,
                                            const rns_packet_t *packet,
                                            uint64_t now_ms)
{
    meshpay_dag_sync_summary_t summary;
    esp_err_t err = meshpay_dag_sync_parse_summary(packet, &summary);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (rns_destination_hash_equal(packet->destination_hash,
                                   runtime->app->local_destination) ||
        runtime->packet_tx == NULL) {
        return ESP_OK;
    }

    bool tips_known = true;
    for (uint8_t i = 0; i < summary.tip_count; ++i) {
        if (!meshpay_dag_contains(&runtime->app->dag, summary.tips[i])) {
            tips_known = false;
            break;
        }
    }

    size_t local_count = meshpay_dag_count(&runtime->app->dag);

    /* Detection de convergence par DIGEST (8 o) : si le digest du pair == le
     * notre, les deux ENSEMBLES sont identiques -> rien a synchroniser. Sinon il
     * y a divergence -- y compris a compte egal (fork de meme taille) ou quand on
     * a un compte >= mais qu'il nous manque des tx de l'autre branche. Remplace
     * l'heuristique tips/count, qui ratait : (a) les tx interieures sous des
     * ordres d'insertion differents, (b) les forks de meme taille, et faisait
     * rester un noeud bloque a un MINT pres (observe au banc). */
    bool converged = false;
    if (summary.has_digest) {
        uint8_t local_digest[RNS_CRYPTO_SHA256_SIZE];
        if (meshpay_dag_digest(&runtime->app->dag, local_digest) == ESP_OK) {
            converged = memcmp(summary.digest, local_digest,
                               MESHPAY_DAG_SYNC_DIGEST_SIZE) == 0;
        }
    }
    ESP_LOGI(APP_RUNTIME_TAG,
             "dag summary rx from=%02x%02x%02x%02x tx=%u tips=%u local=%u conv=%u",
             packet->destination_hash[0],
             packet->destination_hash[1],
             packet->destination_hash[2],
             packet->destination_hash[3],
             (unsigned)summary.tx_count,
             (unsigned)summary.tip_count,
             (unsigned)local_count,
             converged ? 1U : 0U);
    if (converged) {
        return ESP_OK;
    }
    /* Filet de securite (pair sans digest, ancien firmware) : heuristique
     * d'origine. */
    if (!summary.has_digest && tips_known && summary.tx_count <= local_count) {
        return ESP_OK;
    }
    /* Backoff anti-tempete : ne pas re-demander une sync a chaque summary de
     * chaque pair tant qu'une requete recente est encore "en vol". */
    if (now_ms < runtime->dag_sync_quiet_until_ms) {
        return ESP_OK;
    }

    /* known=0 TOUJOURS : le decoupage du batch cote repondeur est POSITIONNEL,
     * or les ordres d'insertion different entre noeuds (chaque noeud append dans
     * son ordre de reception). Seul start=0 garantit d'inclure les tx qui nous
     * manquent ; les DUPLICATE sont ignorees a l'application. Un known positionnel
     * envoyait les mauvaises tx et laissait des noeuds bloques. (H1) */
    rns_packet_t request;
    ESP_RETURN_ON_ERROR(meshpay_dag_sync_build_request_from_count(
                            0,
                            packet->destination_hash,
                            runtime->app->local_destination,
                            &request),
                        "app_runtime", "");
    /* Jitter (50-550 ms) : echelonne la fenetre de silence des noeuds (backoff),
     * sans bloquer la tache (la requete part immediatement, cf. plus bas). */
    uint32_t request_delay_ms =
        50U + ((((uint32_t)runtime->app->local_destination[0] << 8) |
                 (uint32_t)runtime->app->local_destination[1]) %
                500U);
    ESP_LOGI(APP_RUNTIME_TAG,
             "dag request tx to=%02x%02x%02x%02x known=0 delay=%u",
             packet->destination_hash[0],
             packet->destination_hash[1],
             packet->destination_hash[2],
             packet->destination_hash[3],
             (unsigned)request_delay_ms);
    /* Backoff sans blocage : on arme la fenetre de silence (espace les requetes
     * suivantes) mais on envoie CELLE-CI immediatement. Un vTaskDelay ici gelait
     * la tache reticulum sous lock -> reticulum_queue saturee -> rx_queue saturee
     * -> fragments entrants (dont les batches attendus) dropes silencieusement. */
    runtime->dag_sync_quiet_until_ms = now_ms + request_delay_ms + 3000U;
    return runtime->packet_tx(&request, runtime->packet_tx_ctx);
}

static esp_err_t runtime_handle_dag_request(meshpay_app_runtime_t *runtime,
                                            const rns_packet_t *packet)
{
    uint16_t known_count = 0;
    esp_err_t err = meshpay_dag_sync_request_known_count(packet, &known_count);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t requester[MESHPAY_TX_DESTINATION_HASH_SIZE];
    bool has_requester = false;
    ESP_RETURN_ON_ERROR(meshpay_dag_sync_request_source(packet,
                                                        requester,
                                                        &has_requester),
                        "app_runtime", "");
    if (!has_requester || runtime->packet_tx == NULL ||
        known_count >= meshpay_dag_count(&runtime->app->dag)) {
        ESP_LOGI(APP_RUNTIME_TAG,
                 "dag request rx known=%u local=%u requester=%02x%02x%02x%02x send=0",
                 (unsigned)known_count,
                 (unsigned)meshpay_dag_count(&runtime->app->dag),
                 requester[0],
                 requester[1],
                 requester[2],
                 requester[3]);
        return ESP_OK;
    }

    rns_link_t link;
    runtime_make_dag_sync_link(requester, &link);
    rns_packet_t *packets = calloc(RNS_RESOURCE_MAX_FRAGMENTS,
                                   sizeof(*packets));
    if (packets == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Envoi PAGINE : la DAG peut depasser la capacite d'un seul batch (~29 tx)
     * alors que la fenetre est 250. On emet plusieurs Resource (chunks), chacun
     * reassemble en parallele cote recepteur grace au pool. Borne par requete
     * pour ne pas saturer la radio ; le reste suit au prochain cycle de sync. */
    uint16_t dag_count = (uint16_t)meshpay_dag_count(&runtime->app->dag);
    uint16_t base = known_count; /* < dag_count (garanti par le retour anticipe) */
    uint16_t span = (uint16_t)(dag_count - base);
    /* Offset rotatif (revue #1) : sous fork known=0 -> base=0, et la DAG peut
     * depasser MAX_CHUNKS_PER_REQUEST. Sans rotation on renverrait toujours les
     * memes premiers chunks ; la queue (ou resident les tips) ne serait jamais
     * transferee -> tips jamais connus -> stall permanent. On demarre a un offset
     * qui avance a chaque requete et couvre toute la plage [base, dag_count) au
     * fil des cycles. Petite DAG (<= MAX_CHUNKS) : une requete couvre tout ->
     * offset revient a 0 (comportement inchange, NO-OP). */
    uint16_t offset = runtime->dag_sync_send_offset;
    if (offset >= span) {
        offset = 0;
    }
    uint16_t cursor = (uint16_t)(base + offset);
    unsigned chunks = 0;
    for (; cursor < dag_count &&
           chunks < MESHPAY_DAG_SYNC_MAX_CHUNKS_PER_REQUEST;
         ++chunks) {
        size_t packet_count = 0;
        uint16_t next = cursor;
        err = meshpay_dag_sync_build_batch_resource_from(&runtime->app->dag,
                                                         cursor,
                                                         &link,
                                                         packets,
                                                         RNS_RESOURCE_MAX_FRAGMENTS,
                                                         &packet_count,
                                                         &next);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK) {
            free(packets);
            return err;
        }
        ESP_LOGI(APP_RUNTIME_TAG,
                 "dag resource tx packets=%u start=%u next=%u to=%02x%02x%02x%02x",
                 (unsigned)packet_count,
                 (unsigned)cursor,
                 (unsigned)next,
                 requester[0],
                 requester[1],
                 requester[2],
                 requester[3]);
        for (size_t i = 0; i < packet_count; ++i) {
            err = runtime->packet_tx(&packets[i], runtime->packet_tx_ctx);
            if (err != ESP_OK) {
                free(packets);
                return err;
            }
        }
        if (next <= cursor) { /* garde anti-boucle : progression obligatoire */
            break;
        }
        cursor = next;
    }
    /* Avance l'offset pour la prochaine requete ; revient a 0 quand la fin est
     * atteinte (toute la plage [base, dag_count) couverte au fil des requetes). */
    runtime->dag_sync_send_offset =
        (cursor >= dag_count) ? 0U : (uint16_t)(cursor - base);
    free(packets);
    return ESP_OK;
}

static esp_err_t runtime_handle_dag_resource(meshpay_app_runtime_t *runtime,
                                             const rns_packet_t *packet,
                                             uint64_t now_ms)
{
    uint8_t *batch = malloc(MESHPAY_DAG_SYNC_BATCH_MAX_SIZE);
    if (batch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t batch_len = 0;
    bool complete = false;
    esp_err_t err = rns_resource_reassembler_pool_accept(
        &runtime->dag_sync_reassembler_pool,
        packet,
        batch,
        MESHPAY_DAG_SYNC_BATCH_MAX_SIZE,
        &batch_len,
        &complete);
    if (err != ESP_OK) {
        ESP_LOGW(APP_RUNTIME_TAG,
                 "dag resource accept err=%s", esp_err_to_name(err));
        free(batch);
        return err;
    }
    if (!complete) {
        free(batch);
        return ESP_OK;
    }
    /* DIAG (confirmation cause racine fork) : trace le reassemblage COMPLET.
     * Distingue un echec de reassemblage (cette ligne absente alors que des
     * `dag resource tx` arrivent => H3') d'un echec d'application (cette ligne
     * presente + `apply err=...` ci-dessous => H2). */
    ESP_LOGI(APP_RUNTIME_TAG,
             "dag resource reassembled batch_len=%u", (unsigned)batch_len);

    size_t merged = 0;
    err = meshpay_dag_sync_apply_batch(&runtime->app->dag,
                                       batch,
                                       batch_len,
                                       &merged);
    free(batch);
    if (err != ESP_OK) {
        ESP_LOGW(APP_RUNTIME_TAG,
                 "dag resource apply err=%s batch_len=%u",
                 esp_err_to_name(err), (unsigned)batch_len);
        return err;
    }
    runtime->dag_sync_merged += (uint32_t)merged;
    if (merged > 0) {
        runtime_dag_mark_dirty(runtime); /* persisté au prochain tick (débounce) */
    }
    ESP_LOGI(APP_RUNTIME_TAG,
             "dag resource merged=%u total=%u",
             (unsigned)merged,
             (unsigned)meshpay_dag_count(&runtime->app->dag));
    return runtime_refresh_balance(runtime, now_ms);
}

static bool runtime_packet_is_local_payment_status(
    const meshpay_app_runtime_t *runtime,
    const rns_packet_t *packet)
{
    return runtime != NULL && runtime->app != NULL && packet != NULL &&
           (packet->packet_type == RNS_PACKET_TYPE_DATA ||
            packet->packet_type == RNS_PACKET_TYPE_PROOF) &&
           rns_destination_hash_equal(packet->destination_hash,
                                      runtime->app->local_destination) &&
           packet->data_len == 1U + MESHPAY_TX_ID_SIZE &&
           (packet->data[0] == MESHPAY_PAYMENT_MSG_ACK ||
            packet->data[0] == MESHPAY_PAYMENT_MSG_REJECT);
}

static bool runtime_packet_is_local_single_data(
    const meshpay_app_runtime_t *runtime,
    const rns_packet_t *packet)
{
    return runtime != NULL && runtime->app != NULL && packet != NULL &&
           packet->packet_type == RNS_PACKET_TYPE_DATA &&
           packet->destination_type == RNS_DESTINATION_TYPE_SINGLE &&
           rns_destination_hash_equal(packet->destination_hash,
                                      runtime->app->local_destination) &&
           packet->data_len > 0;
}

static bool runtime_packet_has_reject_status(const rns_packet_t *packet)
{
    return packet != NULL &&
           packet->data_len == 1U + MESHPAY_TX_ID_SIZE &&
           packet->data[0] == MESHPAY_PAYMENT_MSG_REJECT;
}

/* Filtre de FORME : vrai pour tout paquet DATA diffusé en clair (broadcast). Le
 * TYPE de message est porté par data[0] et discriminé par l'appelant : 0x31/0x32
 * (dag_sync summary/request), 0x33 (REQUEST de rejointe -> on sert l'OFFER si
 * membre, B5) et 0x34 (OFFER de rejointe -> import, B4). */
static bool runtime_packet_is_plain_broadcast(const rns_packet_t *packet)
{
    return packet != NULL &&
           packet->packet_type == RNS_PACKET_TYPE_DATA &&
           packet->destination_type == RNS_DESTINATION_TYPE_PLAIN &&
           packet->data_len > 0;
}

/*
 * Palier B4 — importe un descripteur DÉJÀ VÉRIFIÉ (signature + ancre contrôlées
 * par l'appelant) : dérive la config, RÉ-ENCODE le descripteur sous sa forme
 * CANONIQUE, la persiste sur le record DU RUNTIME puis applique la config EN
 * PLACE dans app->currency. On persiste le ré-encodage canonique (et non le wire
 * reçu tel quel) pour que deux pairs de la même monnaie détiennent des blobs
 * identiques octet à octet, quel que soit l'ordre des clés CBOR reçu.
 *
 * Atomique vis-à-vis du storage : un INSTANTANÉ COMPLET du record est pris avant
 * mutation et restauré INTÉGRALEMENT si le save échoue (le blob, la longueur ET
 * le flag), de sorte qu'un échec ne laisse jamais un record incohérent ; la
 * config n'est appliquée qu'après un save réussi. Exécuté sous le lock du runtime
 * (déjà tenu par process_one). N'alloue rien de lourd sur la pile (record ~570 o
 * + blob 384 o ; pas de meshpay_dag_t).
 */
static esp_err_t runtime_import_currency_descriptor(
    meshpay_app_runtime_t *runtime,
    const meshpay_currency_descriptor_signed_t *signed_desc)
{
    if (!runtime->has_storage) {
        /* Sans persistance, une rejointe ne survivrait pas au reboot : refus. */
        return ESP_ERR_INVALID_STATE;
    }

    /* Dérive la config depuis le descripteur (déjà vérifié par l'appelant). */
    meshpay_currency_config_t derived;
    ESP_RETURN_ON_ERROR(
        meshpay_currency_config_from_descriptor(&derived, signed_desc),
        "app_runtime", "");

    /* Ré-encodage CANONIQUE : indépendant de l'ordre des clés du wire reçu. */
    uint8_t blob[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t blob_len = 0;
    ESP_RETURN_ON_ERROR(
        meshpay_currency_descriptor_encode(signed_desc, blob, sizeof(blob),
                                           &blob_len),
        "app_runtime", "");

    /* Instantané COMPLET du record pour un rollback réellement atomique. */
    meshpay_storage_record_t snapshot = runtime->storage_record;

    ESP_RETURN_ON_ERROR(
        meshpay_storage_record_set_currency_descriptor(&runtime->storage_record,
                                                       blob, blob_len),
        "app_runtime", "");

    esp_err_t err = meshpay_storage_save(&runtime->storage_backend,
                                         &runtime->storage_record);
    if (err != ESP_OK) {
        /* Rollback intégral : le flash n'a pas été écrit, le record RAM doit
         * revenir EXACTEMENT à son état antérieur (aucun résidu de blob). */
        runtime->storage_record = snapshot;
        return err;
    }

    /* Applique la config EN PLACE : l'engine de paiement tient &app->currency,
     * la mutation est donc vue immédiatement sans ré-init. */
    runtime->app->currency = derived;
    return ESP_OK;
}

/*
 * Palier B5 — répond à une REQUEST de descripteur reçue (data[0] == 0x33). Tout
 * MEMBRE d'une monnaie peut servir son descripteur (pas seulement le fondateur) :
 * on décode le blob stocké et on le rediffuse en OFFER (PLAIN broadcast). Ne sert
 * QUE si l'on détient la monnaie demandée (currency_id). Non-bloquant, ESP_OK sur
 * tous les cas gérés. Exécuté sous le lock du runtime (déjà tenu).
 */
static esp_err_t runtime_handle_join_request(meshpay_app_runtime_t *runtime,
                                             const rns_packet_t *packet)
{
    /* Je ne peux servir un descripteur que si j'en détiens un, persisté. */
    if (!runtime->app->currency.has_descriptor || !runtime->has_storage ||
        !runtime->storage_record.has_currency_descriptor) {
        return ESP_OK;
    }
    uint32_t wanted = 0;
    if (meshpay_descriptor_sync_parse_request(packet, &wanted, NULL) != ESP_OK) {
        return ESP_OK; /* requête illisible ignorée */
    }
    /* Pas ma monnaie -> je ne réponds pas (un autre membre le fera). */
    if (wanted != runtime->app->currency.currency_id) {
        return ESP_OK;
    }
    if (runtime->packet_tx == NULL) {
        return ESP_OK; /* pas d'émetteur radio câblé */
    }

    /* Décode MON descripteur stocké et le ressert tel quel en OFFER broadcast. */
    meshpay_currency_descriptor_signed_t signed_desc;
    if (meshpay_currency_descriptor_decode(
            runtime->storage_record.currency_descriptor,
            runtime->storage_record.currency_descriptor_len,
            &signed_desc) != ESP_OK) {
        ESP_LOGW("app_runtime", "descripteur stocké illisible -> pas d'OFFER");
        return ESP_OK;
    }
    rns_packet_t offer;
    if (meshpay_descriptor_sync_build_offer(
            &signed_desc, runtime->app->local_destination, &offer) != ESP_OK) {
        return ESP_OK;
    }
    (void)runtime->packet_tx(&offer, runtime->packet_tx_ctx);
    ESP_LOGI("app_runtime", "OFFER descripteur servi pour currency_id=%08x",
             (unsigned)wanted);
    return ESP_OK;
}

/*
 * Palier C4 — auto-crédit initial (CLAIM réflexive). PRÉ-REQUIS : le lock du
 * runtime est DÉJÀ détenu par l'appelant (process_one ou le wrapper public).
 *
 * Garde « déjà réclamé » = le DAG lui-même : s'il contient une CLAIM
 * `from == moi`, on ne ré-émet pas (le DAG est persisté via dag_store, donc la
 * garde survit au reboot ; et même en cas de course, l'unicité (from, seq==0)
 * du merge rejette toute 2e CLAIM en CONFLICT). Aucun flag storage séparé.
 */
static esp_err_t runtime_claim_initial_credit_locked(
    meshpay_app_runtime_t *runtime, uint64_t now_ms)
{
    meshpay_app_t *app = runtime->app;

    /* Pas membre d'une monnaie à descripteur -> le crédit initial n'existe pas
     * (le repli legacy garde son boot-credit MINT, géré côté main/). */
    if (!app->currency.has_descriptor) {
        return ESP_OK;
    }
    /* Monnaie sans crédit initial : rien à réclamer. */
    if (app->currency.initial_credit == 0) {
        return ESP_OK;
    }
    /* Déjà réclamé ? (une CLAIM from==moi dans la fenêtre DAG persistée) */
    for (size_t i = 0; i < meshpay_dag_count(&app->dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(&app->dag, i);
        if (tx != NULL && tx->type == MESHPAY_TX_TYPE_CLAIM &&
            rns_crypto_constant_equal(tx->from, app->local_destination,
                                      MESHPAY_TX_DESTINATION_HASH_SIZE)) {
            return ESP_OK; /* idempotent : la garde EST le DAG */
        }
    }

    /* CLAIM DÉTERMINISTE : timestamp figé à 0 et AUCUN parent (genesis pur). Son
     * id ne dépend donc que de (from, to, amount, currency_id) — invariants du
     * membre. Une ré-émission après un reboot où la CLAIM n'a pas été persistée
     * (mais a déjà été tirée par des pairs) produit alors le MÊME id -> merge
     * DUPLICATE (vraie idempotence), et jamais un id différent qui, faute de
     * tie-break sur (from, seq==0), forkerait le réseau de façon permanente
     * (constat #4 de la revue). now_ms ne sert qu'au flush/refresh plus bas, pas
     * d'horodatage de la CLAIM. */
    meshpay_tx_t claim;
    ESP_RETURN_ON_ERROR(
        meshpay_tx_create_claim(&claim, &app->identity, app->local_destination,
                                app->currency.initial_credit,
                                app->currency.currency_id,
                                NULL, 0, 0 /* timestamp déterministe */),
        APP_RUNTIME_TAG, "");

    /* Validation économique (montant exact + plafond max_supply). */
    meshpay_currency_result_t verdict =
        meshpay_currency_validate_tx(&app->currency, &app->dag, &claim);
    if (verdict != MESHPAY_CURRENCY_OK) {
        ESP_LOGW(APP_RUNTIME_TAG,
                 "credit initial refuse (verdict=%d, plafond atteint ?)",
                 (int)verdict);
        return ESP_ERR_INVALID_STATE;
    }

    meshpay_dag_merge_result_t merge = meshpay_dag_merge_tx(&app->dag, &claim);
    if (merge == MESHPAY_DAG_MERGE_DUPLICATE) {
        /* Même id déjà présent : ré-émission déterministe -> vraie idempotence. */
        return ESP_OK;
    }
    if (merge == MESHPAY_DAG_MERGE_CONFLICT) {
        /* ANOMALIE (pas une perte silencieuse) : un AUTRE tx occupe déjà
         * (moi, seq==0) dans CETTE monnaie. Avec la CLAIM déterministe + le
         * conflit scopé par currency_id, ça ne devrait pas se produire pour un
         * membre honnête ; on le signale et on ne crédite pas (le solde reste 0,
         * diagnostiquable — pas d'échec masqué). */
        ESP_LOGW(APP_RUNTIME_TAG,
                 "CLAIM en conflit sur (moi, seq=0) cur=%08x -> credit non applique",
                 (unsigned)app->currency.currency_id);
        return ESP_OK;
    }
    if (merge != MESHPAY_DAG_MERGE_OK) {
        ESP_LOGW(APP_RUNTIME_TAG, "merge CLAIM inattendu (%d)", (int)merge);
        return ESP_ERR_INVALID_STATE;
    }

    /* Commit-on-send : la CLAIM est committée localement -> persistance FORCÉE
     * (comme un paiement local, le cas critique à ne pas perdre). Les pairs la
     * tireront via la sync DAG (SUMMARY/REQUEST/BATCH). */
    runtime_dag_mark_dirty(runtime);
    runtime_dag_flush(runtime, now_ms, true, "claim");
    (void)runtime_refresh_balance(runtime, now_ms);
    ESP_LOGI(APP_RUNTIME_TAG, "credit initial reclame amount=%u",
             (unsigned)app->currency.initial_credit);
    return ESP_OK;
}

esp_err_t meshpay_app_runtime_claim_initial_credit(meshpay_app_runtime_t *runtime,
                                                   uint64_t now_ms)
{
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = runtime_claim_initial_credit_locked(runtime, now_ms);
    xSemaphoreGive(runtime->lock);
    return err;
}

/*
 * Palier D1 — création de monnaie côté fondateur. PRÉ-REQUIS : lock déjà tenu.
 * Réutilise le chemin d'import (persistance canonique + rollback atomique) et le
 * chemin de crédit initial (CLAIM déterministe), pour ne pas dupliquer ces
 * invariants sensibles.
 */
static esp_err_t create_currency_locked(meshpay_app_runtime_t *runtime,
                                        const meshpay_app_currency_params_t *params,
                                        uint64_t now_ms)
{
    meshpay_app_t *app = runtime->app;

    /* Mono-monnaie STRICT : on ne crée pas si l'on est déjà membre d'une monnaie. */
    if (app->currency.has_descriptor) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Sans storage, la monnaie créée ne survivrait pas au reboot -> refus (comme
     * l'import d'une rejointe). */
    if (!runtime->has_storage) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Corps du descripteur depuis les params fondateur. founder_public et le
     * genesis/currency_id sont renseignés par sign() ; name/symbol sont bornés et
     * null-terminés (le corps est remis à zéro par init). */
    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    strncpy(body.name, params->name, sizeof(body.name) - 1);
    strncpy(body.symbol, params->symbol, sizeof(body.symbol) - 1);
    body.max_supply = params->max_supply;
    body.transfer_fee = params->transfer_fee;
    body.initial_credit = params->initial_credit;
    body.demurrage_enabled = params->demurrage_enabled;
    body.demurrage_bps = params->demurrage_bps;
    body.created_at_ms = now_ms;

    /* Signe avec l'identité locale : cette identité DEVIENT le fondateur. */
    meshpay_currency_descriptor_signed_t signed_desc;
    ESP_RETURN_ON_ERROR(
        meshpay_currency_descriptor_sign(&signed_desc, &body, &app->identity),
        APP_RUNTIME_TAG, "");

    /* Persiste le blob + applique la config EN PLACE (devient fondateur-membre).
     * Même chemin atomique que l'import d'une rejointe. */
    ESP_RETURN_ON_ERROR(
        runtime_import_currency_descriptor(runtime, &signed_desc),
        APP_RUNTIME_TAG, "");

    /* Le fondateur est aussi un membre : il s'auto-crédite le crédit initial
     * (best-effort, comme à la rejointe ; un plafond trop petit ne fait pas
     * échouer la création). */
    (void)runtime_claim_initial_credit_locked(runtime, now_ms);
    (void)runtime_refresh_balance(runtime, now_ms);
    ESP_LOGI(APP_RUNTIME_TAG,
             "monnaie creee currency_id=%08x initial_credit=%u",
             (unsigned)app->currency.currency_id,
             (unsigned)app->currency.initial_credit);
    return ESP_OK;
}

esp_err_t meshpay_app_runtime_create_currency(
    meshpay_app_runtime_t *runtime,
    const meshpay_app_currency_params_t *params,
    uint64_t now_ms)
{
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL ||
        params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = create_currency_locked(runtime, params, now_ms);
    xSemaphoreGive(runtime->lock);
    return err;
}

/*
 * Palier B4 — traite un OFFER de descripteur reçu (data[0] == 0x34). Séquence
 * stricte : idempotence (déjà membre) -> armé ? -> parse -> matches_anchor ->
 * verify -> import. Tout rejet est NON DESTRUCTIF et NON BLOQUANT (log + ESP_OK)
 * pour ne pas figer la tâche reticulum ni désarmer sur un OFFER parasite.
 */
static esp_err_t runtime_handle_join_offer(meshpay_app_runtime_t *runtime,
                                           const rns_packet_t *packet,
                                           uint64_t now_ms)
{
    /* Idempotence : déjà membre d'une monnaie -> on ignore tout OFFER. */
    if (runtime->app->currency.has_descriptor) {
        return ESP_OK;
    }
    /* Aucune rejointe armée -> on n'importe pas une monnaie non demandée. */
    if (!runtime->join_armed) {
        return ESP_OK;
    }
    /* (Timeout de fenêtre : champ join_armed_until_ms posé, désarmement différé
     * à un palier ultérieur — non évalué ici.) */

    meshpay_currency_descriptor_signed_t signed_desc;
    if (meshpay_descriptor_sync_parse_offer(packet, &signed_desc) != ESP_OK) {
        ESP_LOGW("app_runtime", "OFFER descripteur illisible ignoré");
        return ESP_OK;
    }
    /* L'ancre saisie hors-bande DOIT préfixer la genèse recalculée. */
    if (meshpay_currency_descriptor_matches_anchor(
            &signed_desc, runtime->pending_anchor,
            runtime->pending_anchor_len) != ESP_OK) {
        ESP_LOGW("app_runtime", "OFFER ancre non-matchante ignoré (autre monnaie)");
        return ESP_OK; /* reste armé */
    }
    /* Ancre OK mais la signature du fondateur doit aussi vérifier. */
    if (meshpay_currency_descriptor_verify(&signed_desc) != ESP_OK) {
        ESP_LOGW("app_runtime", "OFFER signature fondateur invalide ignoré");
        return ESP_OK; /* reste armé */
    }

    /* Descripteur décodé + ancre + signature validées : import (persistance du
     * ré-encodage canonique + application de la config). */
    esp_err_t err = runtime_import_currency_descriptor(runtime, &signed_desc);
    if (err != ESP_OK) {
        ESP_LOGE("app_runtime", "import descripteur échoué: %s -> reste armé",
                 esp_err_to_name(err));
        return ESP_OK; /* échec (p.ex. storage) : le membre réessaiera */
    }

    runtime->join_armed = false;
    /* Palier C4 : nouveau membre -> auto-réclame le crédit initial (CLAIM).
     * Non bloquant : un refus (p.ex. plafond épuisé) n'invalide pas la rejointe,
     * le boot retentera (idempotent, la garde est le DAG). Lock déjà détenu. */
    (void)runtime_claim_initial_credit_locked(runtime, now_ms);
    (void)runtime_refresh_balance(runtime, now_ms);
    ESP_LOGI("app_runtime", "rejointe réussie: currency_id=%08x",
             (unsigned)runtime->app->currency.currency_id);
    return ESP_OK;
}

static esp_err_t runtime_process_reticulum(meshpay_app_runtime_t *runtime,
                                           const meshpay_app_event_t *event)
{
    if (event->type == MESHPAY_APP_EVENT_STOP) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type == MESHPAY_APP_EVENT_RETICULUM_TX) {
        if (runtime->packet_tx != NULL) {
            ESP_RETURN_ON_ERROR(runtime->packet_tx(&event->packet,
                                                   runtime->packet_tx_ctx),
                                "app_runtime", "");
        }
        runtime->processed_reticulum++;
        return ESP_OK;
    }
    if (event->type != MESHPAY_APP_EVENT_RETICULUM_RX) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if (event->packet.packet_type == RNS_PACKET_TYPE_ANNOUNCE) {
        err = rns_announce_verify_and_remember(&event->packet, NULL);
        if (err == ESP_OK) {
            runtime_refresh_known_peers(runtime);
            const rns_announce_known_destination_t *known =
                rns_announce_recall(event->packet.destination_hash);
            char label[MESHPAY_UI_PEER_LABEL_MAX];
            runtime_peer_label_from_known(known, label);
            ESP_LOGI(APP_RUNTIME_TAG,
                     "peer announce accepted alias=%s peers=%u",
                     label,
                     (unsigned)runtime->app->ui.network_peers);
        }
    } else if (event->packet.packet_type == RNS_PACKET_TYPE_DATA &&
               event->packet.context == RNS_PACKET_CONTEXT_REQUEST) {
        err = runtime_handle_dag_request(runtime, &event->packet);
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    } else if (event->packet.packet_type == RNS_PACKET_TYPE_DATA &&
               event->packet.context == RNS_PACKET_CONTEXT_RESPONSE) {
        err = ESP_OK;
    } else if (event->packet.packet_type == RNS_PACKET_TYPE_DATA &&
               event->packet.context == RNS_PACKET_CONTEXT_RESOURCE) {
        err = runtime_handle_dag_resource(runtime,
                                          &event->packet,
                                          event->now_ms);
    } else if (runtime_packet_is_local_payment_status(runtime, &event->packet)) {
        uint32_t amount = event->amount;
        uint8_t peer_destination[MESHPAY_TX_DESTINATION_HASH_SIZE];
        bool has_peer_destination = false;
        if (amount == 0 && runtime->app->payments.has_pending) {
            amount = runtime->app->payments.pending_tx.amount;
        }
        if (runtime->app->payments.has_pending) {
            memcpy(peer_destination,
                   runtime->app->payments.pending_tx.to,
                   sizeof(peer_destination));
            has_peer_destination = true;
        }
        err = meshpay_payment_engine_receive_ack(&runtime->app->payments,
                                                 &event->packet);
        if (err == ESP_OK) {
            (void)runtime_persist_wallet_state(runtime);
            if (has_peer_destination) {
                runtime_set_history_peer(runtime, peer_destination);
            }
            err = runtime_refresh_balance(runtime, event->now_ms);
        }
        if (err == ESP_OK) {
            err = meshpay_ui_on_payment_feedback(&runtime->app->ui,
                                                 runtime->app->payments.feedback,
                                                 amount);
        }
    } else if (runtime_packet_is_local_single_data(runtime, &event->packet)) {
        uint32_t amount = event->amount;
        rns_packet_t ack;
        rns_packet_clear(&ack);
        err = meshpay_payment_engine_receive_payment(&runtime->app->payments,
                                                     &event->packet,
                                                     event->now_ms,
                                                     &ack);
        if (err == ESP_OK && amount == 0 &&
            runtime->app->payments.has_last_received) {
            amount = runtime->app->payments.last_received_tx.amount;
        }
        if (err == ESP_OK) {
            err = runtime_refresh_balance(runtime, event->now_ms);
        }
        if ((err == ESP_OK || runtime_packet_has_reject_status(&ack)) &&
            runtime->packet_tx != NULL) {
            esp_err_t tx_err = runtime->packet_tx(&ack, runtime->packet_tx_ctx);
            if (err == ESP_OK) {
                err = tx_err;
            } else if (tx_err == ESP_OK) {
                ESP_LOGW(APP_RUNTIME_TAG,
                         "payment rejected and reject sent: %s",
                         esp_err_to_name(err));
                err = ESP_OK;
            }
        }
        if (err == ESP_OK &&
            runtime->app->payments.feedback == MESHPAY_PAYMENT_FEEDBACK_RECEIVED) {
            /* Paiement reçu et mergé dans la DAG locale : persiste (débouncé). */
            runtime_dag_mark_dirty(runtime);
            if (runtime->app->payments.has_last_received) {
                runtime_set_history_peer(
                    runtime,
                    runtime->app->payments.last_received_tx.from);
            }
            err = meshpay_ui_on_payment_feedback(&runtime->app->ui,
                                                 runtime->app->payments.feedback,
                                                 amount);
        }
    } else if (runtime_packet_is_plain_broadcast(&event->packet)) {
        if (event->packet.data[0] == MESHPAY_DAG_SYNC_MSG_SUMMARY) {
            err = runtime_handle_dag_summary(runtime,
                                             &event->packet,
                                             event->now_ms);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                err = ESP_OK;
            }
        } else if (event->packet.data[0] == MESHPAY_DAG_SYNC_MSG_REQUEST) {
            err = runtime_handle_dag_request(runtime, &event->packet);
            if (err == ESP_ERR_NOT_SUPPORTED ||
                err == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        } else if (event->packet.data[0] == MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST) {
            /* Palier B5 — REQUEST de descripteur : servir l'OFFER si membre. */
            err = runtime_handle_join_request(runtime, &event->packet);
        } else if (event->packet.data[0] == MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER) {
            /* Palier B4 — OFFER de descripteur de monnaie (rejointe). Le handler
             * neutralise déjà tous ses rejets (retourne ESP_OK). */
            err = runtime_handle_join_offer(runtime, &event->packet,
                                            event->now_ms);
        }
    }
    if (err == ESP_OK) {
        runtime->processed_reticulum++;
    }
    return err;
}

static esp_err_t runtime_process_core(meshpay_app_runtime_t *runtime,
                                      const meshpay_app_event_t *event)
{
    if (event->type == MESHPAY_APP_EVENT_STOP) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (event->type == MESHPAY_APP_EVENT_CORE_ANNOUNCE) {
        err = meshpay_app_announce(runtime->app);
    } else if (event->type == MESHPAY_APP_EVENT_CORE_PAYMENT) {
        rns_packet_t packet;
        bool payment_ready = false;
        bool had_pending_before = runtime->app->payments.has_pending;
        const rns_announce_known_destination_t *known =
            rns_announce_recall(event->destination);
        if (known != NULL) {
            rns_identity_t recipient;
            err = rns_identity_load_public(&recipient, known->public_key);
            if (err == ESP_OK) {
                err = meshpay_payment_engine_create_encrypted_payment(
                    &runtime->app->payments,
                    event->destination,
                    &recipient,
                    event->amount,
                    event->now_ms,
                    &packet);
                rns_identity_clear(&recipient);
            }
        } else {
            err = meshpay_payment_engine_create_payment(&runtime->app->payments,
                                                        event->destination,
                                                        event->amount,
                                                        event->now_ms,
                                                        &packet);
        }
        /* Option A : create_payment persiste next_seq via son hook AVANT de
         * committer la tx dans la DAG (pas de persist séparé ici, sinon double
         * écriture). En cas d'échec, l'engine a déjà restauré le seq et n'a rien
         * committé. */
        if (err != ESP_OK) {
            esp_err_t reason = err;
            if (!had_pending_before && runtime->app->payments.has_pending) {
                (void)meshpay_payment_engine_cancel_pending(
                    &runtime->app->payments);
            }
            runtime_report_payment_rejected(runtime,
                                            event->amount,
                                            event->now_ms,
                                            reason);
            if (reason == ESP_ERR_INVALID_STATE ||
                reason == ESP_ERR_INVALID_SIZE ||
                reason == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            payment_ready = true;
        }
        if (err == ESP_OK && payment_ready) {
            /* Paiement committé localement : on persiste IMMÉDIATEMENT (cas le
             * plus critique à ne pas perdre — son propre paiement avant la sync). */
            runtime_dag_mark_dirty(runtime);
            runtime_dag_flush(runtime, event->now_ms, true, "payment");
            const meshpay_app_event_t tx_event = {
                .type = MESHPAY_APP_EVENT_RETICULUM_TX,
                .now_ms = event->now_ms,
                .amount = event->amount,
                .packet = packet,
            };
            if (xQueueSend(runtime->reticulum_queue, &tx_event, 0) != pdTRUE) {
                /* Option A : la tx est DÉJÀ committée dans la DAG. L'envoi direct
                 * est best-effort ; si la file est pleine, la synchro DAG
                 * (SUMMARY/REQUEST/BATCH) livrera la tx au destinataire. NE PAS
                 * annuler (cela réutiliserait le seq d'une tx committée). */
                ESP_LOGW(APP_RUNTIME_TAG,
                         "paiement committe; envoi direct non file, sync DAG relaiera");
            }
            err = runtime_refresh_balance(runtime, event->now_ms);
            if (err == ESP_OK) {
                runtime_set_history_peer(runtime, event->destination);
                err = meshpay_ui_on_payment_feedback(
                    &runtime->app->ui,
                    runtime->app->payments.feedback,
                    event->amount);
            }
        }
    } else if (event->type == MESHPAY_APP_EVENT_CORE_DAG_SUMMARY) {
        /* Le SUMMARY se diffuse TOUJOURS periodiquement : c'est l'annonce du
         * digest qui permet aux pairs de detecter une divergence et de tirer ce
         * qui leur manque. Ne JAMAIS le supprimer via quiet_until -- sinon le
         * noeud qui detient une tx unique, occupe a requeter (donc "quiet"),
         * cesse d'annoncer et personne ne tire sa tx (deadlock observe au banc :
         * 3 cartes bloquees a 1 MINT pres). quiet_until ne gate QUE les requetes
         * (cf. runtime_handle_dag_summary). */
        rns_packet_t packet;
        err = meshpay_dag_sync_build_summary(
            &runtime->app->dag,
            runtime->app->local_destination,
            &packet);
        if (err == ESP_OK) {
            const meshpay_app_event_t tx_event = {
                .type = MESHPAY_APP_EVENT_RETICULUM_TX,
                .now_ms = event->now_ms,
                .packet = packet,
            };
            if (xQueueSend(runtime->reticulum_queue,
                           &tx_event,
                           0) != pdTRUE) {
                return ESP_ERR_TIMEOUT;
            }
        }
        /* Tick périodique fiable du wallet (~15 s) : flush débouncé de la DAG.
         * Sur le wallet, UI_REFRESH n'est posté qu'une fois au boot — ce tick
         * SUMMARY est donc la source de flush périodique des tx accumulées. */
        runtime_dag_flush(runtime, event->now_ms, false, "summary");
    }
    if (err == ESP_OK &&
        (event->type == MESHPAY_APP_EVENT_CORE_ANNOUNCE ||
         event->type == MESHPAY_APP_EVENT_CORE_PAYMENT ||
         event->type == MESHPAY_APP_EVENT_CORE_DAG_SUMMARY)) {
        runtime->processed_core++;
    }
    return err;
}

esp_err_t meshpay_app_runtime_process_one(meshpay_app_runtime_t *runtime,
                                          meshpay_app_queue_id_t queue_id,
                                          TickType_t timeout_ticks)
{
    if (runtime == NULL || runtime->app == NULL || runtime->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = runtime_select_queue(runtime, queue_id);
    if (queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_app_event_t event;
    if (xQueueReceive(queue, &event, timeout_ticks) != pdTRUE) {
        runtime_refresh_depths(runtime);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(runtime->lock, timeout_ticks) != pdTRUE) {
        runtime_refresh_depths(runtime);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    switch (queue_id) {
    case MESHPAY_APP_QUEUE_UI:
        err = runtime_process_ui(runtime, &event);
        break;
    case MESHPAY_APP_QUEUE_RETICULUM:
        err = runtime_process_reticulum(runtime, &event);
        break;
    case MESHPAY_APP_QUEUE_CORE:
        err = runtime_process_core(runtime, &event);
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    xSemaphoreGive(runtime->lock);
    runtime_refresh_depths(runtime);
    return err;
}

static void runtime_task_loop(meshpay_app_runtime_t *runtime,
                              meshpay_app_queue_id_t queue_id)
{
    while (true) {
        esp_err_t err = meshpay_app_runtime_process_one(runtime,
                                                        queue_id,
                                                        portMAX_DELAY);
        if (err == ESP_ERR_INVALID_STATE && !runtime->tasks_started) {
            break;
        }
        if (err != ESP_OK) {
            ESP_LOGW(APP_RUNTIME_TAG,
                     "queue %u event failed: %s",
                     (unsigned)queue_id,
                     esp_err_to_name(err));
        }
    }
}

static void runtime_ui_task(void *arg)
{
    meshpay_app_runtime_t *runtime = (meshpay_app_runtime_t *)arg;
    runtime_task_loop(runtime, MESHPAY_APP_QUEUE_UI);
    runtime->ui_task = NULL;
    vTaskDelete(NULL);
}

static void runtime_reticulum_task(void *arg)
{
    meshpay_app_runtime_t *runtime = (meshpay_app_runtime_t *)arg;
    runtime_task_loop(runtime, MESHPAY_APP_QUEUE_RETICULUM);
    runtime->reticulum_task = NULL;
    vTaskDelete(NULL);
}

static void runtime_core_task(void *arg)
{
    meshpay_app_runtime_t *runtime = (meshpay_app_runtime_t *)arg;
    runtime_task_loop(runtime, MESHPAY_APP_QUEUE_CORE);
    runtime->core_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t meshpay_app_runtime_start_tasks(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->ui_queue == NULL ||
        runtime->reticulum_queue == NULL || runtime->core_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->tasks_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(runtime_ui_task,
                    MESHPAY_APP_UI_TASK_NAME,
                    runtime->config.ui_stack_words,
                    runtime,
                    runtime->config.ui_priority,
                    &runtime->ui_task) != pdPASS ||
        xTaskCreate(runtime_reticulum_task,
                    MESHPAY_APP_RETICULUM_TASK_NAME,
                    runtime->config.reticulum_stack_words,
                    runtime,
                    runtime->config.reticulum_priority,
                    &runtime->reticulum_task) != pdPASS ||
        xTaskCreate(runtime_core_task,
                    MESHPAY_APP_CORE_TASK_NAME,
                    runtime->config.core_stack_words,
                    runtime,
                    runtime->config.core_priority,
                    &runtime->core_task) != pdPASS) {
        runtime->tasks_started = true;
        (void)meshpay_app_runtime_stop_tasks(runtime);
        return ESP_ERR_NO_MEM;
    }

    runtime->tasks_started = true;
    return ESP_OK;
}

esp_err_t meshpay_app_runtime_stop_tasks(meshpay_app_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->tasks_started) {
        return ESP_ERR_INVALID_STATE;
    }

    runtime->tasks_started = false;
    const meshpay_app_event_t stop_event = {
        .type = MESHPAY_APP_EVENT_STOP,
    };
    (void)meshpay_app_runtime_post(runtime,
                                   MESHPAY_APP_QUEUE_UI,
                                   &stop_event,
                                   0);
    (void)meshpay_app_runtime_post(runtime,
                                   MESHPAY_APP_QUEUE_RETICULUM,
                                   &stop_event,
                                   0);
    (void)meshpay_app_runtime_post(runtime,
                                   MESHPAY_APP_QUEUE_CORE,
                                   &stop_event,
                                   0);
    return ESP_OK;
}
