#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_destination.h"
#include "meshpay/rns/rns_identity.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_ANNOUNCE_RANDOM_HASH_SIZE 10
#define RNS_ANNOUNCE_PUBLIC_KEY_SIZE RNS_IDENTITY_PUBLIC_SIZE
#define RNS_ANNOUNCE_SIGNATURE_SIZE RNS_CRYPTO_ED25519_SIGNATURE_SIZE
#define RNS_ANNOUNCE_BASE_SIZE \
    (RNS_ANNOUNCE_PUBLIC_KEY_SIZE + RNS_DESTINATION_NAME_HASH_SIZE + \
     RNS_ANNOUNCE_RANDOM_HASH_SIZE + RNS_ANNOUNCE_SIGNATURE_SIZE)
#define RNS_ANNOUNCE_MAX_APP_DATA_SIZE (RNS_PACKET_MAX_DATA_SIZE - RNS_ANNOUNCE_BASE_SIZE)
#define RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX 16

typedef struct {
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE];
    uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE];
    uint8_t signature[RNS_ANNOUNCE_SIGNATURE_SIZE];
    uint8_t app_data[RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t app_data_len;
} rns_announce_t;

typedef struct {
    bool in_use;
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t packet_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    uint8_t app_data[RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t app_data_len;
} rns_announce_known_destination_t;

esp_err_t rns_announce_encode(const rns_destination_t *destination,
                              const rns_identity_t *identity,
                              const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE],
                              const uint8_t *app_data,
                              size_t app_data_len,
                              uint8_t *out,
                              size_t out_len,
                              size_t *written);
esp_err_t rns_announce_decode(const rns_packet_t *packet,
                              rns_announce_t *out);
esp_err_t rns_announce_verify(const rns_packet_t *packet,
                              rns_announce_t *out);
esp_err_t rns_announce_verify_and_remember(const rns_packet_t *packet,
                                           rns_announce_t *out);

void rns_announce_known_reset(void);
size_t rns_announce_known_count(void);
const rns_announce_known_destination_t *rns_announce_known_get(size_t index);
const rns_announce_known_destination_t *rns_announce_recall(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE]);

#ifdef __cplusplus
}
#endif
