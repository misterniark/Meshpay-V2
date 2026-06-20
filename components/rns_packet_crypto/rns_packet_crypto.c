#include "meshpay/rns/rns_packet_crypto.h"

#include <string.h>

static esp_err_t derive_token_key(const uint8_t private_key[RNS_CRYPTO_X25519_KEY_SIZE],
                                  const uint8_t peer_public_key[RNS_CRYPTO_X25519_KEY_SIZE],
                                  const uint8_t salt[RNS_IDENTITY_HASH_SIZE],
                                  uint8_t derived[RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE])
{
    uint8_t shared[RNS_CRYPTO_X25519_SHARED_SIZE];
    esp_err_t err = rns_crypto_x25519_shared_secret(private_key, peer_public_key, shared);
    if (err == ESP_OK) {
        err = rns_crypto_hkdf_sha256(shared, sizeof(shared),
                                     salt, RNS_IDENTITY_HASH_SIZE,
                                     NULL, 0,
                                     derived, RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE);
    }
    rns_crypto_secure_zero(shared, sizeof(shared));
    return err;
}

static size_t pkcs7_padded_len(size_t plaintext_len)
{
    size_t pad_len = RNS_CRYPTO_AES_BLOCK_SIZE -
                     (plaintext_len % RNS_CRYPTO_AES_BLOCK_SIZE);
    return plaintext_len + pad_len;
}

