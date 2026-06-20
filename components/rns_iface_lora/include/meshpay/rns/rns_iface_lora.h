#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_LORA_MAX_FRAME_SIZE 255
#define RNS_LORA_FRAGMENT_HEADER_SIZE 23
#define RNS_LORA_MESSAGE_ID_SIZE 16
#define RNS_LORA_MAX_FRAGMENTS 16
#define RNS_LORA_MAX_FRAGMENT_PAYLOAD \
    (RNS_LORA_MAX_FRAME_SIZE - RNS_LORA_FRAGMENT_HEADER_SIZE)
#define RNS_LORA_DEFAULT_INIT_TIMEOUT_MS 500
#define RNS_LORA_DEFAULT_INIT_RETRIES 3

typedef struct {
    uint8_t message_id[RNS_LORA_MESSAGE_ID_SIZE];
    uint8_t index;
    uint8_t count;
    uint8_t payload[RNS_LORA_MAX_FRAGMENT_PAYLOAD];
    size_t payload_len;
} rns_lora_fragment_t;

typedef struct {
    bool active;
    uint8_t message_id[RNS_LORA_MESSAGE_ID_SIZE];
    uint8_t expected_count;
    bool received[RNS_LORA_MAX_FRAGMENTS];
    uint8_t payloads[RNS_LORA_MAX_FRAGMENTS][RNS_LORA_MAX_FRAGMENT_PAYLOAD];
    size_t payload_lens[RNS_LORA_MAX_FRAGMENTS];
    size_t received_count;
} rns_lora_reassembler_t;

typedef esp_err_t (*rns_lora_wait_ready_fn_t)(void *ctx, uint32_t timeout_ms);
typedef esp_err_t (*rns_lora_tx_fn_t)(void *ctx, const uint8_t *frame, size_t frame_len);

typedef struct {
    rns_lora_wait_ready_fn_t wait_ready;
    rns_lora_tx_fn_t tx;
    void *ctx;
    uint32_t init_timeout_ms;
    uint8_t init_retries;
} rns_lora_config_t;

typedef struct {
    rns_lora_config_t config;
    bool initialized;
    bool tx_busy;
} rns_lora_iface_t;

esp_err_t rns_iface_lora_init(rns_lora_iface_t *iface,
                              const rns_lora_config_t *config);
esp_err_t rns_iface_lora_send_frame(rns_lora_iface_t *iface,
                                    const uint8_t *frame,
                                    size_t frame_len);

esp_err_t rns_iface_lora_fragment_packet(const uint8_t *packet,
                                         size_t packet_len,
                                         size_t frame_size,
                                         rns_lora_fragment_t *fragments,
                                         size_t max_fragments,
                                         size_t *fragment_count);
esp_err_t rns_iface_lora_pack_fragment(const rns_lora_fragment_t *fragment,
                                       uint8_t *frame,
                                       size_t frame_len,
                                       size_t *written);
esp_err_t rns_iface_lora_unpack_fragment(const uint8_t *frame,
                                         size_t frame_len,
                                         rns_lora_fragment_t *fragment);
void rns_iface_lora_reassembler_init(rns_lora_reassembler_t *reassembler);
esp_err_t rns_iface_lora_reassembler_accept(rns_lora_reassembler_t *reassembler,
                                            const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t *packet,
                                            size_t packet_len,
                                            size_t *written,
                                            bool *complete);

#ifdef __cplusplus
}
#endif
