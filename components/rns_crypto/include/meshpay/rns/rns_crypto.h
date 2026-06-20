#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_CRYPTO_SHA256_SIZE 32
#define RNS_CRYPTO_SHA512_SIZE 64
#define RNS_CRYPTO_HMAC_SHA256_SIZE 32
#define RNS_CRYPTO_AES256_KEY_SIZE 32
#define RNS_CRYPTO_AES_BLOCK_SIZE 16
#define RNS_CRYPTO_ED25519_PUBLIC_SIZE 32
#define RNS_CRYPTO_ED25519_PRIVATE_SIZE 64
#define RNS_CRYPTO_ED25519_SEED_SIZE 32
#define RNS_CRYPTO_ED25519_SIGNATURE_SIZE 64
#define RNS_CRYPTO_X25519_KEY_SIZE 32
#define RNS_CRYPTO_X25519_SHARED_SIZE 32

#define RNS_CRYPTO_SIGNATURE_WIRE_VERSION 1
#define RNS_CRYPTO_SIGNATURE_SCHEME "meshpay-reticulum-monocypher-4.0.2-ed25519"
#define RNS_CRYPTO_SIGNATURE_PROVIDER "Monocypher 4.0.2 crypto_ed25519_*"

typedef esp_err_t (*rns_crypto_rng_fn_t)(void *ctx, uint8_t *out, size_t len);

void rns_crypto_set_rng(rns_crypto_rng_fn_t rng, void *ctx);
void rns_crypto_secure_zero(void *buf, size_t len);
bool rns_crypto_constant_equal(const uint8_t *a, const uint8_t *b, size_t len);

esp_err_t rns_crypto_random(uint8_t *out, size_t len);
esp_err_t rns_crypto_sha256(const uint8_t *data, size_t len,
                            uint8_t out[RNS_CRYPTO_SHA256_SIZE]);
esp_err_t rns_crypto_sha512(const uint8_t *data, size_t len,
                            uint8_t out[RNS_CRYPTO_SHA512_SIZE]);
esp_err_t rns_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                 const uint8_t *data, size_t data_len,
                                 uint8_t out[RNS_CRYPTO_HMAC_SHA256_SIZE]);
esp_err_t rns_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                                 const uint8_t *salt, size_t salt_len,
                                 const uint8_t *context, size_t context_len,
                                 uint8_t *out, size_t out_len);
esp_err_t rns_crypto_pbkdf2_sha256(const uint8_t *password, size_t password_len,
                                   const uint8_t *salt, size_t salt_len,
                                   uint32_t iterations,
                                   uint8_t *out, size_t out_len);

esp_err_t rns_crypto_aes256_cbc_encrypt(
    const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE],
    const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *plain, size_t len,
    uint8_t *cipher);
esp_err_t rns_crypto_aes256_cbc_decrypt(
    const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE],
    const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *cipher, size_t len,
    uint8_t *plain);

esp_err_t rns_crypto_ed25519_keypair_from_seed(
    const uint8_t seed[RNS_CRYPTO_ED25519_SEED_SIZE],
    uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE]);
esp_err_t rns_crypto_ed25519_generate_keypair(
    uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE]);
esp_err_t rns_crypto_ed25519_sign(
    const uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    const uint8_t *message, size_t message_len,
    uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE]);
esp_err_t rns_crypto_ed25519_verify(
    const uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE],
    const uint8_t *message, size_t message_len,
    const uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE]);

esp_err_t rns_crypto_x25519_public_key(
    const uint8_t private_key[RNS_CRYPTO_X25519_KEY_SIZE],
    uint8_t public_key[RNS_CRYPTO_X25519_KEY_SIZE]);
esp_err_t rns_crypto_x25519_shared_secret(
    const uint8_t private_key[RNS_CRYPTO_X25519_KEY_SIZE],
    const uint8_t peer_public_key[RNS_CRYPTO_X25519_KEY_SIZE],
    uint8_t shared_secret[RNS_CRYPTO_X25519_SHARED_SIZE]);

#ifdef __cplusplus
}
#endif
