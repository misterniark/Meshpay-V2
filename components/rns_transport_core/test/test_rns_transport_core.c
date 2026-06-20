#include "meshpay/rns/rns_transport_core.h"
#include "unity.h"
#include <string.h>

typedef struct {
    size_t local_count;
    size_t forward_count;
    rns_packet_t last_local;
    rns_packet_t last_forward;
} transport_test_ctx_t;

static esp_err_t local_cb(const rns_packet_t *packet, void *ctx)
{
    transport_test_ctx_t *test_ctx = (transport_test_ctx_t *)ctx;
    test_ctx->local_count++;
    test_ctx->last_local = *packet;
    return ESP_OK;
}

static esp_err_t forward_cb(const rns_packet_t *packet, void *ctx)
{
    transport_test_ctx_t *test_ctx = (transport_test_ctx_t *)ctx;
    test_ctx->forward_count++;
    test_ctx->last_forward = *packet;
    return ESP_OK;
}

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void load_fixture_identity(rns_identity_t *identity)
{
    const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

TEST_CASE("rns transport core drops duplicate packets", "[rns_transport_core]")
{
    rns_transport_core_t core;
    rns_transport_core_init(&core);

    transport_test_ctx_t ctx = {0};
    const rns_transport_callbacks_t callbacks = {
        .local_rx = local_cb,
        .forward_tx = forward_cb,
        .ctx = &ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_set_callbacks(&core, &callbacks));

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_DATA;
    packet.hops = 2;
    fill_sequence(packet.destination_hash, sizeof(packet.destination_hash), 0x10);
    memcpy(packet.data, "hello", 5);
    packet.data_len = 5;

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_receive(&core, &packet, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_FORWARDED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.forward_count);
    TEST_ASSERT_EQUAL_UINT8(3, ctx.last_forward.hops);

    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_receive(&core, &packet, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_DUPLICATE_DROP, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.forward_count);
}

TEST_CASE("rns transport core delivers local destination without forwarding", "[rns_transport_core]")
{
    rns_transport_core_t core;
    rns_transport_core_init(&core);

    transport_test_ctx_t ctx = {0};
    const rns_transport_callbacks_t callbacks = {
        .local_rx = local_cb,
        .forward_tx = forward_cb,
        .ctx = &ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_set_callbacks(&core, &callbacks));

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_DATA;
    fill_sequence(packet.destination_hash, sizeof(packet.destination_hash), 0x40);
    memcpy(packet.data, "local", 5);
    packet.data_len = 5;

    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_add_local_destination(&core,
                                                                       packet.destination_hash));

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_receive(&core, &packet, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_LOCAL_DELIVERED, result);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.local_count);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.forward_count);
    TEST_ASSERT_EQUAL_MEMORY(packet.destination_hash,
                             ctx.last_local.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);
}

TEST_CASE("rns transport core rejects zero local destination", "[rns_transport_core]")
{
    rns_transport_core_t core;
    rns_transport_core_init(&core);

    uint8_t zero_destination[RNS_DESTINATION_HASH_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_transport_core_add_local_destination(
                          &core,
                          zero_destination));
    TEST_ASSERT_EQUAL_UINT32(0, core.local_destination_count);
}

TEST_CASE("rns transport core updates path from valid announce", "[rns_transport_core]")
{
    rns_transport_core_t core;
    rns_transport_core_init(&core);
    rns_announce_known_reset();

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&identity, &destination));

    const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
        0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    };
    const uint8_t app_data[] = "Alice";

    rns_packet_t announce;
    rns_packet_clear(&announce);
    announce.header_type = RNS_PACKET_HEADER_TYPE_2;
    announce.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    announce.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    announce.hops = 4;
    fill_sequence(announce.transport_id, sizeof(announce.transport_id), 0x90);
    memcpy(announce.destination_hash, destination.hash, RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_encode(&destination,
                                                  &identity,
                                                  random_hash,
                                                  app_data,
                                                  sizeof(app_data) - 1,
                                                  announce.data,
                                                  sizeof(announce.data),
                                                  &announce.data_len));

    rns_transport_rx_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, rns_transport_core_receive(&core, &announce, &result));
    TEST_ASSERT_EQUAL(RNS_TRANSPORT_RX_ACCEPTED, result);

    const rns_transport_path_t *path =
        rns_transport_core_find_path(&core, destination.hash);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_UINT8(4, path->hops);
    TEST_ASSERT_EQUAL_MEMORY(announce.transport_id,
                             path->via_transport_id,
                             RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_UINT32(1, rns_announce_known_count());
}
