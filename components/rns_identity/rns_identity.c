#include "meshpay/rns/rns_identity.h"

#include <string.h>

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static esp_err_t identity_update_hash(rns_identity_t *identity)
{
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    memcpy(public_key, identity->x25519_public, RNS_CRYPTO_X25519_KEY_SIZE);
    memcpy(public_key + RNS_CRYPTO_X25519_KEY_SIZE,
           identity->ed25519_public,
           RNS_CRYPTO_ED25519_PUBLIC_SIZE);

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(public_key, sizeof(public_key), full_hash);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(full_hash, sizeof(full_hash));
        return err;
    }

    memcpy(identity->hash, full_hash, RNS_IDENTITY_HASH_SIZE);
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return ESP_OK;
}

void rns_identity_clear(rns_identity_t *identity)
{
    if (identity == NULL) {
        return;
    }
    rns_crypto_secure_zero(identity, sizeof(*identity));
}

esp_err_t rns_identity_load_private(rns_identity_t *identity,
                                    const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE])
{
    if (identity == NULL || private_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(private_key, RNS_CRYPTO_X25519_KEY_SIZE) ||
        bytes_zero(private_key + RNS_CRYPTO_X25519_KEY_SIZE,
                   RNS_CRYPTO_ED25519_SEED_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_identity_clear(identity);
    memcpy(identity->x25519_private, private_key, RNS_CRYPTO_X25519_KEY_SIZE);
    memcpy(identity->ed25519_seed,
           private_key + RNS_CRYPTO_X25519_KEY_SIZE,
           RNS_CRYPTO_ED25519_SEED_SIZE);

    esp_err_t err = rns_crypto_x25519_public_key(identity->x25519_private,
                                                 identity->x25519_public);
    if (err != ESP_OK) {
        rns_identity_clear(identity);
        return err;
    }

    err = rns_crypto_ed25519_keypair_from_seed(identity->ed25519_seed,
                                               identity->ed25519_private,
                                               identity->ed25519_public);
    if (err != ESP_OK) {
        rns_identity_clear(identity);
        return err;
    }

    identity->has_private = true;
    identity->has_public = true;
    err = identity_update_hash(identity);
    if (err != ESP_OK) {
        rns_identity_clear(identity);
    }
    return err;
}

esp_err_t rns_identity_load_public(rns_identity_t *identity,
                                   const uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE])
{
    if (identity == NULL || public_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(public_key, RNS_CRYPTO_X25519_KEY_SIZE) ||
        bytes_zero(public_key + RNS_CRYPTO_X25519_KEY_SIZE,
                   RNS_CRYPTO_ED25519_PUBLIC_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_identity_clear(identity);
    memcpy(identity->x25519_public, public_key, RNS_CRYPTO_X25519_KEY_SIZE);
    memcpy(identity->ed25519_public,
           public_key + RNS_CRYPTO_X25519_KEY_SIZE,
           RNS_CRYPTO_ED25519_PUBLIC_SIZE);
    identity->has_public = true;

    esp_err_t err = identity_update_hash(identity);
    if (err != ESP_OK) {
        rns_identity_clear(identity);
    }
    return err;
}

esp_err_t rns_identity_generate(rns_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    esp_err_t err = rns_crypto_random(private_key, sizeof(private_key));
    if (err != ESP_OK) {
        return err;
    }

    err = rns_identity_load_private(identity, private_key);
    rns_crypto_secure_zero(private_key, sizeof(private_key));
    return err;
}

esp_err_t rns_identity_get_private_key(const rns_identity_t *identity,
                                       uint8_t out[RNS_IDENTITY_PRIVATE_SIZE])
{
    if (identity == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_private) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out, identity->x25519_private, RNS_CRYPTO_X25519_KEY_SIZE);
    memcpy(out + RNS_CRYPTO_X25519_KEY_SIZE,
           identity->ed25519_seed,
           RNS_CRYPTO_ED25519_SEED_SIZE);
    return ESP_OK;
}

esp_err_t rns_identity_get_public_key(const rns_identity_t *identity,
                                      uint8_t out[RNS_IDENTITY_PUBLIC_SIZE])
{
    if (identity == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_public) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out, identity->x25519_public, RNS_CRYPTO_X25519_KEY_SIZE);
    memcpy(out + RNS_CRYPTO_X25519_KEY_SIZE,
           identity->ed25519_public,
           RNS_CRYPTO_ED25519_PUBLIC_SIZE);
    return ESP_OK;
}

esp_err_t rns_identity_get_hash(const rns_identity_t *identity,
                                uint8_t out[RNS_IDENTITY_HASH_SIZE])
{
    if (identity == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_public) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out, identity->hash, RNS_IDENTITY_HASH_SIZE);
    return ESP_OK;
}

esp_err_t rns_identity_sign(const rns_identity_t *identity,
                            const uint8_t *message, size_t message_len,
                            uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE])
{
    if (identity == NULL || signature == NULL || (message == NULL && message_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_private) {
        return ESP_ERR_INVALID_STATE;
    }

    return rns_crypto_ed25519_sign(identity->ed25519_private,
                                   message, message_len,
                                   signature);
}

esp_err_t rns_identity_verify(const rns_identity_t *identity,
                              const uint8_t *message, size_t message_len,
                              const uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE])
{
    if (identity == NULL || signature == NULL || (message == NULL && message_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_public) {
        return ESP_ERR_INVALID_STATE;
    }

    return rns_crypto_ed25519_verify(identity->ed25519_public,
                                     message, message_len,
                                     signature);
}

esp_err_t rns_identity_shared_secret(const rns_identity_t *identity,
                                     const uint8_t peer_public_key[RNS_IDENTITY_PUBLIC_SIZE],
                                     uint8_t shared_secret[RNS_CRYPTO_X25519_SHARED_SIZE])
{
    if (identity == NULL || peer_public_key == NULL || shared_secret == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!identity->has_private) {
        return ESP_ERR_INVALID_STATE;
    }

    return rns_crypto_x25519_shared_secret(identity->x25519_private,
                                           peer_public_key,
                                           shared_secret);
}
