#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_destination.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_PACKET_MTU 500
#define RNS_PACKET_MDU 464
#define RNS_PACKET_HEADER_SIZE 2
#define RNS_PACKET_CONTEXT_SIZE 1
#define RNS_PACKET_ADDRESS_SIZE RNS_DESTINATION_HASH_SIZE
#define RNS_PACKET_MAX_DATA_SIZE RNS_PACKET_MDU

#define RNS_PACKET_HEADER_IFAC_MASK 0x80
#define RNS_PACKET_HEADER_TYPE_MASK 0x40
#define RNS_PACKET_HEADER_CONTEXT_FLAG_MASK 0x20
#define RNS_PACKET_HEADER_PROPAGATION_MASK 0x10
#define RNS_PACKET_HEADER_DESTINATION_MASK 0x0c
#define RNS_PACKET_HEADER_PACKET_MASK 0x03

typedef enum {
    RNS_PACKET_HEADER_TYPE_1 = 0,
    RNS_PACKET_HEADER_TYPE_2 = 1,
} rns_packet_header_type_t;

typedef enum {
    RNS_PACKET_PROPAGATION_BROADCAST = 0,
    RNS_PACKET_PROPAGATION_TRANSPORT = 1,
} rns_packet_propagation_type_t;

typedef enum {
    RNS_PACKET_TYPE_DATA = 0,
    RNS_PACKET_TYPE_ANNOUNCE = 1,
    RNS_PACKET_TYPE_LINK_REQUEST = 2,
    RNS_PACKET_TYPE_PROOF = 3,
} rns_packet_type_t;

typedef enum {
    RNS_PACKET_CONTEXT_NONE = 0x00,
    RNS_PACKET_CONTEXT_RESOURCE = 0x01,
    RNS_PACKET_CONTEXT_RESOURCE_ADV = 0x02,
    RNS_PACKET_CONTEXT_RESOURCE_REQ = 0x03,
    RNS_PACKET_CONTEXT_RESOURCE_HMU = 0x04,
    RNS_PACKET_CONTEXT_RESOURCE_PRF = 0x05,
    RNS_PACKET_CONTEXT_RESOURCE_ICL = 0x06,
    RNS_PACKET_CONTEXT_RESOURCE_RCL = 0x07,
    RNS_PACKET_CONTEXT_CACHE_REQUEST = 0x08,
    RNS_PACKET_CONTEXT_REQUEST = 0x09,
    RNS_PACKET_CONTEXT_RESPONSE = 0x0a,
    RNS_PACKET_CONTEXT_PATH_RESPONSE = 0x0b,
    RNS_PACKET_CONTEXT_COMMAND = 0x0c,
    RNS_PACKET_CONTEXT_COMMAND_STATUS = 0x0d,
    RNS_PACKET_CONTEXT_CHANNEL = 0x0e,
    RNS_PACKET_CONTEXT_KEEPALIVE = 0xfa,
    RNS_PACKET_CONTEXT_LINKIDENTIFY = 0xfb,
    RNS_PACKET_CONTEXT_LINKCLOSE = 0xfc,
    RNS_PACKET_CONTEXT_LINKPROOF = 0xfd,
    RNS_PACKET_CONTEXT_LRRTT = 0xfe,
    RNS_PACKET_CONTEXT_LRPROOF = 0xff,
} rns_packet_context_t;

typedef struct {
    rns_packet_header_type_t header_type;
    bool context_flag;
    rns_packet_propagation_type_t propagation_type;
    rns_destination_type_t destination_type;
    rns_packet_type_t packet_type;
    uint8_t hops;
    uint8_t destination_hash[RNS_PACKET_ADDRESS_SIZE];
    uint8_t transport_id[RNS_PACKET_ADDRESS_SIZE];
    uint8_t context;
    uint8_t data[RNS_PACKET_MAX_DATA_SIZE];
    size_t data_len;
} rns_packet_t;

void rns_packet_clear(rns_packet_t *packet);
size_t rns_packet_packed_size(const rns_packet_t *packet);
esp_err_t rns_packet_pack(const rns_packet_t *packet, uint8_t *out,
                          size_t out_len, size_t *written);
esp_err_t rns_packet_unpack(const uint8_t *wire, size_t wire_len,
                            rns_packet_t *out);
uint8_t rns_packet_encode_header_byte(const rns_packet_t *packet);
esp_err_t rns_packet_increment_hops(rns_packet_t *packet);
esp_err_t rns_packet_hash(const rns_packet_t *packet,
                          uint8_t out[RNS_CRYPTO_SHA256_SIZE]);
esp_err_t rns_packet_truncated_hash(const rns_packet_t *packet,
                                    uint8_t out[RNS_DESTINATION_HASH_SIZE]);

#ifdef __cplusplus
}
#endif
