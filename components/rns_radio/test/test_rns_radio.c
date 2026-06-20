#include "meshpay/rns/rns_radio.h"
#include "unity.h"

#include <string.h>

typedef struct {
    size_t rx_count;
    size_t tx_count;
    size_t proof_count;
    size_t request_count;
    rns_packet_t last_rx;
    rns_packet_t last_tx;
    rns_packet_t last_proof;
    rns_packet_t last_request;
} radio_node_ctx_t;

typedef struct {
    esp_err_t espnow_result;
    esp_err_t lora_result;
    size_t espnow_count;
    size_t lora_count;
} radio_recording_hal_ctx_t;

static esp_err_t recording_espnow_send(void *ctx,
                                       const uint8_t *data,
                                       size_t len)
{
    radio_recording_hal_ctx_t *recording =
        (radio_recording_hal_ctx_t *)ctx;
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    recording->espnow_count++;
    return recording->espnow_result;
}

static esp_err_t recording_lora_send(void *ctx,
                                     const uint8_t *data,
                                     size_t len)
{
    radio_recording_hal_ctx_t *recording =
        (radio_recording_hal_ctx_t *)ctx;
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    recording->lora_count++;
    return recording->lora_result;
}

static esp_err_t recording_recv_timeout(void *ctx,
                                        uint8_t *data,
                                        size_t size,
                                        size_t *len)
{
    (void)ctx;
    (void)data;
    (void)size;
    (void)len;
    return ESP_ERR_TIMEOUT;
}

static const meshpay_hal_ops_t RECORDING_RADIO_OPS = {
    .espnow_send = recording_espnow_send,
    .espnow_recv = recording_recv_timeout,
    .lora_send = recording_lora_send,
    .lora_recv = recording_recv_timeout,
};

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void load_identity(rns_identity_t *identity, uint8_t seed_base)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(private_key, sizeof(private_key), seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

static esp_err_t rx_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    radio_node_ctx_t *radio_ctx = (radio_node_ctx_t *)ctx;
    radio_ctx->rx_count++;
    radio_ctx->last_rx = *packet;
    return ESP_OK;
}

static esp_err_t tx_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    radio_node_ctx_t *radio_ctx = (radio_node_ctx_t *)ctx;
    radio_ctx->tx_count++;
    radio_ctx->last_tx = *packet;
    return ESP_OK;
}

static esp_err_t proof_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    radio_node_ctx_t *radio_ctx = (radio_node_ctx_t *)ctx;
    radio_ctx->proof_count++;
    radio_ctx->last_proof = *packet;
    return ESP_OK;
}

static esp_err_t request_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    radio_node_ctx_t *radio_ctx = (radio_node_ctx_t *)ctx;
    radio_ctx->request_count++;
    radio_ctx->last_request = *packet;
    return ESP_OK;
}

static void init_node(rns_node_t *node,
                      radio_node_ctx_t *ctx,
                      uint8_t seed_base)
{
    rns_identity_t identity;
    load_identity(&identity, seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_init(node, &identity));
    const rns_node_callbacks_t callbacks = {
        .rx = rx_cb,
        .ctx = ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_set_callbacks(node, &callbacks));
}

static void make_data_packet(const rns_node_t *node, rns_packet_t *packet)
{
    rns_packet_clear(packet);
    packet->destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    memcpy(packet->destination_hash, node->destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    memcpy(packet->data, "radio", 5);
    packet->data_len = 5;
}

static uint8_t test_select_bearer(const rns_packet_t *packet, void *ctx)
{
    (void)ctx;
    if (packet == NULL) {
        return 0;
    }
    if (packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE) {
        return RNS_RADIO_BEARER_ESPNOW;
    }
    if (packet->context == RNS_PACKET_CONTEXT_RESOURCE ||
        packet->context == RNS_PACKET_CONTEXT_REQUEST) {
        return RNS_RADIO_BEARER_LORA;
    }
    return RNS_RADIO_BEARER_ESPNOW;
}

TEST_CASE("rns radio sends packet over espnow and lora bearers", "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_LILYGO_T5S3_H752);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ALL));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x21);

    rns_packet_t packet;
    make_data_packet(&node, &packet);
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_send_packet(&radio, &packet));
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_packets);
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_frames_espnow);
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_frames_lora);
    TEST_ASSERT_GREATER_THAN_UINT32(0, mock.espnow_len);
    TEST_ASSERT_GREATER_THAN_UINT32(0, mock.lora_len);
}

TEST_CASE("rns radio requires all selected bearers to transmit",
          "[rns_radio]")
{
    radio_recording_hal_ctx_t recording = {
        .espnow_result = ESP_OK,
        .lora_result = ESP_ERR_TIMEOUT,
    };
    meshpay_hal_t hal;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_hal_init(&hal,
                                       MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
                                       &RECORDING_RADIO_OPS,
                                       &recording));

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ALL));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x25);

    rns_packet_t packet;
    make_data_packet(&node, &packet);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      rns_radio_send_packet_over(&radio,
                                                 &packet,
                                                 RNS_RADIO_BEARER_ALL));
    TEST_ASSERT_EQUAL_UINT32(1, recording.espnow_count);
    TEST_ASSERT_EQUAL_UINT32(1, recording.lora_count);
    TEST_ASSERT_EQUAL_UINT32(0, radio.tx_packets);
}

