#pragma once

#include "esp_err.h"
#include "meshpay/currency.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_WALLET_LOCK_TIMEOUT_MS 30000ULL
#define MESHPAY_WALLET_PIN_HASH_SIZE RNS_CRYPTO_SHA256_SIZE
#define MESHPAY_WALLET_MAX_PIN_FAILURES 3

typedef struct {
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint32_t next_seq;

    bool lock_active;
    uint8_t lock_tx_id[MESHPAY_TX_ID_SIZE];
    uint32_t locked_amount;
    uint64_t lock_started_ms;

    bool has_pin;
    uint8_t pin_hash[MESHPAY_WALLET_PIN_HASH_SIZE];
    uint8_t pin_failures;
    bool pin_locked;
} meshpay_wallet_t;

esp_err_t meshpay_wallet_init(meshpay_wallet_t *wallet,
                              const uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE],
                              uint32_t next_seq);
esp_err_t meshpay_wallet_allocate_seq(meshpay_wallet_t *wallet,
                                      uint32_t *seq);

bool meshpay_wallet_lock_active(const meshpay_wallet_t *wallet,
                                uint64_t now_ms);
esp_err_t meshpay_wallet_lock(meshpay_wallet_t *wallet,
                              const uint8_t tx_id[MESHPAY_TX_ID_SIZE],
                              uint32_t amount,
                              uint64_t now_ms);
esp_err_t meshpay_wallet_unlock(meshpay_wallet_t *wallet,
                                const uint8_t tx_id[MESHPAY_TX_ID_SIZE]);
esp_err_t meshpay_wallet_get_available_balance(
    meshpay_wallet_t *wallet,
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t now_ms,
    uint32_t *balance);

esp_err_t meshpay_wallet_set_pin(meshpay_wallet_t *wallet,
                                 const char *pin,
                                 size_t pin_len);
esp_err_t meshpay_wallet_load_pin_hash(
    meshpay_wallet_t *wallet,
    const uint8_t pin_hash[MESHPAY_WALLET_PIN_HASH_SIZE]);
esp_err_t meshpay_wallet_verify_pin(meshpay_wallet_t *wallet,
                                    const char *pin,
                                    size_t pin_len);

#ifdef __cplusplus
}
#endif
