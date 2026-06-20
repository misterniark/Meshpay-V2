#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_link_request.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_REQUEST_RESPONSE_VERSION 1
#define RNS_REQUEST_ID_SIZE RNS_DESTINATION_HASH_SIZE
#define RNS_REQUEST_PATH_HASH_SIZE RNS_DESTINATION_HASH_SIZE
#define RNS_REQUEST_HEADER_SIZE 27
#define RNS_RESPONSE_HEADER_SIZE 19
#define RNS_REQUEST_MAX_DATA_SIZE (RNS_PACKET_MAX_DATA_SIZE - RNS_REQUEST_HEADER_SIZE)
#define RNS_RESPONSE_MAX_DATA_SIZE (RNS_PACKET_MAX_DATA_SIZE - RNS_RESPONSE_HEADER_SIZE)

typedef enum {
    RNS_REQUEST_RECEIPT_PENDING = 0,
    RNS_REQUEST_RECEIPT_COMPLETE,
    RNS_REQUEST_RECEIPT_TIMEOUT,
} rns_request_receipt_status_t;

typedef struct {
    uint64_t requested_at_ms;
    uint8_t path_hash[RNS_REQUEST_PATH_HASH_SIZE];
    uint8_t data[RNS_REQUEST_MAX_DATA_SIZE];
    size_t data_len;
    uint8_t request_id[RNS_REQUEST_ID_SIZE];
} rns_request_t;

typedef struct {
    bool active;
    rns_request_receipt_status_t status;
    uint8_t request_id[RNS_REQUEST_ID_SIZE];
    uint64_t sent_at_ms;
    uint32_t timeout_ms;
    uint8_t response[RNS_RESPONSE_MAX_DATA_SIZE];
    size_t response_len;
} rns_request_receipt_t;

esp_err_t rns_request_path_hash(const char *path,
                                uint8_t out[RNS_REQUEST_PATH_HASH_SIZE]);
esp_err_t rns_request_create(const rns_link_t *link,
                             const char *path,
                             const uint8_t *data,
                             size_t data_len,
                             uint64_t now_ms,
                             uint32_t timeout_ms,
                             rns_packet_t *packet,
                             rns_request_receipt_t *receipt);
esp_err_t rns_request_decode(const rns_packet_t *packet,
                             rns_request_t *request);
esp_err_t rns_response_create(const rns_link_t *link,
                              const uint8_t request_id[RNS_REQUEST_ID_SIZE],
                              const uint8_t *response,
                              size_t response_len,
                              rns_packet_t *packet);
esp_err_t rns_response_create_for_request(const rns_link_t *link,
                                          const rns_packet_t *request_packet,
                                          const uint8_t *response,
                                          size_t response_len,
                                          rns_packet_t *packet);
esp_err_t rns_request_receipt_accept_response(rns_request_receipt_t *receipt,
                                              const rns_packet_t *response_packet,
                                              uint64_t now_ms);
esp_err_t rns_request_receipt_check_timeout(rns_request_receipt_t *receipt,
                                            uint64_t now_ms);

#ifdef __cplusplus
}
#endif
