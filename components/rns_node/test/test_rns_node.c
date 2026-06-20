#include "meshpay/rns/rns_node.h"
#include "unity.h"

#include <string.h>

typedef struct {
    size_t tx_count;
    size_t rx_count;
    size_t proof_count;
    size_t request_count;
    rns_packet_t last_tx;
    rns_packet_t last_rx;
    uint8_t last_wire[RNS_PACKET_MTU];
    size_t last_wire_len;
} node_test_ctx_t;

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static esp_err_t deterministic_rng(void *ctx, uint8_t *out, size_t len)
{
    uint8_t *counter = (uint8_t *)ctx;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(*counter + i);
    }
    *counter = (uint8_t)(*counter + len);
    return ESP_OK;
}

static void load_identity(rns_identity_t *identity, uint8_t seed_base)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(private_key, sizeof(private_key), seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

static esp_err_t tx_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    node_test_ctx_t *test_ctx = (node_test_ctx_t *)ctx;
    test_ctx->tx_count++;
    test_ctx->last_tx = *packet;
    return rns_packet_pack(packet,
                           test_ctx->last_wire,
                           sizeof(test_ctx->last_wire),
                           &test_ctx->last_wire_len);
}

static esp_err_t rx_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    node_test_ctx_t *test_ctx = (node_test_ctx_t *)ctx;
    test_ctx->rx_count++;
    test_ctx->last_rx = *packet;
    return ESP_OK;
}

static esp_err_t proof_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    (void)packet;
    node_test_ctx_t *test_ctx = (node_test_ctx_t *)ctx;
    test_ctx->proof_count++;
    return ESP_OK;
}

static esp_err_t request_cb(rns_node_t *node, const rns_packet_t *packet, void *ctx)
{
    (void)node;
    (void)packet;
    node_test_ctx_t *test_ctx = (node_test_ctx_t *)ctx;
    test_ctx->request_count++;
    return ESP_OK;
}

static void init_node(rns_node_t *node,
                      node_test_ctx_t *ctx,
                      uint8_t seed_base)
{
    rns_identity_t identity;
    load_identity(&identity, seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_init(node, &identity));

    const rns_node_callbacks_t callbacks = {
        .tx = tx_cb,
        .rx = rx_cb,
        .proof = proof_cb,
        .request = request_cb,
        .ctx = ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_set_callbacks(node, &callbacks));
}

TEST_CASE("rns node announces and peer learns path", "[rns_node]")
{
    uint8_t rng_counter = 0xa0;
    rns_crypto_set_rng(deterministic_rng, &rng_counter);
    rns_announce_known_reset();

    rns_node_t alice;
    rns_node_t bob;
    node_test_ctx_t alice_ctx = {0};
    node_test_ctx_t bob_ctx = {0};
    init_node(&alice, &alice_ctx, 0x01);
    init_node(&bob, &bob_ctx, 0x41);

    const uint8_t app_data[] = "Alice";
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_announce(&alice,
                                                app_data,
                                                sizeof(app_data) - 1));
    TEST_ASSERT_EQUAL_UINT32(1, alice_ctx.tx_count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_ANNOUNCE, alice_ctx.last_tx.packet_type);

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_poll(&bob,
                                            alice_ctx.last_wire,
                                            alice_ctx.last_wire_len,
                                            &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_FORWARDED, result);

    const rns_transport_path_t *path =
        rns_transport_core_find_path(&bob.transport, alice.destination.hash);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_UINT32(1, rns_announce_known_count());
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.tx_count);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.rx_count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_ANNOUNCE, bob_ctx.last_rx.packet_type);
    TEST_ASSERT_EQUAL_UINT32(1, bob.stats.forwarded_packets);

    rns_crypto_set_rng(NULL, NULL);
}

TEST_CASE("rns node sends data and drops duplicate poll", "[rns_node]")
{
    rns_node_t alice;
    rns_node_t bob;
    node_test_ctx_t alice_ctx = {0};
    node_test_ctx_t bob_ctx = {0};
    init_node(&alice, &alice_ctx, 0x11);
    init_node(&bob, &bob_ctx, 0x51);

    const uint8_t payload[] = "meshpay data";
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_send(&alice,
                                            bob.destination.hash,
                                            payload,
                                            sizeof(payload) - 1));
    TEST_ASSERT_EQUAL_UINT32(1, alice_ctx.tx_count);

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_poll(&bob,
                                            alice_ctx.last_wire,
                                            alice_ctx.last_wire_len,
                                            &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.rx_count);
    TEST_ASSERT_EQUAL_MEMORY(payload, bob_ctx.last_rx.data,
                             sizeof(payload) - 1);

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_poll(&bob,
                                            alice_ctx.last_wire,
                                            alice_ctx.last_wire_len,
                                            &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_DUPLICATE_DROP, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.rx_count);
    TEST_ASSERT_EQUAL_UINT32(1, bob.stats.duplicate_drops);
}

TEST_CASE("rns node forwards and locally dispatches plain broadcast data",
          "[rns_node]")
{
    rns_node_t bob;
    node_test_ctx_t bob_ctx = {0};
    init_node(&bob, &bob_ctx, 0x21);

    rns_packet_t summary;
    rns_packet_clear(&summary);
    summary.destination_type = RNS_DESTINATION_TYPE_PLAIN;
    summary.packet_type = RNS_PACKET_TYPE_DATA;
    summary.propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    fill_sequence(summary.destination_hash,
                  sizeof(summary.destination_hash),
                  0xa0);
    memcpy(summary.data, "sum", 3);
    summary.data_len = 3;

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&bob,
                                                      &summary,
                                                      &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_FORWARDED, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.tx_count);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.rx_count);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_PLAIN,
                      bob_ctx.last_rx.destination_type);
    TEST_ASSERT_EQUAL_MEMORY(summary.data, bob_ctx.last_rx.data, 3);

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&bob,
                                                      &summary,
                                                      &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_DUPLICATE_DROP, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.tx_count);
    TEST_ASSERT_EQUAL_UINT32(2, bob_ctx.rx_count);
}

TEST_CASE("rns node dispatches proof and request callbacks", "[rns_node]")
{
    rns_node_t bob;
    node_test_ctx_t bob_ctx = {0};
    init_node(&bob, &bob_ctx, 0x61);

    rns_packet_t proof;
    rns_packet_clear(&proof);
    proof.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    proof.packet_type = RNS_PACKET_TYPE_PROOF;
    memcpy(proof.destination_hash, bob.destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    proof.data[0] = 0x42;
    proof.data_len = 1;

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&bob, &proof, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.proof_count);

    rns_packet_t request;
    rns_packet_clear(&request);
    request.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    request.packet_type = RNS_PACKET_TYPE_DATA;
    request.context = RNS_PACKET_CONTEXT_REQUEST;
    memcpy(request.destination_hash, bob.destination.hash,
           RNS_DESTINATION_HASH_SIZE);
    request.data[0] = 0x99;
    request.data_len = 1;

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&bob, &request, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, bob_ctx.request_count);

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_receive_packet(&bob, &request, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_DUPLICATE_DROP, result);
    TEST_ASSERT_EQUAL_UINT32(2, bob_ctx.request_count);

    const rns_node_stats_t *stats = rns_node_stats(&bob);
    TEST_ASSERT_NOT_NULL(stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats->proof_packets);
    TEST_ASSERT_EQUAL_UINT32(2, stats->request_packets);
}
