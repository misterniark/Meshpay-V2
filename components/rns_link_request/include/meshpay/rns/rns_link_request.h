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

#define RNS_LINK_EC_PUBLIC_SIZE 32
#define RNS_LINK_SIGN_PUBLIC_SIZE 32
#define RNS_LINK_PUBLIC_KEY_SIZE (RNS_LINK_EC_PUBLIC_SIZE + RNS_LINK_SIGN_PUBLIC_SIZE)
#define RNS_LINK_MTU_SIGNAL_SIZE 3
#define RNS_LINK_REQUEST_MIN_SIZE RNS_LINK_PUBLIC_KEY_SIZE
#define RNS_LINK_REQUEST_MAX_SIZE (RNS_LINK_PUBLIC_KEY_SIZE + RNS_LINK_MTU_SIGNAL_SIZE)
#define RNS_LINK_PROOF_MIN_SIZE \
    (RNS_CRYPTO_ED25519_SIGNATURE_SIZE + RNS_LINK_EC_PUBLIC_SIZE)
#define RNS_LINK_PROOF_MAX_SIZE (RNS_LINK_PROOF_MIN_SIZE + RNS_LINK_MTU_SIGNAL_SIZE)
#define RNS_LINK_MTU_BYTEMASK 0x1fffff
#define RNS_LINK_MODE_BYTEMASK 0xe0
#define RNS_LINK_MODE_AES256_CBC 0x01

typedef enum {
    RNS_LINK_STATUS_PENDING = 0,
    RNS_LINK_STATUS_HANDSHAKE = 1,
    RNS_LINK_STATUS_ACTIVE = 2,
    RNS_LINK_STATUS_CLOSED = 4,
} rns_link_status_t;

typedef struct {
    bool initiator;
    rns_link_status_t status;
    rns_identity_t local_identity;
    uint8_t peer_x25519_public[RNS_LINK_EC_PUBLIC_SIZE];
    uint8_t link_id[RNS_DESTINATION_HASH_SIZE];
    uint8_t shared_key[RNS_CRYPTO_X25519_SHARED_SIZE];
    uint32_t mtu;
    uint8_t mode;
} rns_link_t;

void rns_link_clear(rns_link_t *link);
esp_err_t rns_link_signalling_bytes(uint32_t mtu,
                                    uint8_t mode,
                                    uint8_t out[RNS_LINK_MTU_SIGNAL_SIZE]);
esp_err_t rns_link_request_link_id(const rns_packet_t *request_packet,
                                   uint8_t out[RNS_DESTINATION_HASH_SIZE]);
esp_err_t rns_link_request_create(const rns_destination_t *destination,
                                  uint32_t mtu,
                                  rns_link_t *link,
                                  rns_packet_t *request_packet);
esp_err_t rns_link_request_accept(const rns_identity_t *owner_identity,
                                  const rns_packet_t *request_packet,
                                  uint32_t mtu,
                                  rns_link_t *link,
                                  rns_packet_t *proof_packet);
esp_err_t rns_link_request_validate_proof(const rns_identity_t *destination_identity,
                                          const rns_packet_t *proof_packet,
                                          rns_link_t *link);

#ifdef __cplusplus
}
#endif
