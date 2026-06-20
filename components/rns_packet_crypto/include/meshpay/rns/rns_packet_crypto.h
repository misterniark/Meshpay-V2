#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_identity.h"
#include "meshpay/rns/rns_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE RNS_CRYPTO_X25519_KEY_SIZE
#define RNS_PACKET_CRYPTO_IV_SIZE RNS_CRYPTO_AES_BLOCK_SIZE
#define RNS_PACKET_CRYPTO_HMAC_SIZE RNS_CRYPTO_HMAC_SHA256_SIZE
#define RNS_PACKET_CRYPTO_TOKEN_OVERHEAD \
    (RNS_PACKET_CRYPTO_IV_SIZE + RNS_PACKET_CRYPTO_HMAC_SIZE)
#define RNS_PACKET_CRYPTO_DERIVED_KEY_SIZE 64
#define RNS_PACKET_CRYPTO_MAX_PLAINTEXT_SIZE 383
#define RNS_PACKET_CRYPTO_MAX_CIPHERTEXT_SIZE 384
#define RNS_PACKET_CRYPTO_MIN_TOKEN_SIZE \
    (RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE + RNS_PACKET_CRYPTO_TOKEN_OVERHEAD + RNS_CRYPTO_AES_BLOCK_SIZE)
#define RNS_PACKET_CRYPTO_MAX_TOKEN_SIZE \
    (RNS_PACKET_CRYPTO_EPHEMERAL_PUBLIC_SIZE + RNS_PACKET_CRYPTO_TOKEN_OVERHEAD + RNS_PACKET_CRYPTO_MAX_CIPHERTEXT_SIZE)

esp_err_t rns_packet_crypto_encrypt_single(const rns_identity_t *recipient,
                                           const uint8_t *plaintext,
                                           size_t plaintext_len,
                                           uint8_t *token,
                                           size_t token_len,
                                           size_t *written);
esp_err_t rns_packet_crypto_decrypt_single(const rns_identity_t *recipient,
                                           const uint8_t *token,
                                           size_t token_len,
                                           uint8_t *plaintext,
                                           size_t plaintext_len,
                                           size_t *written);

#ifdef __cplusplus
}
#endif
