#include "meshpay/rns/rns_radio.h"

#include <string.h>

static size_t espnow_payload_capacity_for_frame(size_t frame_size)
{
    if (frame_size <= RNS_ESPNOW_FRAGMENT_HEADER_SIZE ||
        frame_size > RNS_ESPNOW_MAX_FRAME_SIZE) {
        return 0;
    }
    return frame_size - RNS_ESPNOW_FRAGMENT_HEADER_SIZE;
}

static size_t lora_payload_capacity_for_frame(size_t frame_size)
{
    if (frame_size <= RNS_LORA_FRAGMENT_HEADER_SIZE ||
        frame_size > RNS_LORA_MAX_FRAME_SIZE) {
        return 0;
    }
    return frame_size - RNS_LORA_FRAGMENT_HEADER_SIZE;
}

esp_err_t rns_radio_init(rns_radio_t *radio,
                         meshpay_hal_t *hal,
                         uint8_t enabled_bearers)
{
    if (radio == NULL || hal == NULL || enabled_bearers == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(radio, 0, sizeof(*radio));
    radio->hal = hal;
    radio->enabled_bearers = enabled_bearers;
    radio->espnow_frame_size = RNS_ESPNOW_DEFAULT_FRAME_SIZE;
    radio->lora_frame_size = RNS_LORA_MAX_FRAME_SIZE;
    rns_iface_espnow_reassembler_init(&radio->espnow_reassembler);
    rns_iface_lora_reassembler_init(&radio->lora_reassembler);
    return ESP_OK;
}

esp_err_t rns_radio_set_bearer_selector(
    rns_radio_t *radio,
    rns_radio_bearer_selector_t selector,
    void *ctx)
{
    if (radio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    radio->bearer_selector = selector;
    radio->bearer_selector_ctx = ctx;
    return ESP_OK;
}

static esp_err_t send_espnow(rns_radio_t *radio,
                             const uint8_t *wire,
                             size_t wire_len)
{
    size_t payload_max =
        espnow_payload_capacity_for_frame(radio->espnow_frame_size);
    if (payload_max == 0 ||
        payload_max > RNS_ESPNOW_MAX_FRAGMENT_PAYLOAD ||
        wire_len == 0 ||
        wire_len > RNS_PACKET_MTU) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t fragment_count = (wire_len + payload_max - 1) / payload_max;
    if (fragment_count == 0 ||
        fragment_count > RNS_ESPNOW_MAX_FRAGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t message_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(wire, wire_len, message_hash);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < fragment_count; ++i) {
        size_t offset = i * payload_max;
        size_t remaining = wire_len - offset;
        size_t take = remaining > payload_max ? payload_max : remaining;

        rns_espnow_fragment_t fragment = {
            .index = (uint8_t)i,
            .count = (uint8_t)fragment_count,
            .payload_len = take,
        };
        memcpy(fragment.message_id,
               message_hash,
               RNS_ESPNOW_MESSAGE_ID_SIZE);
        memcpy(fragment.payload, wire + offset, take);

        uint8_t frame[RNS_ESPNOW_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        err = rns_iface_espnow_pack_fragment(&fragment,
                                             frame,
                                             sizeof(frame),
                                             &frame_len);
        rns_crypto_secure_zero(&fragment, sizeof(fragment));
        if (err != ESP_OK) {
            rns_crypto_secure_zero(message_hash, sizeof(message_hash));
            return err;
        }
        err = meshpay_hal_espnow_send(radio->hal, frame, frame_len);
        rns_crypto_secure_zero(frame, sizeof(frame));
        if (err != ESP_OK) {
            rns_crypto_secure_zero(message_hash, sizeof(message_hash));
            return err;
        }
        radio->tx_frames_espnow++;
    }
    rns_crypto_secure_zero(message_hash, sizeof(message_hash));
    return ESP_OK;
}

static esp_err_t send_lora(rns_radio_t *radio,
                           const uint8_t *wire,
                           size_t wire_len)
{
    size_t payload_max =
        lora_payload_capacity_for_frame(radio->lora_frame_size);
    if (payload_max == 0 ||
        payload_max > RNS_LORA_MAX_FRAGMENT_PAYLOAD ||
        wire_len == 0 ||
        wire_len > RNS_PACKET_MTU) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t fragment_count = (wire_len + payload_max - 1) / payload_max;
    if (fragment_count == 0 ||
        fragment_count > RNS_LORA_MAX_FRAGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t message_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(wire, wire_len, message_hash);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < fragment_count; ++i) {
        size_t offset = i * payload_max;
        size_t remaining = wire_len - offset;
        size_t take = remaining > payload_max ? payload_max : remaining;

        rns_lora_fragment_t fragment = {
            .index = (uint8_t)i,
            .count = (uint8_t)fragment_count,
            .payload_len = take,
        };
        memcpy(fragment.message_id, message_hash, RNS_LORA_MESSAGE_ID_SIZE);
        memcpy(fragment.payload, wire + offset, take);

        uint8_t frame[RNS_LORA_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        err = rns_iface_lora_pack_fragment(&fragment,
                                           frame,
                                           sizeof(frame),
                                           &frame_len);
        rns_crypto_secure_zero(&fragment, sizeof(fragment));
        if (err != ESP_OK) {
            rns_crypto_secure_zero(message_hash, sizeof(message_hash));
            return err;
        }
        err = meshpay_hal_lora_send(radio->hal, frame, frame_len);
        rns_crypto_secure_zero(frame, sizeof(frame));
        if (err != ESP_OK) {
            rns_crypto_secure_zero(message_hash, sizeof(message_hash));
            return err;
        }
        radio->tx_frames_lora++;
    }
    rns_crypto_secure_zero(message_hash, sizeof(message_hash));
    return ESP_OK;
}

esp_err_t rns_radio_send_packet_over(rns_radio_t *radio,
                                     const rns_packet_t *packet,
                                     uint8_t bearers)
{
    if (radio == NULL || packet == NULL || radio->hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bearers &= radio->enabled_bearers;
    if (bearers == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t wire[RNS_PACKET_MTU];
    size_t wire_len = 0;
    esp_err_t err = rns_packet_pack(packet, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }

    if ((bearers & RNS_RADIO_BEARER_ESPNOW) != 0) {
        err = send_espnow(radio, wire, wire_len);
        if (err != ESP_OK) {
            rns_crypto_secure_zero(wire, sizeof(wire));
            return err;
        }
    }
    if ((bearers & RNS_RADIO_BEARER_LORA) != 0) {
        err = send_lora(radio, wire, wire_len);
        if (err != ESP_OK) {
            rns_crypto_secure_zero(wire, sizeof(wire));
            return err;
        }
    }

    radio->tx_packets++;
    rns_crypto_secure_zero(wire, sizeof(wire));
    return ESP_OK;
}

esp_err_t rns_radio_send_packet(rns_radio_t *radio,
                                const rns_packet_t *packet)
{
    if (radio == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t bearers = radio->enabled_bearers;
    if (radio->bearer_selector != NULL) {
        uint8_t selected = radio->bearer_selector(
            packet,
            radio->bearer_selector_ctx);
        if (selected != 0) {
            bearers = selected;
        }
    }
    return rns_radio_send_packet_over(radio, packet, bearers);
}

static esp_err_t dispatch_wire(rns_radio_t *radio,
                               rns_node_t *node,
                               const uint8_t *wire,
                               size_t wire_len,
                               rns_transport_rx_result_t *result)
{
    esp_err_t err = rns_node_poll(node, wire, wire_len, result);
    if (err == ESP_OK) {
        radio->rx_packets++;
    }
    return err;
}

esp_err_t rns_radio_receive_espnow_frame(rns_radio_t *radio,
                                         rns_node_t *node,
                                         const uint8_t *frame,
                                         size_t frame_len,
                                         rns_transport_rx_result_t *result)
{
    if (radio == NULL || node == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t wire[RNS_PACKET_MTU];
    size_t wire_len = 0;
    bool complete = false;
    esp_err_t err = rns_iface_espnow_reassembler_accept(
        &radio->espnow_reassembler,
        frame,
        frame_len,
        wire,
        sizeof(wire),
        &wire_len,
        &complete);
    if (err != ESP_OK) {
        return err;
    }
    radio->rx_frames_espnow++;
    if (!complete) {
        return ESP_OK;
    }
    return dispatch_wire(radio, node, wire, wire_len, result);
}

esp_err_t rns_radio_receive_lora_frame(rns_radio_t *radio,
                                       rns_node_t *node,
                                       const uint8_t *frame,
                                       size_t frame_len,
                                       rns_transport_rx_result_t *result)
{
    if (radio == NULL || node == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t wire[RNS_PACKET_MTU];
    size_t wire_len = 0;
    bool complete = false;
    esp_err_t err = rns_iface_lora_reassembler_accept(
        &radio->lora_reassembler,
        frame,
        frame_len,
        wire,
        sizeof(wire),
        &wire_len,
        &complete);
    if (err != ESP_OK) {
        return err;
    }
    radio->rx_frames_lora++;
    if (!complete) {
        return ESP_OK;
    }
    return dispatch_wire(radio, node, wire, wire_len, result);
}

esp_err_t rns_radio_poll_hal(rns_radio_t *radio,
                             rns_node_t *node,
                             rns_transport_rx_result_t *result)
{
    if (radio == NULL || node == NULL || radio->hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[MESHPAY_HAL_PACKET_MAX];
    size_t frame_len = 0;
    if ((radio->enabled_bearers & RNS_RADIO_BEARER_ESPNOW) != 0) {
        esp_err_t err = meshpay_hal_espnow_recv(radio->hal,
                                                frame,
                                                sizeof(frame),
                                                &frame_len);
        if (err == ESP_OK) {
            return rns_radio_receive_espnow_frame(radio,
                                                  node,
                                                  frame,
                                                  frame_len,
                                                  result);
        }
        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
    }

    if ((radio->enabled_bearers & RNS_RADIO_BEARER_LORA) != 0) {
        esp_err_t err = meshpay_hal_lora_recv(radio->hal,
                                              frame,
                                              sizeof(frame),
                                              &frame_len);
        if (err == ESP_OK) {
            return rns_radio_receive_lora_frame(radio,
                                                node,
                                                frame,
                                                frame_len,
                                                result);
        }
        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t adapter_tx(rns_node_t *node,
                            const rns_packet_t *packet,
                            void *ctx)
{
    (void)node;
    rns_radio_node_adapter_t *adapter = (rns_radio_node_adapter_t *)ctx;
    if (adapter == NULL || adapter->radio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adapter->upper_callbacks.tx != NULL) {
        esp_err_t err = adapter->upper_callbacks.tx(
            node,
            packet,
            adapter->upper_callbacks.ctx);
        if (err != ESP_OK) {
            return err;
        }
    }
    return rns_radio_send_packet(adapter->radio, packet);
}

static esp_err_t adapter_forward(rns_node_t *node,
                                 const rns_packet_t *packet,
                                 void *ctx,
                                 rns_node_packet_cb_t cb)
{
    rns_radio_node_adapter_t *adapter = (rns_radio_node_adapter_t *)ctx;
    if (adapter == NULL || cb == NULL) {
        return ESP_OK;
    }
    return cb(node, packet, adapter->upper_callbacks.ctx);
}

static esp_err_t adapter_rx(rns_node_t *node,
                            const rns_packet_t *packet,
                            void *ctx)
{
    rns_radio_node_adapter_t *adapter = (rns_radio_node_adapter_t *)ctx;
    return adapter_forward(node,
                           packet,
                           ctx,
                           adapter == NULL ? NULL : adapter->upper_callbacks.rx);
}

static esp_err_t adapter_proof(rns_node_t *node,
                               const rns_packet_t *packet,
                               void *ctx)
{
    rns_radio_node_adapter_t *adapter = (rns_radio_node_adapter_t *)ctx;
    return adapter_forward(node,
                           packet,
                           ctx,
                           adapter == NULL ? NULL : adapter->upper_callbacks.proof);
}

static esp_err_t adapter_request(rns_node_t *node,
                                 const rns_packet_t *packet,
                                 void *ctx)
{
    rns_radio_node_adapter_t *adapter = (rns_radio_node_adapter_t *)ctx;
    return adapter_forward(node,
                           packet,
                           ctx,
                           adapter == NULL ? NULL : adapter->upper_callbacks.request);
}

esp_err_t rns_radio_bind_node(rns_radio_node_adapter_t *adapter,
                              rns_radio_t *radio,
                              rns_node_t *node,
                              const rns_node_callbacks_t *upper_callbacks)
{
    if (adapter == NULL || radio == NULL || node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->radio = radio;
    if (upper_callbacks != NULL) {
        adapter->upper_callbacks = *upper_callbacks;
    }

    const rns_node_callbacks_t callbacks = {
        .tx = adapter_tx,
        .rx = adapter_rx,
        .proof = adapter_proof,
        .request = adapter_request,
        .ctx = adapter,
    };
    return rns_node_set_callbacks(node, &callbacks);
}
