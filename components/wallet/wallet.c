#include "meshpay/wallet.h"

#include "esp_check.h"
#include "meshpay/rns/rns_crypto.h"
#include <string.h>

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static esp_err_t hash_pin(const char *pin, size_t pin_len,
                          uint8_t out[MESHPAY_WALLET_PIN_HASH_SIZE])
{
    static const uint8_t prefix[] = {
        'M', 'e', 's', 'h', 'P', 'a', 'y', 'V', '2', ':', 'P', 'I', 'N'
    };
    if (pin == NULL || pin_len < 4 || pin_len > 16 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[sizeof(prefix) + 16];
    memcpy(buf, prefix, sizeof(prefix));
    memcpy(buf + sizeof(prefix), pin, pin_len);
    esp_err_t ret = rns_crypto_sha256(buf, sizeof(prefix) + pin_len, out);
    rns_crypto_secure_zero(buf, sizeof(buf));
    return ret;
}

esp_err_t meshpay_wallet_init(meshpay_wallet_t *wallet,
                              const uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE],
                              uint32_t next_seq)
{
    if (wallet == NULL || owner == NULL ||
        bytes_zero(owner, MESHPAY_TX_DESTINATION_HASH_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(wallet, 0, sizeof(*wallet));
    memcpy(wallet->owner, owner, sizeof(wallet->owner));
    wallet->next_seq = next_seq;
    return ESP_OK;
}

esp_err_t meshpay_wallet_allocate_seq(meshpay_wallet_t *wallet,
                                      uint32_t *seq)
{
    if (wallet == NULL || seq == NULL || wallet->next_seq == UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *seq = wallet->next_seq;
    wallet->next_seq++;
    return ESP_OK;
}

bool meshpay_wallet_lock_active(const meshpay_wallet_t *wallet,
                                uint64_t now_ms)
{
    if (wallet == NULL || !wallet->lock_active) {
        return false;
    }
    if (now_ms < wallet->lock_started_ms) {
        return true;
    }
    return now_ms - wallet->lock_started_ms < MESHPAY_WALLET_LOCK_TIMEOUT_MS;
}

esp_err_t meshpay_wallet_lock(meshpay_wallet_t *wallet,
                              const uint8_t tx_id[MESHPAY_TX_ID_SIZE],
                              uint32_t amount,
                              uint64_t now_ms)
{
    if (wallet == NULL || tx_id == NULL || amount == 0 ||
        bytes_zero(tx_id, MESHPAY_TX_ID_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (meshpay_wallet_lock_active(wallet, now_ms)) {
        return ESP_ERR_INVALID_STATE;
    }

    wallet->lock_active = true;
    memcpy(wallet->lock_tx_id, tx_id, sizeof(wallet->lock_tx_id));
    wallet->locked_amount = amount;
    wallet->lock_started_ms = now_ms;
    return ESP_OK;
}

esp_err_t meshpay_wallet_unlock(meshpay_wallet_t *wallet,
                                const uint8_t tx_id[MESHPAY_TX_ID_SIZE])
{
    if (wallet == NULL || tx_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wallet->lock_active) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!rns_crypto_constant_equal(wallet->lock_tx_id, tx_id,
                                   MESHPAY_TX_ID_SIZE)) {
        return ESP_ERR_NOT_FOUND;
    }

    wallet->lock_active = false;
    wallet->locked_amount = 0;
    memset(wallet->lock_tx_id, 0, sizeof(wallet->lock_tx_id));
    wallet->lock_started_ms = 0;
    return ESP_OK;
}

esp_err_t meshpay_wallet_get_available_balance(
    meshpay_wallet_t *wallet,
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t now_ms,
    uint32_t *balance)
{
    if (wallet == NULL || balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(meshpay_currency_get_balance(config, dag,
                                                     wallet->owner,
                                                     balance),
                        "wallet", "");

    if (wallet->lock_active && !meshpay_wallet_lock_active(wallet, now_ms)) {
        wallet->lock_active = false;
        wallet->locked_amount = 0;
        memset(wallet->lock_tx_id, 0, sizeof(wallet->lock_tx_id));
        wallet->lock_started_ms = 0;
    }

    if (wallet->lock_active) {
        if (*balance > wallet->locked_amount) {
            *balance -= wallet->locked_amount;
        } else {
            *balance = 0;
        }
    }
    return ESP_OK;
}

esp_err_t meshpay_wallet_set_pin(meshpay_wallet_t *wallet,
                                 const char *pin,
                                 size_t pin_len)
{
    if (wallet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(hash_pin(pin, pin_len, wallet->pin_hash),
                        "wallet", "");
    wallet->has_pin = true;
    wallet->pin_failures = 0;
    wallet->pin_locked = false;
    return ESP_OK;
}

esp_err_t meshpay_wallet_load_pin_hash(
    meshpay_wallet_t *wallet,
    const uint8_t pin_hash[MESHPAY_WALLET_PIN_HASH_SIZE])
{
    if (wallet == NULL || pin_hash == NULL ||
        bytes_zero(pin_hash, MESHPAY_WALLET_PIN_HASH_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(wallet->pin_hash, pin_hash, MESHPAY_WALLET_PIN_HASH_SIZE);
    wallet->has_pin = true;
    wallet->pin_failures = 0;
    wallet->pin_locked = false;
    return ESP_OK;
}

esp_err_t meshpay_wallet_verify_pin(meshpay_wallet_t *wallet,
                                    const char *pin,
                                    size_t pin_len)
{
    if (wallet == NULL || !wallet->has_pin) {
        return ESP_ERR_INVALID_STATE;
    }
    if (wallet->pin_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t candidate[MESHPAY_WALLET_PIN_HASH_SIZE];
    ESP_RETURN_ON_ERROR(hash_pin(pin, pin_len, candidate), "wallet", "");
    bool ok = rns_crypto_constant_equal(candidate, wallet->pin_hash,
                                        sizeof(candidate));
    rns_crypto_secure_zero(candidate, sizeof(candidate));

    if (ok) {
        wallet->pin_failures = 0;
        return ESP_OK;
    }

    wallet->pin_failures++;
    if (wallet->pin_failures >= MESHPAY_WALLET_MAX_PIN_FAILURES) {
        wallet->pin_locked = true;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
