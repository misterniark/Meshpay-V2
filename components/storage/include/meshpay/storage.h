#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_STORAGE_ALIAS_MAX 32
#define MESHPAY_STORAGE_CHECKPOINT_MAX 512
/* Blob CBOR opaque du descripteur de monnaie signé (Palier A). storage ne
 * connaît pas son contenu : il le stocke/relit tel quel, la vérification de
 * signature est faite par la couche currency. Borne alignée sur la borne wire
 * du descripteur (MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX = 384). */
#define MESHPAY_STORAGE_DESCRIPTOR_MAX 384
#define MESHPAY_STORAGE_MAGIC 0x4d505356u
/* v2 : ajout du blob descripteur de monnaie (Palier A). Un blob v1 (plus petit)
 * ne se relit pas — traité comme absent (init neuf), ce qui est sûr. */
#define MESHPAY_STORAGE_VERSION 2u
#define MESHPAY_STORAGE_NVS_NAMESPACE "meshpay"

typedef esp_err_t (*meshpay_storage_write_blob_fn_t)(void *ctx,
                                                     const char *key,
                                                     const void *data,
                                                     size_t len);
typedef esp_err_t (*meshpay_storage_read_blob_fn_t)(void *ctx,
                                                    const char *key,
                                                    void *data,
                                                    size_t *len);
typedef esp_err_t (*meshpay_storage_erase_fn_t)(void *ctx,
                                                const char *key);

typedef struct {
    meshpay_storage_write_blob_fn_t write_blob;
    meshpay_storage_read_blob_fn_t read_blob;
    meshpay_storage_erase_fn_t erase;
    void *ctx;
} meshpay_storage_backend_t;

typedef esp_err_t (*meshpay_storage_nvs_noarg_fn_t)(void);

typedef struct {
    meshpay_storage_nvs_noarg_fn_t init;
    meshpay_storage_nvs_noarg_fn_t erase;
} meshpay_storage_nvs_init_ops_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    bool has_identity;
    bool has_pin_hash;
    bool has_checkpoint;
    uint8_t identity_private[RNS_IDENTITY_PRIVATE_SIZE];
    char alias[MESHPAY_STORAGE_ALIAS_MAX];
    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    uint32_t next_seq;
    uint32_t checkpoint_seq;
    uint8_t checkpoint_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t checkpoint[MESHPAY_STORAGE_CHECKPOINT_MAX];
    size_t checkpoint_len;
    /* Palier A — descripteur de monnaie signé, stocké en blob CBOR opaque. */
    bool has_currency_descriptor;
    uint8_t currency_descriptor[MESHPAY_STORAGE_DESCRIPTOR_MAX];
    size_t currency_descriptor_len;
} meshpay_storage_record_t;

void meshpay_storage_record_init(meshpay_storage_record_t *record);
esp_err_t meshpay_storage_record_set_identity(meshpay_storage_record_t *record,
                                              const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE]);
esp_err_t meshpay_storage_record_set_alias(meshpay_storage_record_t *record,
                                           const char *alias);
esp_err_t meshpay_storage_record_set_pin_hash(meshpay_storage_record_t *record,
                                              const uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE]);
esp_err_t meshpay_storage_record_set_checkpoint(meshpay_storage_record_t *record,
                                                uint32_t checkpoint_seq,
                                                const uint8_t *checkpoint,
                                                size_t checkpoint_len);
/* Stocke le blob CBOR opaque du descripteur de monnaie signé (1..MAX octets).
 * storage reste agnostique : aucune validation de contenu ici (la signature est
 * vérifiée par la couche currency au chargement). */
esp_err_t meshpay_storage_record_set_currency_descriptor(
    meshpay_storage_record_t *record,
    const uint8_t *descriptor,
    size_t descriptor_len);
esp_err_t meshpay_storage_save(const meshpay_storage_backend_t *backend,
                               const meshpay_storage_record_t *record);
esp_err_t meshpay_storage_load(const meshpay_storage_backend_t *backend,
                               meshpay_storage_record_t *record);
esp_err_t meshpay_storage_erase(const meshpay_storage_backend_t *backend);

typedef struct {
    bool present;
    uint8_t blob[sizeof(meshpay_storage_record_t)];
    size_t blob_len;
    uint32_t write_count;
    uint32_t read_count;
    uint32_t erase_count;
} meshpay_storage_mock_t;

void meshpay_storage_mock_init(meshpay_storage_mock_t *mock);
meshpay_storage_backend_t meshpay_storage_mock_backend(meshpay_storage_mock_t *mock);

esp_err_t meshpay_storage_nvs_init(void);
esp_err_t meshpay_storage_nvs_init_with_ops(
    const meshpay_storage_nvs_init_ops_t *ops);
meshpay_storage_backend_t meshpay_storage_nvs_backend(void);

#ifdef __cplusplus
}
#endif
