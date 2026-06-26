#pragma once

#include "esp_err.h"
#include "meshpay/currency.h"
#include "meshpay/rns/rns_identity.h"
#include "meshpay/rns/rns_packet.h"
#include "meshpay/wallet.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_PAYMENT_MSG_TX 0x01
#define MESHPAY_PAYMENT_MSG_ACK 0x02
#define MESHPAY_PAYMENT_MSG_REJECT 0x03

/* Option A (commit-on-send) : une transaction est committée dans la DAG dès
 * l'envoi, puis livrée au destinataire par la synchro DAG. L'ACK n'est qu'un
 * accusé de réception. Ce délai borne l'attente de cet accusé avant d'oublier
 * le suivi (purement cosmétique) d'un paiement DÉJÀ committé : il ne touche ni
 * aux fonds ni au seq. */
#define MESHPAY_PAYMENT_RECEIPT_TIMEOUT_MS 30000ULL

typedef enum {
    MESHPAY_PAYMENT_FEEDBACK_IDLE = 0,
    MESHPAY_PAYMENT_FEEDBACK_LOCKED,
    MESHPAY_PAYMENT_FEEDBACK_SENT,
    MESHPAY_PAYMENT_FEEDBACK_RECEIVED,
    MESHPAY_PAYMENT_FEEDBACK_ACKED,
    MESHPAY_PAYMENT_FEEDBACK_REJECTED,
} meshpay_payment_feedback_t;

typedef struct {
    meshpay_wallet_t *wallet;
    meshpay_dag_t *dag;
    const meshpay_currency_config_t *currency;
    const rns_identity_t *identity;
    meshpay_tx_t pending_tx;
    meshpay_tx_t last_received_tx;
    bool has_pending;
    bool has_last_received;
    meshpay_payment_feedback_t feedback;
    /* Horodatage du dernier paiement committé en attente d'accusé (ms). Sert
     * uniquement au délai d'oubli du suivi de reçu (cf. expire_pending). */
    uint64_t pending_started_ms;
    /* Hook de persistance optionnel : appelé pendant create_payment, APRÈS
     * l'allocation du seq et AVANT le commit DAG, pour garantir que next_seq
     * est durable avant qu'une tx entre dans la DAG. NULL => commit immédiat. */
    esp_err_t (*persist_cb)(void *ctx);
    void *persist_ctx;
} meshpay_payment_engine_t;

esp_err_t meshpay_payment_engine_init(meshpay_payment_engine_t *engine,
                                      meshpay_wallet_t *wallet,
                                      meshpay_dag_t *dag,
                                      const meshpay_currency_config_t *currency,
                                      const rns_identity_t *identity);

esp_err_t meshpay_payment_engine_create_payment(
    meshpay_payment_engine_t *engine,
    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint32_t amount,
    uint64_t now_ms,
    rns_packet_t *packet);
esp_err_t meshpay_payment_engine_create_encrypted_payment(
    meshpay_payment_engine_t *engine,
    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const rns_identity_t *recipient,
    uint32_t amount,
    uint64_t now_ms,
    rns_packet_t *packet);

esp_err_t meshpay_payment_engine_receive_payment(
    meshpay_payment_engine_t *engine,
    const rns_packet_t *packet,
    uint64_t now_ms,
    rns_packet_t *ack_packet);

esp_err_t meshpay_payment_engine_receive_ack(
    meshpay_payment_engine_t *engine,
    const rns_packet_t *ack_packet);
bool meshpay_payment_engine_expire_pending(meshpay_payment_engine_t *engine,
                                           uint64_t now_ms,
                                           uint32_t *expired_amount);
esp_err_t meshpay_payment_engine_cancel_pending(
    meshpay_payment_engine_t *engine);

/* Installe le hook de persistance (cf. champ persist_cb). Avec un hook, le seq
 * est persisté avant tout commit DAG ; sans hook (NULL), create_payment committe
 * immédiatement sans persistance — pratique pour les tests unitaires. */
void meshpay_payment_engine_set_persist(
    meshpay_payment_engine_t *engine,
    esp_err_t (*persist_cb)(void *ctx),
    void *persist_ctx);

#ifdef __cplusplus
}
#endif