TEST_CASE("rns radio bearer selector routes announce and resource packets",
          "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_WAVESHARE_S3_TOUCH);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ALL));
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_set_bearer_selector(
                                  &radio,
                                  test_select_bearer,
                                  NULL));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x29);

    rns_packet_t packet;
    make_data_packet(&node, &packet);
    packet.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_send_packet(&radio, &packet));
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_packets);
    TEST_ASSERT_GREATER_THAN_UINT32(0, mock.espnow_len);
    TEST_ASSERT_EQUAL_UINT32(0, mock.lora_len);

    mock.espnow_len = 0;
    mock.lora_len = 0;
    make_data_packet(&node, &packet);
    packet.context = RNS_PACKET_CONTEXT_RESOURCE;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_send_packet(&radio, &packet));
    TEST_ASSERT_EQUAL_UINT32(2, radio.tx_packets);
    TEST_ASSERT_EQUAL_UINT32(0, mock.espnow_len);
    TEST_ASSERT_GREATER_THAN_UINT32(0, mock.lora_len);
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_frames_espnow);
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_frames_lora);
}

TEST_CASE("rns radio reassembles espnow frame into node poll", "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_CYD);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ESPNOW));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x31);

    rns_packet_t packet;
    make_data_packet(&node, &packet);
    uint8_t wire[RNS_PACKET_MTU];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_pack(&packet, wire,
                                              sizeof(wire), &wire_len));

    rns_espnow_fragment_t fragments[RNS_ESPNOW_MAX_FRAGMENTS];
    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_fragment_packet(
                                  wire, wire_len,
                                  RNS_ESPNOW_DEFAULT_FRAME_SIZE,
                                  fragments,
                                  RNS_ESPNOW_MAX_FRAGMENTS,
                                  &fragment_count));
    TEST_ASSERT_EQUAL_UINT32(1, fragment_count);

    uint8_t frame[RNS_ESPNOW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_pack_fragment(&fragments[0],
                                                             frame,
                                                             sizeof(frame),
                                                             &frame_len));

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_receive_espnow_frame(&radio,
                                                             &node,
                                                             frame,
                                                             frame_len,
                                                             &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.rx_count);
    TEST_ASSERT_EQUAL_MEMORY(packet.data, ctx.last_rx.data, packet.data_len);
    TEST_ASSERT_EQUAL_UINT32(1, radio.rx_packets);
}

TEST_CASE("rns radio polls hal and reports timeout when idle", "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_WAVESHARE_S3_TOUCH);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ALL));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x41);

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, rns_radio_poll_hal(&radio,
                                                          &node,
                                                          &result));
}

TEST_CASE("rns radio adapter binds node tx while preserving upper rx", "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_CYD);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ESPNOW));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x51);

    rns_radio_node_adapter_t adapter;
    const rns_node_callbacks_t upper = {
        .tx = tx_cb,
        .rx = rx_cb,
        .ctx = &ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_bind_node(&adapter,
                                                  &radio,
                                                  &node,
                                                  &upper));

    rns_packet_t packet;
    make_data_packet(&node, &packet);
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_send_packet(&node, &packet));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.tx_count);
    TEST_ASSERT_EQUAL_MEMORY(packet.data, ctx.last_tx.data, packet.data_len);
    TEST_ASSERT_EQUAL_UINT32(1, radio.tx_packets);
    TEST_ASSERT_GREATER_THAN_UINT32(0, mock.espnow_len);

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_poll_hal(&radio, &node, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.rx_count);
}

TEST_CASE("rns radio adapter preserves proof and request callbacks",
          "[rns_radio]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_CYD);

    rns_radio_t radio;
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_init(&radio, &hal,
                                             RNS_RADIO_BEARER_ESPNOW));

    rns_node_t node;
    radio_node_ctx_t ctx = {0};
    init_node(&node, &ctx, 0x61);

    rns_radio_node_adapter_t adapter;
    const rns_node_callbacks_t upper = {
        .tx = tx_cb,
        .rx = rx_cb,
        .proof = proof_cb,
        .request = request_cb,
        .ctx = &ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_radio_bind_node(&adapter,
                                                  &radio,
                                                  &node,
                                                  &upper));

    rns_transport_rx_result_t result;
    rns_packet_t proof;
    rns_packet_clear(&proof);
    proof.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    proof.packet_type = RNS_PACKET_TYPE_PROOF;
    memcpy(proof.destination_hash, node.destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    proof.context = RNS_PACKET_CONTEXT_LINKPROOF;
    proof.data[0] = 0x42;
    proof.data_len = 1;

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&node,
                                                      &proof,
                                                      &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.proof_count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_PROOF, ctx.last_proof.packet_type);
    TEST_ASSERT_EQUAL_UINT8(RNS_PACKET_CONTEXT_LINKPROOF,
                            ctx.last_proof.context);

    rns_packet_t request;
    rns_packet_clear(&request);
    request.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    request.packet_type = RNS_PACKET_TYPE_DATA;
    request.context = RNS_PACKET_CONTEXT_REQUEST;
    memcpy(request.destination_hash, node.destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    request.data[0] = 0x99;
    request.data_len = 1;

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&node,
                                                      &request,
                                                      &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.request_count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_DATA, ctx.last_request.packet_type);
    TEST_ASSERT_EQUAL_UINT8(RNS_PACKET_CONTEXT_REQUEST,
                            ctx.last_request.context);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.rx_count);
}
