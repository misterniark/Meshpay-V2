#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_identity.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_DESTINATION_NAME_HASH_SIZE 10
#define RNS_DESTINATION_HASH_SIZE 16
#define RNS_DESTINATION_MAX_FULL_NAME 64

#define RNS_MESHPAY_APP_NAME "meshpay"
#define RNS_MESHPAY_WALLET_ASPECT "wallet"
#define RNS_MESHPAY_WALLET_FULL_NAME "meshpay.wallet"

typedef enum {
    RNS_DESTINATION_TYPE_SINGLE = 0,
    RNS_DESTINATION_TYPE_GROUP = 1,
    RNS_DESTINATION_TYPE_PLAIN = 2,
    RNS_DESTINATION_TYPE_LINK = 3,
} rns_destination_type_t;

typedef struct {
    rns_destination_type_t type;
    uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE];
    uint8_t hash[RNS_DESTINATION_HASH_SIZE];
    char full_name[RNS_DESTINATION_MAX_FULL_NAME];
} rns_destination_t;

esp_err_t rns_destination_build_full_name(const char *app_name,
                                          const char *const *aspects,
                                          size_t aspect_count,
                                          char *out, size_t out_len);
esp_err_t rns_destination_name_hash(const char *app_name,
                                    const char *const *aspects,
                                    size_t aspect_count,
                                    uint8_t out[RNS_DESTINATION_NAME_HASH_SIZE]);
esp_err_t rns_destination_create_single(const rns_identity_t *identity,
                                        const char *app_name,
                                        const char *const *aspects,
                                        size_t aspect_count,
                                        rns_destination_t *out);
esp_err_t rns_destination_create_plain(const char *app_name,
                                       const char *const *aspects,
                                       size_t aspect_count,
                                       rns_destination_t *out);
esp_err_t rns_destination_create_link(const uint8_t link_hash[RNS_DESTINATION_HASH_SIZE],
                                      rns_destination_t *out);
esp_err_t rns_destination_create_meshpay_wallet(const rns_identity_t *identity,
                                                rns_destination_t *out);
bool rns_destination_hash_equal(const uint8_t a[RNS_DESTINATION_HASH_SIZE],
                                const uint8_t b[RNS_DESTINATION_HASH_SIZE]);

#ifdef __cplusplus
}
#endif

