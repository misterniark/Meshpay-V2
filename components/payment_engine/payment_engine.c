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

bool meshpay_payment_engine_expire_pending(meshpay_payment_engine_t *engine,
                                           uint64_t now_ms,
                                           uint32_t *expired_amount)
{
    if (engine == NULL || !engine->has_pending || engine->wallet == NULL ||
        meshpay_wallet_lock_active(engine->wallet, now_ms)) {
        return false;
    }

    if (expired_amount != NULL) {
        *expired_amount = engine->pending_tx.amount;
    }
    (void)meshpay_wallet_unlock(engine->wallet, engine->pending_tx.id);
    meshpay_tx_clear(&engine->pending_tx);
    engine->has_pending = false;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
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
    (void)meshpay_payment_engine_expire_pending(engine, now_ms, NULL);
    if (engine->has_pending ||
        meshpay_wallet_lock_active(engine->wallet, now_ms)) {
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_STATE;
    }

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
    uint32_t cost = amount + engine->currency->transfer_fee;
    err = meshpay_wallet_lock(engine->wallet, tx.id, cost, now_ms);
    if (err != ESP_OK) {
        rollback_allocated_seq(engine->wallet, seq);
        return err;
    }
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_LOCKED;

    uint8_t encoded[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t encoded_len = 0;
    err = meshpay_tx_encode(&tx, encoded, sizeof(encoded), &encoded_len);
    if (err != ESP_OK) {
        (void)meshpay_wallet_unlock(engine->wallet, tx.id);
        rollback_allocated_seq(engine->wallet, seq);
        return err;
    }
    if (encoded_len + 1U > RNS_PACKET_MAX_DATA_SIZE) {
        (void)meshpay_wallet_unlock(engine->wallet, tx.id);
        rollback_allocated_seq(engine->wallet, seq);
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_SIZE;
    }

    packet_base(packet, to);
    packet->data[0] = MESHPAY_PAYMENT_MSG_TX;
    memcpy(packet->data + 1, encoded, encoded_len);
    packet->data_len = encoded_len + 1U;

    memcpy(&engine->pending_tx, &tx, sizeof(tx));
    engine->has_pending = true;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_SENT;
    return ESP_OK;
}

static void rollback_pending_payment(meshpay_payment_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    if (engine->has_pending && engine->wallet != NULL) {
        (void)meshpay_wallet_unlock(engine->wallet, engine->pending_tx.id);
        rollback_allocated_seq(engine->wallet, engine->pending_tx.seq);
    }
    meshpay_tx_clear(&engine->pending_tx);
    engine->has_pending = false;
    engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
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
    if (engine == NULL || recipient == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = meshpay_payment_engine_create_payment(engine,
                                                          to,
                                                          amount,
                                                          now_ms,
                                                          packet);
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
    if (err == ESP_OK) {
        memcpy(packet->data, token, token_len);
        packet->data_len = token_len;
    } else {
        rollback_pending_payment(engine);
    }
    rns_crypto_secure_zero(token, sizeof(token));
    return err;
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
    meshpay_currency_result_t currency_result =
        meshpay_currency_validate_tx(engine->currency, engine->dag, &tx);
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

    if (ack_packet->data[0] == MESHPAY_PAYMENT_MSG_REJECT) {
        rollback_pending_payment(engine);
        return ESP_OK;
    }

    meshpay_dag_merge_result_t merge =
        meshpay_dag_merge_tx(engine->dag, &engine->pending_tx);
    if (merge != MESHPAY_DAG_MERGE_OK &&
        merge != MESHPAY_DAG_MERGE_DUPLICATE) {
        engine->feedback = MESHPAY_PAYMENT_FEEDBACK_REJECTED;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(meshpay_wallet_unlock(engine->wallet,
                                              engine->pending_tx.id),
                        "payment_engine", "");
    engine->has_pending = false;
    meshpay_tx_clear(&engine->pending_tx);
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
    rollback_pending_payment(engine);
    return ESP_OK;
}