static esp_err_t pkcs7_pad(const uint8_t *plaintext, size_t plaintext_len,
                           uint8_t *padded, size_t padded_len)
{
    size_t expected_len = pkcs7_padded_len(plaintext_len);
    if (padded == NULL || padded_len != expected_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plaintext_len > 0) {
        if (plaintext == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(padded, plaintext, plaintext_len);
    }

    uint8_t pad_value = (uint8_t)(padded_len - plaintext_len);
    memset(padded + plaintext_len, pad_value, pad_value);
    return ESP_OK;
}

static esp_err_t pkcs7_unpadded_len(const uint8_t *padded, size_t padded_len,
                                    size_t *unpadded_len)
{
    if (padded == NULL || unpadded_len == NULL ||
        padded_len == 0 || (padded_len % RNS_CRYPTO_AES_BLOCK_SIZE) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pad_value = padded[padded_len - 1];
    if (pad_value == 0 || pad_value > RNS_CRYPTO_AES_BLOCK_SIZE ||
        pad_value > padded_len) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = padded_len - pad_value; i < padded_len; ++i) {
        if (padded[i] != pad_value) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    *unpadded_len = padded_len - pad_value;
    return ESP_OK;
}

static esp_err_t token_hmac(const uint8_t derived[RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE],
                            const uint8_t *signed_part,
                            size_t signed_part_len,
                            uint8_t out[RNS_PACKET_CRYPTO_HMAC_SIZE])
{
    return rns_crypto_hmac_sha256(derived,
                                  RNS_CRYPTO_AES256_KEY_SIZE,
                                  signed_part,
                                  signed_part_len,
                                  out);
}

esp_err_t rns_packet_crypto_encrypt_single(const rns_identity_t *recipient,
                                           const uint8_t *plaintext,
                                           size_t plaintext_len,
                                           uint8_t *token,
                                           size_t token_len,
                                           size_t *written)
{
    if (recipient == NULL || token == NULL ||
        (plaintext == NULL && plaintext_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!recipient->has_public || plaintext_len > RNS_PACKET_CRYPTO_MAX_PLAINTEXT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t padded_len = pkcs7_padded_len(plaintext_len);
    size_t needed = RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE +
                    RNS_PACKET_CRYPTO_IV_SIZE +
                    padded_len +
                    RNS_PACKET_CRYPTO_HMAC_SIZE;
    if (needed > RNS_PACKET_MAX_DATA_SIZE || token_len < needed) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t ephemeral_private[RNS_CRYPTO_X25519_KEY_SIZE];
    uint8_t derived[RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE];
    uint8_t padded[RNS_PACKET_CRYPTO_MAX_CIPHERTEXT_SIZE];
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t identity_hash[RNS_IDENTITY_HASH_SIZE];
    esp_err_t err = ESP_OK;

    err = rns_crypto_random(ephemeral_private, sizeof(ephemeral_private));
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = rns_crypto_x25519_public_key(ephemeral_private, token);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = rns_identity_get_public_key(recipient, public_key);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = rns_identity_get_hash(recipient, identity_hash);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = derive_token_key(ephemeral_private, public_key, identity_hash, derived);
    if (err != ESP_OK) {
        goto cleanup;
    }

    uint8_t *iv = token + RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE;
    err = rns_crypto_random(iv, RNS_PACKET_CRYPTO_IV_SIZE);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = pkcs7_pad(plaintext, plaintext_len, padded, padded_len);
    if (err != ESP_OK) {
        goto cleanup;
    }

    uint8_t *ciphertext = iv + RNS_PACKET_CRYPTO_IV_SIZE;
    err = rns_crypto_aes256_cbc_encrypt(derived + RNS_CRYPTO_AES256_KEY_SIZE,
                                        iv,
                                        padded,
                                        padded_len,
                                        ciphertext);
    if (err != ESP_OK) {
        goto cleanup;
    }

    uint8_t *mac = ciphertext + padded_len;
    err = token_hmac(derived,
                     iv,
                     RNS_PACKET_CRYPTO_IV_SIZE + padded_len,
                     mac);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (written != NULL) {
        *written = needed;
    }

cleanup:
    rns_crypto_secure_zero(ephemeral_private, sizeof(ephemeral_private));
    rns_crypto_secure_zero(derived, sizeof(derived));
    rns_crypto_secure_zero(padded, sizeof(padded));
    rns_crypto_secure_zero(public_key, sizeof(public_key));
    rns_crypto_secure_zero(identity_hash, sizeof(identity_hash));
    return err;
}

esp_err_t rns_packet_crypto_decrypt_single(const rns_identity_t *recipient,
                                           const uint8_t *token,
                                           size_t token_len,
                                           uint8_t *plaintext,
                                           size_t plaintext_len,
                                           size_t *written)
{
    if (recipient == NULL || token == NULL || plaintext == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!recipient->has_private ||
        token_len < RNS_PACKET_CRYPTO_MIN_TOKEN_SIZE ||
        token_len > RNS_PACKET_CRYPTO_MAX_TOKEN_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t ciphertext_len = token_len -
                            RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE -
                            RNS_PACKET_CRYPTO_IV_SIZE -
                            RNS_PACKET_CRYPTO_HMAC_SIZE;
    if (ciphertext_len == 0 ||
        ciphertext_len > RNS_PACKET_CRYPTO_MAX_CIPHERTEXT_SIZE ||
        (ciphertext_len % RNS_CRYPTO_AES_BLOCK_SIZE) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *ephemeral_public = token;
    const uint8_t *iv = token + RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE;
    const uint8_t *ciphertext = iv + RNS_PACKET_CRYPTO_IV_SIZE;
    const uint8_t *received_mac = ciphertext + ciphertext_len;

    uint8_t derived[RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE];
    uint8_t expected_mac[RNS_PACKET_CRYPTO_HMAC_SIZE];
    uint8_t padded[RNS_PACKET_CRYPTO_MAX_CIPHERTEXT_SIZE];
    uint8_t identity_hash[RNS_IDENTITY_HASH_SIZE];
    esp_err_t err = rns_identity_get_hash(recipient, identity_hash);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = derive_token_key(recipient->x25519_private,
                           ephemeral_public,
                           identity_hash,
                           derived);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = token_hmac(derived,
                     iv,
                     RNS_PACKET_CRYPTO_IV_SIZE + ciphertext_len,
                     expected_mac);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (!rns_crypto_constant_equal(received_mac, expected_mac, sizeof(expected_mac))) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    err = rns_crypto_aes256_cbc_decrypt(derived + RNS_CRYPTO_AES256_KEY_SIZE,
                                        iv,
                                        ciphertext,
                                        ciphertext_len,
                                        padded);
    if (err != ESP_OK) {
        goto cleanup;
    }

    size_t unpadded_len = 0;
    err = pkcs7_unpadded_len(padded, ciphertext_len, &unpadded_len);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (unpadded_len > RNS_PACKET_CRYPTO_MAX_PLAINTEXT_SIZE ||
        plaintext_len < unpadded_len) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (unpadded_len > 0) {
        memcpy(plaintext, padded, unpadded_len);
    }
    if (written != NULL) {
        *written = unpadded_len;
    }

cleanup:
    rns_crypto_secure_zero(derived, sizeof(derived));
    rns_crypto_secure_zero(expected_mac, sizeof(expected_mac));
    rns_crypto_secure_zero(padded, sizeof(padded));
    rns_crypto_secure_zero(identity_hash, sizeof(identity_hash));
    return err;
}
