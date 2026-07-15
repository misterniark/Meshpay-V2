#include "meshpay/payment_engine.h"

#include "esp_check.h"
#include "esp_log.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_packet_crypto.h"
#include <string.h>

static const char *TAG = "payment_engine";

static bool account_equal(const uint8_t a[MESHPAY_TX_DESTINATION_HASH_SIZE],
                          const uint8_t b[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    return memcmp(a, b, MESHPAY_TX_DESTINATION_HASH_SIZE) == 0;
}

static void packet_base(rns_packet_t *packet,
                        const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    rns_packet_clear(packet);
    packet->header_type = RNS_PACKET_HEADER_TYPE_1;
    packet->propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet->destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    packet->context = RNS_PACKET_CONTEXT_NONE;
    memcpy(packet->destination_hash, destination, RNS_PACKET_ADDRESS_SIZE);
}

static void payment_status_packet(
    rns_packet_t *packet,
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint8_t status,
    const uint8_t tx_id[MESHPAY_TX_ID_SIZE])
{
    packet_base(packet, destination);
    packet->data[0] = status;
    memcpy(packet->data + 1, tx_id, MESHPAY_TX_ID_SIZE);
    packet->data_len = 1U + MESHPAY_TX_ID_SIZE;
}

static void rollback_allocated_seq(meshpay_wallet_t *wallet, uint32_t seq)
{
    if (wallet != NULL && seq < UINT32_MAX && wallet->next_seq == seq + 1U) {
        wallet->next_seq = seq;
    }
}

/* Oublie le suivi de reçu d'un paiement SANS toucher aux fonds ni au seq.
 * En Option A la tx est déjà committée à l'envoi : « oublier le pending » ne
 * doit donc rien restaurer (contrairement à l'ancien rollback verrou/seq). */
static void clear_pending(meshpay_payment_engine_t *engine)
{
    meshpay_tx_clear(&engine->pending_tx);
    engine->has_pending = false;
    engine->pending_started_ms = 0;
}

bool meshpay_payment_engine_expire_pending(meshpay_payment_engine_t *engine,
                                           uint64_t now_ms,
                                           uint32_t *expired_amount)
{
    if (engine == NULL || !engine->has_pending) {
        return false;
    }
    /* Pas encore expiré : on est dans la fenêtre d'attente de l'accusé.
     * (Le test now_ms >= pending_started_ms évite un sous-débordement en cas
     * d'horloge incohérente.) */
    if (now_ms >= engine->pending_started_ms &&
        now_ms - engine->pending_started_ms <
            MESHPAY_PAYMENT_RECEIPT_TIMEOUT_MS) {
        return false;
    }

    /* Délai écoulé : la tx étant déjà committée, on ne restaure NI fonds NI
     * seq — on oublie juste le suivi cosmétique. Le solde reste correct. */
    if (expired_amount != NULL) {
        *expired_amount = engine->pending_tx.amount;
    }
    clear_pending(engine);
    return true;
}

esp_err_t meshpay_payment_engine_init(meshpay_payment_engine_t *engine,
                                      meshpay_wallet_t *wallet,
                                      meshpay_dag_t *dag,
                                      const meshpay_currency_config_t *currency,
                                      const rns_identity_t *identity)
{
    if (engine == NULL || wallet == NULL || dag == NULL ||
        currency == NULL || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(engine, 0, sizeof(*engine));
    engine->wallet = wallet;
    engine->dag = dag;
    engine->currency = currency;
    engine->identity = identity;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_IDLE;
    return ESP_OK;
}

static uint8_t select_parents(const meshpay_dag_t *dag,
                              uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE])
{
    const meshpay_tx_t *tips[MESHPAY_TX_MAX_PARENTS];
    size_t tip_count = 0;
    (void)meshpay_dag_get_tips(dag, tips, MESHPAY_TX_MAX_PARENTS,
                               &tip_count, NULL);
    for (size_t i = 0; i < tip_count; ++i) {
        memcpy(parents[i], tips[i]->id, MESHPAY_TX_PARENT_ID_SIZE);
    }
    return (uint8_t)tip_count;
}

/* Construit et signe une transaction de transfert, valide les fonds, et encode
 * le paquet applicatif en clair — SANS committer dans la DAG ni marquer de
 * pending. Le seq est alloué ici ; en cas d'échec il est toujours restauré.
 * En sortie : *out_tx = tx prête à committer, *packet = paquet à émettre. */
static esp_err_t build_payment(meshpay_payment_engine_t *engine,
                               const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                               uint32_t amount,
                               uint64_t now_ms,
                               meshpay_tx_t *out_tx,
                               rns_packet_t *packet)
{
    uint32_t seq = 0;
    ESP_RETURN_ON_ERROR(meshpay_wallet_allocate_seq(engine->wallet, &seq),
                        "payment_engine", "");

    uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
    uint8_t parent_count = select_parents(engine->dag, parents);

    meshpay_tx_t tx;
    esp_err_t err = meshpay_tx_create_transfer(&tx,
                                               engine->identity,
                                               engine->wallet->owner,
                                               to,
                                               amount,
                                               seq,
                                               engine->currency->transfer_fee,
                                               engine->currency->currency_id,
                                               parent_count > 0 ? parents : NULL,
                                               parent_count,
                                               now_ms);
    if (err != ESP_OK) {
        rollback_allocated_seq(engine->wallet, seq);
        return err;
    }

    if (meshpay_currency_validate_tx(engine->currency, engine->dag, &tx) !=
        MESHPAY_CURRENCY_OK) {
        rollback_allocated_seq(engine->wallet, seq);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_STATE;
    }

    if (amount > UINT32_MAX - engine->currency->transfer_fee) {
        rollback_allocated_seq(engine->wallet, seq);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t encoded[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t encoded_len = 0;
    err = meshpay_tx_encode(&tx, encoded, sizeof(encoded), &encoded_len);
    if (err != ESP_OK) {
        rollback_allocated_seq(engine->wallet, seq);
        return err;
    }
    if (encoded_len + 1U > RNS_PACKET_MAX_DATA_SIZE) {
        rollback_allocated_seq(engine->wallet, seq);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_SIZE;
    }

    packet_base(packet, to);
    packet->data[0] = MESHPAY_PAYMENT_MSG_TX;
    memcpy(packet->data + 1, encoded, encoded_len);
    packet->data_len = encoded_len + 1U;

    memcpy(out_tx, &tx, sizeof(*out_tx));
    return ESP_OK;
}

/* Persiste l'état du wallet (next_seq) PUIS committe la tx dans la DAG locale.
 * L'ordre est crucial : persister AVANT le merge garantit qu'au reboot le seq
 * ne sera jamais réutilisé pour une autre tx (la DAG est en RAM ; seul next_seq
 * est durable). En cas d'échec, le seq est restauré et rien n'est committé. */
static esp_err_t commit_built(meshpay_payment_engine_t *engine,
                              const meshpay_tx_t *tx,
                              uint64_t now_ms)
{
    if (engine->persist_cb != NULL) {
        esp_err_t err = engine->persist_cb(engine->persist_ctx);
        if (err != ESP_OK) {
            rollback_allocated_seq(engine->wallet, tx->seq);
            engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
            return err;
        }
    }

    meshpay_dag_merge_result_t merge = meshpay_dag_merge_tx(engine->dag, tx);
    if (merge != MESHPAY_DAG_MERGE_OK &&
        merge != MESHPAY_DAG_MERGE_DUPLICATE) {
        /* Le merge a échoué APRÈS une persistance réussie de next_seq (NVS déjà
         * à N+1). On NE rollback PAS le seq en RAM : cela le ferait passer sous
         * la valeur durable et risquerait une réutilisation au reboot. On laisse
         * un trou de seq (jamais réutilisé, donc sûr) et on rejette. */
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_STATE;
    }

    /* Suivi cosmétique du reçu : la tx est déjà committée, l'ACK confirmera. */
    memcpy(&engine->pending_tx, tx, sizeof(engine->pending_tx));
    engine->has_pending = true;
    engine->pending_started_ms = now_ms;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_SENT;
    return ESP_OK;
}

esp_err_t meshpay_payment_engine_create_payment(
    meshpay_payment_engine_t *engine,
    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint32_t amount,
    uint64_t now_ms,
    rns_packet_t *packet)
{
    if (engine == NULL || to == NULL || packet == NULL ||
        engine->wallet == NULL || engine->dag == NULL ||
        engine->currency == NULL || engine->identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Option A (commit-on-send) : on oublie d'abord un suivi de reçu périmé,
     * puis chaque paiement est committé immédiatement et indépendamment — plus
     * de verrou ni de blocage « un seul paiement à la fois ». La protection
     * anti-double-dépense est assurée par validate_tx qui lit la DAG (mise à
     * jour à chaque commit). */
    (void)meshpay_payment_engine_expire_pending(engine, now_ms, NULL);

    meshpay_tx_t tx;
    esp_err_t err = build_payment(engine, to, amount, now_ms, &tx, packet);
    if (err != ESP_OK) {
        return err;
    }
    return commit_built(engine, &tx, now_ms);
}

static esp_err_t verify_sender_if_known(const meshpay_tx_t *tx)
{
    const rns_announce_known_destination_t *known =
        rns_announce_recall(tx->from);
    if (known == NULL) {
        return ESP_OK;
    }

    rns_identity_t sender;
    esp_err_t err = rns_identity_load_public(&sender, known->public_key);
    if (err == ESP_OK) {
        err = meshpay_tx_verify(tx, &sender);
    }
    rns_identity_clear(&sender);
    return err;
}

esp_err_t meshpay_payment_engine_create_encrypted_payment(
    meshpay_payment_engine_t *engine,
    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const rns_identity_t *recipient,
    uint32_t amount,
    uint64_t now_ms,
    rns_packet_t *packet)
{
    if (engine == NULL || to == NULL || recipient == NULL || packet == NULL ||
        engine->wallet == NULL || engine->dag == NULL ||
        engine->currency == NULL || engine->identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)meshpay_payment_engine_expire_pending(engine, now_ms, NULL);

    /* On construit le paquet en clair, on le CHIFFRE, et seulement ENSUITE on
     * committe : ainsi un échec de chiffrement n'a aucune tx committée à
     * annuler (on restaure juste le seq alloué par build_payment). */
    meshpay_tx_t tx;
    esp_err_t err = build_payment(engine, to, amount, now_ms, &tx, packet);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t token[RNS_PACKET_MAX_DATA_SIZE];
    size_t token_len = 0;
    err = rns_packet_crypto_encrypt_single(recipient,
                                           packet->data,
                                           packet->data_len,
                                           token,
                                           sizeof(token),
                                           &token_len);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(token, sizeof(token));
        rollback_allocated_seq(engine->wallet, tx.seq);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return err;
    }
    memcpy(packet->data, token, token_len);
    packet->data_len = token_len;
    rns_crypto_secure_zero(token, sizeof(token));

    return commit_built(engine, &tx, now_ms);
}

esp_err_t meshpay_payment_engine_receive_payment(
    meshpay_payment_engine_t *engine,
    const rns_packet_t *packet,
    uint64_t now_ms,
    rns_packet_t *ack_packet)
{
    (void)now_ms;
    if (engine == NULL || packet == NULL || ack_packet == NULL ||
        engine->wallet == NULL || engine->dag == NULL ||
        engine->currency == NULL || engine->identity == NULL ||
        packet->data_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Neutralise le motif AVANT tout chemin d'échec : un échec pré-validation
     * (déchiffrement, signature) ne doit pas laisser un ERR_INSUFFICIENT
     * périmé qui ferait retenir à tort ce paquet (F1). */
    engine->last_currency_result = MESHPAY_CURRENCY_OK;
    if (!account_equal(packet->destination_hash, engine->wallet->owner)) {
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t decrypted[RNS_PACKET_MAX_DATA_SIZE];
    const uint8_t *payload = packet->data;
    size_t payload_len = packet->data_len;
    esp_err_t err = ESP_OK;
    if (payload[0] != MESHPAY_PAYMENT_MSG_TX) {
        err = rns_packet_crypto_decrypt_single(engine->identity,
                                               packet->data,
                                               packet->data_len,
                                               decrypted,
                                               sizeof(decrypted),
                                               &payload_len);
        if (err != ESP_OK) {
            engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
            rns_crypto_secure_zero(decrypted, sizeof(decrypted));
            return err;
        }
        payload = decrypted;
    }
    if (payload_len < 2 || payload[0] != MESHPAY_PAYMENT_MSG_TX) {
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        rns_crypto_secure_zero(decrypted, sizeof(decrypted));
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_tx_t tx;
    err = meshpay_tx_decode(payload + 1, payload_len - 1, &tx);
    rns_crypto_secure_zero(decrypted, sizeof(decrypted));
    if (err != ESP_OK) {
        return err;
    }
    bool tx_is_for_us = account_equal(tx.to, engine->wallet->owner);
    if (engine->currency->has_descriptor) {
        /* Durcissement ingestion : sous une monnaie à descripteur, le gate
         * crypto complet (annuaire de la DAG) remplace la vérification
         * opportuniste par announce — un émetteur silencieux (jamais annoncé)
         * n'échappe plus à la vérification de signature. Le motif est mémorisé
         * pour l'orchestrateur : UNKNOWN_MEMBER est TRANSITOIRE (la CLAIM du
         * payeur peut être en route par la sync → rétention F1), tout le reste
         * est définitif. */
        meshpay_currency_result_t gate_result = meshpay_currency_ingest_check(
            engine->currency, engine->dag, &tx);
        if (gate_result != MESHPAY_CURRENCY_OK) {
            engine->last_currency_result = gate_result;
            ESP_LOGW(TAG,
                     "payment reject gate=%d from=%02x%02x%02x%02x dag=%u",
                     (int)gate_result,
                     tx.from[0],
                     tx.from[1],
                     tx.from[2],
                     tx.from[3],
                     (unsigned)meshpay_dag_count(engine->dag));
            engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
            if (tx_is_for_us) {
                payment_status_packet(ack_packet,
                                      tx.from,
                                      MESHPAY_PAYMENT_MSG_REJECT,
                                      tx.id);
            }
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        /* Config de repli (pas de descripteur, donc pas d'annuaire) :
         * vérification opportuniste historique — signature contrôlée si
         * l'émetteur s'est annoncé, sinon accepté (maillage ouvert de dev). */
        esp_err_t verify_err = verify_sender_if_known(&tx);
        if (verify_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "payment reject verify from=%02x%02x%02x%02x err=%s",
                     tx.from[0],
                     tx.from[1],
                     tx.from[2],
                     tx.from[3],
                     esp_err_to_name(verify_err));
            engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
            if (tx_is_for_us) {
                payment_status_packet(ack_packet,
                                      tx.from,
                                      MESHPAY_PAYMENT_MSG_REJECT,
                                      tx.id);
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    meshpay_currency_result_t currency_result =
        meshpay_currency_validate_tx(engine->currency, engine->dag, &tx);
    /* Palier F1 : mémorise le motif pour l'orchestrateur (voir header). */
    engine->last_currency_result = currency_result;
    if (!tx_is_for_us || currency_result != MESHPAY_CURRENCY_OK) {
        ESP_LOGW(TAG,
                 "payment reject tx_for_us=%u currency=%d amount=%u from=%02x%02x%02x%02x dag=%u",
                 tx_is_for_us ? 1U : 0U,
                 (int)currency_result,
                 (unsigned)tx.amount,
                 tx.from[0],
                 tx.from[1],
                 tx.from[2],
                 tx.from[3],
                 (unsigned)meshpay_dag_count(engine->dag));
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        if (tx_is_for_us) {
            payment_status_packet(ack_packet,
                                  tx.from,
                                  MESHPAY_PAYMENT_MSG_REJECT,
                                  tx.id);
        }
        return ESP_ERR_INVALID_STATE;
    }

    meshpay_dag_merge_result_t merge = meshpay_dag_merge_tx(engine->dag, &tx);
    if (merge != MESHPAY_DAG_MERGE_OK &&
        merge != MESHPAY_DAG_MERGE_DUPLICATE) {
        ESP_LOGW(TAG,
                 "payment reject merge=%d amount=%u from=%02x%02x%02x%02x dag=%u",
                 (int)merge,
                 (unsigned)tx.amount,
                 tx.from[0],
                 tx.from[1],
                 tx.from[2],
                 tx.from[3],
                 (unsigned)meshpay_dag_count(engine->dag));
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        payment_status_packet(ack_packet,
                              tx.from,
                              MESHPAY_PAYMENT_MSG_REJECT,
                              tx.id);
        return ESP_ERR_INVALID_STATE;
    }

    payment_status_packet(ack_packet, tx.from, MESHPAY_PAYMENT_MSG_ACK, tx.id);
    memcpy(&engine->last_received_tx, &tx, sizeof(tx));
    engine->has_last_received = true;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_RECEIVED;
    ESP_LOGI(TAG,
             "payment received amount=%u from=%02x%02x%02x%02x dag=%u",
             (unsigned)tx.amount,
             tx.from[0],
             tx.from[1],
             tx.from[2],
             tx.from[3],
             (unsigned)meshpay_dag_count(engine->dag));
    return ESP_OK;
}

esp_err_t meshpay_payment_engine_receive_ack(
    meshpay_payment_engine_t *engine,
    const rns_packet_t *ack_packet)
{
    if (engine == NULL || ack_packet == NULL || !engine->has_pending ||
        ack_packet->data_len != 1U + MESHPAY_TX_ID_SIZE ||
        (ack_packet->data[0] != MESHPAY_PAYMENT_MSG_ACK &&
         ack_packet->data[0] != MESHPAY_PAYMENT_MSG_REJECT)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!account_equal(ack_packet->destination_hash, engine->wallet->owner) ||
        memcmp(ack_packet->data + 1, engine->pending_tx.id,
               MESHPAY_TX_ID_SIZE) != 0) {
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_STATE;
    }

    /* Option A : la tx a déjà été committée à l'envoi. L'ACK n'est qu'un
     * ACCUSÉ DE RÉCEPTION ; un REJECT du destinataire n'annule PAS un transfert
     * valide déjà committé — on cesse simplement d'attendre le reçu. */
    if (ack_packet->data[0] == MESHPAY_PAYMENT_MSG_REJECT) {
        clear_pending(engine);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_OK;
    }

    /* ACK : la tx a DÉJÀ été committée à l'envoi par commit_built (toujours, même
     * sans persist_cb). L'ACK ne fait que confirmer la réception — aucun merge à
     * refaire ici (un merge post-checkpoint pourrait échouer et bloquer le
     * suivi). Aucun verrou à libérer (Option A n'en pose plus). */
    clear_pending(engine);
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_ACKED;
    return ESP_OK;
}

esp_err_t meshpay_payment_engine_cancel_pending(
    meshpay_payment_engine_t *engine)
{
    if (engine == NULL || engine->wallet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!engine->has_pending) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Annulation = on cesse d'attendre le reçu. La tx étant committée à
     * l'envoi, on ne restaure NI le solde NI le seq (Option A). */
    clear_pending(engine);
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
    return ESP_OK;
}

void meshpay_payment_engine_set_persist(
    meshpay_payment_engine_t *engine,
    esp_err_t (*persist_cb)(void *ctx),
    void *persist_ctx)
{
    if (engine == NULL) {
        return;
    }
    engine->persist_cb = persist_cb;
    engine->persist_ctx = persist_ctx;
}
