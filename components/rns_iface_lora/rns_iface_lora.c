#include "meshpay/rns/rns_iface_lora.h"

#include <string.h>

#define RNS_LORA_MAGIC_0 0x4d
#define RNS_LORA_MAGIC_1 0x4c
#define RNS_LORA_FRAME_VERSION 0x01

static size_t payload_capacity_for_frame(size_t frame_size)
{
    if (frame_size <= RNS_LORA_FRAGMENT_HEADER_SIZE ||
        frame_size > RNS_LORA_MAX_FRAME_SIZE) {
        return 0;
    }
    return frame_size - RNS_LORA_FRAGMENT_HEADER_SIZE;
}

esp_err_t rns_iface_lora_init(rns_lora_iface_t *iface,
                              const rns_lora_config_t *config)
{
    if (iface == NULL || config == NULL || config->wait_ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(iface, 0, sizeof(*iface));
    iface->config = *config;
    if (iface->config.init_timeout_ms == 0) {
        iface->config.init_timeout_ms = RNS_LORA_DEFAULT_INIT_TIMEOUT_MS;
    }

    uint8_t retries = iface->config.init_retries;
    if (retries == 0) {
        retries = RNS_LORA_DEFAULT_INIT_RETRIES;
    }

    esp_err_t last_err = ESP_FAIL;
    for (uint8_t attempt = 0; attempt <= retries; ++attempt) {
        last_err = iface->config.wait_ready(iface->config.ctx,
                                            iface->config.init_timeout_ms);
        if (last_err == ESP_OK) {
            iface->initialized = true;
            return ESP_OK;
        }
        if (last_err != ESP_ERR_TIMEOUT) {
            return last_err;
        }
    }

    return last_err;
}

esp_err_t rns_iface_lora_send_frame(rns_lora_iface_t *iface,
                                    const uint8_t *frame,
                                    size_t frame_len)
{
    if (iface == NULL || frame == NULL || frame_len == 0 ||
        frame_len > RNS_LORA_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!iface->initialized || iface->config.tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (iface->tx_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    iface->tx_busy = true;
    esp_err_t err = iface->config.tx(iface->config.ctx, frame, frame_len);
    iface->tx_busy = false;
    return err;
}

esp_err_t rns_iface_lora_fragment_packet(const uint8_t *packet,
                                         size_t packet_len,
                                         size_t frame_size,
                                         rns_lora_fragment_t *fragments,
                                         size_t max_fragments,
                                         size_t *fragment_count)
{
    if (packet == NULL || fragments == NULL || fragment_count == NULL ||
        packet_len == 0 || packet_len > RNS_PACKET_MTU) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t payload_max = payload_capacity_for_frame(frame_size);
    if (payload_max == 0 || payload_max > RNS_LORA_MAX_FRAGMENT_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t count = (packet_len + payload_max - 1) / payload_max;
    if (count == 0 || count > RNS_LORA_MAX_FRAGMENTS || max_fragments < count) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(packet, packet_len, full_hash);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(full_hash, sizeof(full_hash));
        return err;
    }

    for (size_t i = 0; i < count; ++i) {
        size_t offset = i * payload_max;
        size_t remaining = packet_len - offset;
        size_t take = remaining > payload_max ? payload_max : remaining;

        memset(&fragments[i], 0, sizeof(fragments[i]));
        memcpy(fragments[i].message_id, full_hash, RNS_LORA_MESSAGE_ID_SIZE);
        fragments[i].index = (uint8_t)i;
        fragments[i].count = (uint8_t)count;
        memcpy(fragments[i].payload, packet + offset, take);
        fragments[i].payload_len = take;
    }

    *fragment_count = count;
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return ESP_OK;
}

esp_err_t rns_iface_lora_pack_fragment(const rns_lora_fragment_t *fragment,
                                       uint8_t *frame,
                                       size_t frame_len,
                                       size_t *written)
{
    if (fragment == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment->count == 0 ||
        fragment->count > RNS_LORA_MAX_FRAGMENTS ||
        fragment->index >= fragment->count ||
        fragment->payload_len > RNS_LORA_MAX_FRAGMENT_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t needed = RNS_LORA_FRAGMENT_HEADER_SIZE + fragment->payload_len;
    if (frame_len < needed || needed > RNS_LORA_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    frame[pos++] = RNS_LORA_MAGIC_0;
    frame[pos++] = RNS_LORA_MAGIC_1;
    frame[pos++] = RNS_LORA_FRAME_VERSION;
    frame[pos++] = fragment->index;
    frame[pos++] = fragment->count;
    frame[pos++] = (uint8_t)((fragment->payload_len >> 8) & 0xff);
    frame[pos++] = (uint8_t)(fragment->payload_len & 0xff);
    memcpy(frame + pos, fragment->message_id, RNS_LORA_MESSAGE_ID_SIZE);
    pos += RNS_LORA_MESSAGE_ID_SIZE;
    memcpy(frame + pos, fragment->payload, fragment->payload_len);
    pos += fragment->payload_len;

    if (written != NULL) {
        *written = pos;
    }
    return ESP_OK;
}

esp_err_t rns_iface_lora_unpack_fragment(const uint8_t *frame,
                                         size_t frame_len,
                                         rns_lora_fragment_t *fragment)
{
    if (frame == NULL || fragment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_len < RNS_LORA_FRAGMENT_HEADER_SIZE ||
        frame_len > RNS_LORA_MAX_FRAME_SIZE ||
        frame[0] != RNS_LORA_MAGIC_0 ||
        frame[1] != RNS_LORA_MAGIC_1 ||
        frame[2] != RNS_LORA_FRAME_VERSION) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(fragment, 0, sizeof(*fragment));
    size_t pos = 3;
    fragment->index = frame[pos++];
    fragment->count = frame[pos++];
    fragment->payload_len = ((size_t)frame[pos] << 8) | frame[pos + 1];
    pos += 2;
    if (fragment->count == 0 ||
        fragment->count > RNS_LORA_MAX_FRAGMENTS ||
        fragment->index >= fragment->count ||
        fragment->payload_len > RNS_LORA_MAX_FRAGMENT_PAYLOAD ||
        frame_len != RNS_LORA_FRAGMENT_HEADER_SIZE + fragment->payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(fragment->message_id, frame + pos, RNS_LORA_MESSAGE_ID_SIZE);
    pos += RNS_LORA_MESSAGE_ID_SIZE;
    memcpy(fragment->payload, frame + pos, fragment->payload_len);
    return ESP_OK;
}

void rns_iface_lora_reassembler_init(rns_lora_reassembler_t *reassembler)
{
    if (reassembler == NULL) {
        return;
    }
    memset(reassembler, 0, sizeof(*reassembler));
}

static bool same_message(const rns_lora_reassembler_t *reassembler,
                         const rns_lora_fragment_t *fragment)
{
    return reassembler->active &&
           reassembler->expected_count == fragment->count &&
           rns_crypto_constant_equal(reassembler->message_id,
                                     fragment->message_id,
                                     RNS_LORA_MESSAGE_ID_SIZE);
}

static void start_message(rns_lora_reassembler_t *reassembler,
                          const rns_lora_fragment_t *fragment)
{
    memset(reassembler, 0, sizeof(*reassembler));
    reassembler->active = true;
    reassembler->expected_count = fragment->count;
    memcpy(reassembler->message_id, fragment->message_id, RNS_LORA_MESSAGE_ID_SIZE);
}

esp_err_t rns_iface_lora_reassembler_accept(rns_lora_reassembler_t *reassembler,
                                            const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t *packet,
                                            size_t packet_len,
                                            size_t *written,
                                            bool *complete)
{
    if (reassembler == NULL || frame == NULL || packet == NULL || complete == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *complete = false;
    if (written != NULL) {
        *written = 0;
    }

    rns_lora_fragment_t fragment;
    esp_err_t err = rns_iface_lora_unpack_fragment(frame, frame_len, &fragment);
    if (err != ESP_OK) {
        return err;
    }

    if (!same_message(reassembler, &fragment)) {
        start_message(reassembler, &fragment);
    }

    if (reassembler->received[fragment.index]) {
        if (reassembler->payload_lens[fragment.index] != fragment.payload_len ||
            memcmp(reassembler->payloads[fragment.index],
                   fragment.payload,
                   fragment.payload_len) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        memcpy(reassembler->payloads[fragment.index],
               fragment.payload,
               fragment.payload_len);
        reassembler->payload_lens[fragment.index] = fragment.payload_len;
        reassembler->received[fragment.index] = true;
        reassembler->received_count++;
    }

    if (reassembler->received_count != reassembler->expected_count) {
        return ESP_OK;
    }

    size_t total = 0;
    for (size_t i = 0; i < reassembler->expected_count; ++i) {
        total += reassembler->payload_lens[i];
    }
    if (total > RNS_PACKET_MTU || packet_len < total) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    for (size_t i = 0; i < reassembler->expected_count; ++i) {
        memcpy(packet + pos, reassembler->payloads[i], reassembler->payload_lens[i]);
        pos += reassembler->payload_lens[i];
    }

    if (written != NULL) {
        *written = pos;
    }
    *complete = true;
    return ESP_OK;
}
