#include "meshpay/storage.h"

#include <string.h>

#define MESHPAY_STORAGE_STATE_KEY "meshpay_state"

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

void meshpay_storage_record_init(meshpay_storage_record_t *record)
{
    if (record == NULL) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->magic = MESHPAY_STORAGE_MAGIC;
    record->version = MESHPAY_STORAGE_VERSION;
}

static bool record_header_valid(const meshpay_storage_record_t *record)
{
    return record != NULL &&
           record->magic == MESHPAY_STORAGE_MAGIC &&
           record->version == MESHPAY_STORAGE_VERSION;
}

esp_err_t meshpay_storage_record_set_identity(meshpay_storage_record_t *record,
                                              const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE])
{
    if (!record_header_valid(record) || private_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(private_key, RNS_CRYPTO_X25519_KEY_SIZE) ||
        bytes_zero(private_key + RNS_CRYPTO_X25519_KEY_SIZE,
                   RNS_CRYPTO_ED25519_SEED_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->identity_private, private_key, RNS_IDENTITY_PRIVATE_SIZE);
    record->has_identity = true;
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_alias(meshpay_storage_record_t *record,
                                           const char *alias)
{
    if (!record_header_valid(record) || alias == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(alias);
    if (len == 0 || len >= MESHPAY_STORAGE_ALIAS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(record->alias, 0, sizeof(record->alias));
    memcpy(record->alias, alias, len);
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_pin_hash(meshpay_storage_record_t *record,
                                              const uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE])
{
    if (!record_header_valid(record) || pin_hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(pin_hash, RNS_CRYPTO_SHA256_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->pin_hash, pin_hash, RNS_CRYPTO_SHA256_SIZE);
    record->has_pin_hash = true;
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_checkpoint(meshpay_storage_record_t *record,
                                                uint32_t checkpoint_seq,
                                                const uint8_t *checkpoint,
                                                size_t checkpoint_len)
{
    if (!record_header_valid(record) ||
        (checkpoint == NULL && checkpoint_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (checkpoint_len == 0 || checkpoint_len > MESHPAY_STORAGE_CHECKPOINT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(checkpoint, checkpoint_len, hash);
    if (err != ESP_OK) {
        return err;
    }

    memset(record->checkpoint, 0, sizeof(record->checkpoint));
    memcpy(record->checkpoint, checkpoint, checkpoint_len);
    memcpy(record->checkpoint_hash, hash, sizeof(hash));
    record->checkpoint_seq = checkpoint_seq;
    record->checkpoint_len = checkpoint_len;
    record->has_checkpoint = true;
    rns_crypto_secure_zero(hash, sizeof(hash));
    return ESP_OK;
}

static esp_err_t validate_record(const meshpay_storage_record_t *record)
{
    if (!record_header_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->alias[MESHPAY_STORAGE_ALIAS_MAX - 1] != '\0') {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->checkpoint_len > MESHPAY_STORAGE_CHECKPOINT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->has_identity) {
        if (bytes_zero(record->identity_private, RNS_CRYPTO_X25519_KEY_SIZE) ||
            bytes_zero(record->identity_private + RNS_CRYPTO_X25519_KEY_SIZE,
                       RNS_CRYPTO_ED25519_SEED_SIZE)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (record->has_pin_hash &&
        bytes_zero(record->pin_hash, sizeof(record->pin_hash))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record->has_checkpoint) {
        if (record->checkpoint_len == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
        esp_err_t err = rns_crypto_sha256(record->checkpoint,
                                          record->checkpoint_len,
                                          hash);
        if (err != ESP_OK) {
            return err;
        }
        bool ok = rns_crypto_constant_equal(hash,
                                            record->checkpoint_hash,
                                            sizeof(hash));
        rns_crypto_secure_zero(hash, sizeof(hash));
        if (!ok) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

esp_err_t meshpay_storage_save(const meshpay_storage_backend_t *backend,
                               const meshpay_storage_record_t *record)
{
    if (backend == NULL || backend->write_blob == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = validate_record(record);
    if (err != ESP_OK) {
        return err;
    }
    return backend->write_blob(backend->ctx,
                               MESHPAY_STORAGE_STATE_KEY,
                               record,
                               sizeof(*record));
}

esp_err_t meshpay_storage_load(const meshpay_storage_backend_t *backend,
                               meshpay_storage_record_t *record)
{
    if (backend == NULL || backend->read_blob == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_storage_record_t loaded;
    size_t len = sizeof(loaded);
    esp_err_t err = backend->read_blob(backend->ctx,
                                       MESHPAY_STORAGE_STATE_KEY,
                                       &loaded,
                                       &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(loaded)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = validate_record(&loaded);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(&loaded, sizeof(loaded));
        return err;
    }

    memcpy(record, &loaded, sizeof(*record));
    rns_crypto_secure_zero(&loaded, sizeof(loaded));
    return ESP_OK;
}

esp_err_t meshpay_storage_erase(const meshpay_storage_backend_t *backend)
{
    if (backend == NULL || backend->erase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return backend->erase(backend->ctx, MESHPAY_STORAGE_STATE_KEY);
}
