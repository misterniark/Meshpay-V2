#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_TX_DESTINATION_HASH_SIZE RNS_IDENTITY_HASH_SIZE
#define MESHPAY_TX_ID_SIZE RNS_CRYPTO_SHA256_SIZE
#define MESHPAY_TX_SIGNATURE_SIZE RNS_CRYPTO_ED25519_SIGNATURE_SIZE
#define MESHPAY_TX_PARENT_ID_SIZE RNS_CRYPTO_SHA256_SIZE
#define MESHPAY_TX_MAX_PARENTS 2
#define MESHPAY_TX_CBOR_MAX_SIZE 320

typedef enum {
    MESHPAY_TX_TYPE_TRANSFER = 1,
    MESHPAY_TX_TYPE_MINT = 2,
} meshpay_tx_type_t;

typedef struct {
    uint8_t id[MESHPAY_TX_ID_SIZE];
    meshpay_tx_type_t type;
    uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint32_t amount;
    uint32_t seq;
    uint32_t fee;
    uint32_t currency_id;
    uint64_t timestamp_ms;
    uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
    uint8_t parent_count;
    uint8_t signature[MESHPAY_TX_SIGNATURE_SIZE];
} meshpay_tx_t;

void meshpay_tx_clear(meshpay_tx_t *tx);

esp_err_t meshpay_tx_create_transfer(meshpay_tx_t *tx,
                                     const rns_identity_t *signer,
                                     const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                     const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                     uint32_t amount,
                                     uint32_t seq,
                                     uint32_t fee,
                                     uint32_t currency_id,
                                     const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                                     uint8_t parent_count,
                                     uint64_t timestamp_ms);
esp_err_t meshpay_tx_create_mint(meshpay_tx_t *tx,
                                 const rns_identity_t *signer,
                                 const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 uint32_t amount,
                                 uint32_t seq,
                                 uint32_t currency_id,
                                 const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                                 uint8_t parent_count,
                                 uint64_t timestamp_ms);

esp_err_t meshpay_tx_encode_signable(const meshpay_tx_t *tx,
                                     uint8_t *out,
                                     size_t out_size,
                                     size_t *out_len);
esp_err_t meshpay_tx_encode(const meshpay_tx_t *tx,
                            uint8_t *out,
                            size_t out_size,
                            size_t *out_len);
esp_err_t meshpay_tx_decode(const uint8_t *data,
                            size_t data_len,
                            meshpay_tx_t *tx);
esp_err_t meshpay_tx_compute_id(const meshpay_tx_t *tx,
                                uint8_t out_id[MESHPAY_TX_ID_SIZE]);
esp_err_t meshpay_tx_sign(meshpay_tx_t *tx, const rns_identity_t *signer);

/* The caller must resolve tx->from to the correct Reticulum identity. */
esp_err_t meshpay_tx_verify(const meshpay_tx_t *tx,
                            const rns_identity_t *from_identity);

#ifdef __cplusplus
}
#endif
