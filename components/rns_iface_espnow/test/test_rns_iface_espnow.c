#include "meshpay/rns/rns_iface_espnow.h"
#include "unity.h"
#include <string.h>

static void fill_packet(uint8_t *packet, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        packet[i] = (uint8_t)(i ^ (i >> 3));
    }
}

TEST_CASE("rns espnow fragments and reassembles packet out of order", "[rns_iface_espnow]")
{
    uint8_t packet[RNS_PACKET_MTU];
    fill_packet(packet, sizeof(packet));

    rns_espnow_fragment_t fragments[RNS_ESPNOW_MAX_FRAGMENTS];
    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_fragment_packet(packet,
                                                               sizeof(packet),
                                                               120,
                                                               fragments,
                                                               RNS_ESPNOW_MAX_FRAGMENTS,
                                                               &fragment_count));
    TEST_ASSERT_EQUAL_UINT32(6, fragment_count);
    TEST_ASSERT_EQUAL_UINT32(97, fragments[0].payload_len);
    TEST_ASSERT_EQUAL_UINT32(15, fragments[5].payload_len);

    uint8_t frames[RNS_ESPNOW_MAX_FRAGMENTS][RNS_ESPNOW_MAX_FRAME_SIZE];
    size_t frame_lens[RNS_ESPNOW_MAX_FRAGMENTS];
    for (size_t i = 0; i < fragment_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_pack_fragment(&fragments[i],
                                                                 frames[i],
                                                                 sizeof(frames[i]),
                                                                 &frame_lens[i]));
        TEST_ASSERT_TRUE(frame_lens[i] <= 120);
    }

    rns_espnow_reassembler_t reassembler;
    rns_iface_espnow_reassembler_init(&reassembler);

    const size_t order[] = {5, 1, 3, 0, 2, 4};
    uint8_t rebuilt[RNS_PACKET_MTU];
    size_t rebuilt_len = 0;
    bool complete = false;
    for (size_t i = 0; i < fragment_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_iface_espnow_reassembler_accept(&reassembler,
                                                              frames[order[i]],
                                                              frame_lens[order[i]],
                                                              rebuilt,
                                                              sizeof(rebuilt),
                                                              &rebuilt_len,
                                                              &complete));
        if (i + 1 < fragment_count) {
            TEST_ASSERT_FALSE(complete);
        }
    }

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(packet), rebuilt_len);
    TEST_ASSERT_EQUAL_MEMORY(packet, rebuilt, sizeof(packet));
}

TEST_CASE("rns espnow default frame stays within esp-now v1 payload cap",
          "[rns_iface_espnow]")
{
    TEST_ASSERT_LESS_THAN_UINT32(250, RNS_ESPNOW_DEFAULT_FRAME_SIZE);
    TEST_ASSERT_LESS_THAN_UINT32(250, RNS_ESPNOW_MAX_FRAME_SIZE);

    uint8_t packet[RNS_PACKET_MTU];
    fill_packet(packet, sizeof(packet));

    rns_espnow_fragment_t fragments[RNS_ESPNOW_MAX_FRAGMENTS];
    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_fragment_packet(
                                  packet,
                                  sizeof(packet),
                                  RNS_ESPNOW_DEFAULT_FRAME_SIZE,
                                  fragments,
                                  RNS_ESPNOW_MAX_FRAGMENTS,
                                  &fragment_count));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3, fragment_count);

    for (size_t i = 0; i < fragment_count; ++i) {
        uint8_t frame[RNS_ESPNOW_MAX_FRAME_SIZE];
        size_t frame_len = 0;
        TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_pack_fragment(&fragments[i],
                                                                 frame,
                                                                 sizeof(frame),
                                                                 &frame_len));
        TEST_ASSERT_LESS_THAN_UINT32(250, frame_len);
    }
}

TEST_CASE("rns espnow rejects invalid frame sizes and conflicting duplicates", "[rns_iface_espnow]")
{
    uint8_t packet[64];
    fill_packet(packet, sizeof(packet));

    rns_espnow_fragment_t fragments[2];
    size_t fragment_count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      rns_iface_espnow_fragment_packet(packet,
                                                       sizeof(packet),
                                                       RNS_ESPNOW_FRAGMENT_HEADER_SIZE,
                                                       fragments,
                                                       2,
                                                       &fragment_count));

    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_fragment_packet(packet,
                                                               sizeof(packet),
                                                               80,
                                                               fragments,
                                                               2,
                                                               &fragment_count));
    TEST_ASSERT_EQUAL_UINT32(2, fragment_count);

    uint8_t frame[RNS_ESPNOW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_pack_fragment(&fragments[0],
                                                             frame,
                                                             sizeof(frame),
                                                             &frame_len));

    rns_espnow_reassembler_t reassembler;
    rns_iface_espnow_reassembler_init(&reassembler);
    uint8_t rebuilt[RNS_PACKET_MTU];
    size_t rebuilt_len = 0;
    bool complete = false;
    TEST_ASSERT_EQUAL(ESP_OK, rns_iface_espnow_reassembler_accept(&reassembler,
                                                                  frame,
                                                                  frame_len,
                                                                  rebuilt,
                                                                  sizeof(rebuilt),
                                                                  &rebuilt_len,
                                                                  &complete));
    frame[frame_len - 1] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_iface_espnow_reassembler_accept(&reassembler,
                                                          frame,
                                                          frame_len,
                                                          rebuilt,
                                                          sizeof(rebuilt),
                                                          &rebuilt_len,
                                                          &complete));
}
