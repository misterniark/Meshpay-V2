#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_IDENTITY_PRIVATE_SIZE 64
#define RNS_IDENTITY_PUBLIC_SIZE 64
#define RNS_IDENTITY_HASH_SIZE 16

typedef struct {
    bool has_private;
    bool has_public;
    uint8_t x25519_private[RNS_CRYPTO_X25519_KEY_SIZE];
    uint8_t x25519_public[RNS_CRYPTO_X25519_KEY_SIZE];
    uint8_t ed25519_seed[RNS_CRYPTO_ED25519_SEED_SIZE];
    uint8_t ed25519_private[RNS_CRYPTO_ED25519_PRIVATE_SIZE];
    uint8_t ed25519_public[RNS_CRYPTO_ED25519_PUBLIC_SIZE];
    uint8_t hash[RNS_IDENTITY_HASH_SIZE];
} rns_identity_t;

void rns_identity_clear(rns_identity_t *identity);
esp_err_t rns_identity_generate(rns_identity_t *identity);
esp_err_t rns_identity_load_private(rns_identity_t *identity,
                                    const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE]);
esp_err_t rns_identity_load_public(rns_identity_t *identity,
                                   const uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE]);

esp_err_t rns_identity_get_private_key(const rns_identity_t *identity,
                                       uint8_t out[RNS_IDENTITY_PRIVATE_SIZE]);
esp_err_t rns_identity_get_public_key(const rns_identity_t *identity,
                                      uint8_t out[RNS_IDENTITY_PUBLIC_SIZE]);
esp_err_t rns_identity_get_hash(const rns_identity_t *identity,
                                uint8_t out[RNS_IDENTITY_HASH_SIZE]);

esp_err_t rns_identity_sign(const rns_identity_t *identity,
                            const uint8_t *message, size_t message_len,
                            uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE]);
esp_err_t rns_identity_verify(const rns_identity_t *identity,
                              const uint8_t *message, size_t message_len,
                              const uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE]);
esp_err_t rns_identity_shared_secret(const rns_identity_t *identity,
                                     const uint8_t peer_public_key[RNS_IDENTITY_PUBLIC_SIZE],
                                     uint8_t shared_secret[RNS_CRYPTO_X25519_SHARED_SIZE]);

#ifdef __cplusplus
}
#endif

