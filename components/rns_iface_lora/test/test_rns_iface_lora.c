#include "meshpay/rns/rns_iface_lora.h"
#include "unity.h"
#include <string.h>

typedef struct {
    size_t ready_attempts;
    size_t ready_failures_before_ok;
    size_t tx_count;
    size_t last_tx_len;
} lora_test_ctx_t;

static esp_err_t wait_ready_timeout(void *ctx, uint32_t timeout_ms)
{
    (void)timeout_ms;
    lora_test_ctx_t *test_ctx = (lora_test_ctx_t *)ctx;
    test_ctx->ready_attempts++;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_ready_after_failures(void *ctx, uint32_t timeout_ms)
{
    (void)timeout_ms;
    lora_test_ctx_t *test_ctx = (lora_test_ctx_t *)ctx;
    test_ctx->ready_attempts++;
    if (test_ctx->ready_attempts <= test_ctx->ready_failures_before_ok) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t tx_counting(void *ctx, const uint8_t *frame, size_t frame_len)
{
    (void)frame;
    lora_test_ctx_t *test_ctx = (lora_test_ctx_t *)ctx;
    test_ctx->tx_count++;
    test_ctx->last_tx_len = frame_len;
    return ESP_OK;
}

static void fill_packet(uint8_t *packet, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        packet[i] = (uint8_t)(0xa5 ^ i);
    }
}

TEST_CASE("rns lora fragments and reassembles 255 byte frames", "[rns_iface_lora]")
{
    uint8_t packet[RNS_PACKET_MTU];
    fill_packet(packet, sizeof(packet));

    rns_lora_fragment_t fragments[RNS_LORA_MAX_FRAGMENTS];
    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_lora_fragment_packet(packet,
                                                             sizeof(packet),
                                                             RNS_LORA_MAX_FRAME_SIZE,
                                                             fragments,
                                                             RNS_LORA_MAX_FRAGMENTS,
                                                             &fragment_count));
    TEST_ASSERT_EQUAL_UINT32(3, fragment_count);
    TEST_ASSERT_EQUAL_UINT32(232, fragments[0].payload_len);
    TEST_ASSERT_EQUAL_UINT32(36, fragments[2].payload_len);

    uint8_t frames[RNS_LORA_MAX_FRAGMENTS][RNS_LORA_MAX_FRAME_SIZE];
    size_t frame_lens[RNS_LORA_MAX_FRAGMENTS];
    for (size_t i = 0; i < fragment_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, rns_iface_lora_pack_fragment(&fragments[i],
                                                               frames[i],
                                                               sizeof(frames[i]),
                                                               &frame_lens[i]));
        TEST_ASSERT_TRUE(frame_lens[i] <= RNS_LORA_MAX_FRAME_SIZE);
    }

    rns_lora_reassembler_t reassembler;
    rns_iface_lora_reassembler_init(&reassembler);
    uint8_t rebuilt[RNS_PACKET_MTU];
    size_t rebuilt_len = 0;
    bool complete = false;
    const size_t order[] = {2, 0, 1};
    for (size_t i = 0; i < fragment_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_iface_lora_reassembler_accept(&reassembler,
                                                            frames[order[i]],
                                                            frame_lens[order[i]],
                                                            rebuilt,
                                                            sizeof(rebuilt),
                                                            &rebuilt_len,
                                                            &complete));
    }

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(packet), rebuilt_len);
    TEST_ASSERT_EQUAL_MEMORY(packet, rebuilt, sizeof(packet));
}

TEST_CASE("rns lora init retries and reports timeout", "[rns_iface_lora]")
{
    lora_test_ctx_t ctx = {0};
    rns_lora_iface_t iface;
    const rns_lora_config_t config = {
        .wait_ready = wait_ready_timeout,
        .tx = tx_counting,
        .ctx = &ctx,
        .init_timeout_ms = 10,
        .init_retries = 2,
    };

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, rns_iface_lora_init(&iface, &config));
    TEST_ASSERT_EQUAL_UINT32(3, ctx.ready_attempts);
    TEST_ASSERT_FALSE(iface.initialized);
}

TEST_CASE("rns lora init succeeds after retry and sends half duplex frame", "[rns_iface_lora]")
{
    lora_test_ctx_t ctx = {
        .ready_failures_before_ok = 1,
    };
    rns_lora_iface_t iface;
    const rns_lora_config_t config = {
        .wait_ready = wait_ready_after_failures,
        .tx = tx_counting,
        .ctx = &ctx,
        .init_timeout_ms = 10,
        .init_retries = 2,
    };

    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_lora_init(&iface, &config));
    TEST_ASSERT_TRUE(iface.initialized);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.ready_attempts);

    const uint8_t frame[] = {0x4d, 0x4c, 0x01};
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_lora_send_frame(&iface, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.tx_count);
    TEST_ASSERT_EQUAL_UINT32(sizeof(frame), ctx.last_tx_len);
}
