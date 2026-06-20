#include "meshpay/rns/rns_resource.h"
#include "unity.h"
#include <string.h>

static void make_active_link(rns_link_t *link)
{
    rns_link_clear(link);
    link->status = RNS_LINK_STATUS_ACTIVE;
    link->mtu = RNS_PACKET_MTU;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        link->link_id[i] = (uint8_t)(0x70 + i);
    }
}

static void fill_batch(uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        data[i] = (uint8_t)((i * 13u) ^ (i >> 2));
    }
}

TEST_CASE("rns resource transfers batch larger than mtu bit identically", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);

    uint8_t batch[900];
    fill_batch(batch, sizeof(batch));

    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(&link,
                                                          batch,
                                                          sizeof(batch),
                                                          packets,
                                                          RNS_RESOURCE_MAX_FRAGMENTS,
                                                          &packet_count));
    TEST_ASSERT_EQUAL_UINT32(3, packet_count);
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_TRUE(packets[i].data_len <= RNS_PACKET_MAX_DATA_SIZE);
        TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_RESOURCE, packets[i].context);
        TEST_ASSERT_EQUAL_MEMORY(link.link_id,
                                 packets[i].destination_hash,
                                 RNS_DESTINATION_HASH_SIZE);
    }

    rns_resource_reassembler_t reassembler;
    rns_resource_reassembler_init(&reassembler);
    uint8_t rebuilt[sizeof(batch)];
    size_t rebuilt_len = 0;
    bool complete = false;
    const size_t order[] = {2, 0, 1};
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_resource_reassembler_accept(&reassembler,
                                                          &packets[order[i]],
                                                          rebuilt,
                                                          sizeof(rebuilt),
                                                          &rebuilt_len,
                                                          &complete));
        if (i + 1 < packet_count) {
            TEST_ASSERT_FALSE(complete);
        }
    }

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(batch), rebuilt_len);
    TEST_ASSERT_EQUAL_MEMORY(batch, rebuilt, sizeof(batch));
}

TEST_CASE("rns resource rejects active link without link id", "[rns_resource]")
{
    rns_link_t link;
    rns_link_clear(&link);
    link.status = RNS_LINK_STATUS_ACTIVE;
    link.mtu = RNS_PACKET_MTU;
    link.mode = RNS_LINK_MODE_AES256_CBC;

    uint8_t batch[8];
    fill_batch(batch, sizeof(batch));
    rns_packet_t packet;
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_resource_create_packets(&link,
                                                  batch,
                                                  sizeof(batch),
                                                  &packet,
                                                  1,
                                                  &packet_count));
}

TEST_CASE("rns resource rejects corrupted checksum", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);

    uint8_t batch[600];
    fill_batch(batch, sizeof(batch));

    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(&link,
                                                          batch,
                                                          sizeof(batch),
                                                          packets,
                                                          RNS_RESOURCE_MAX_FRAGMENTS,
                                                          &packet_count));
    packets[packet_count - 1].data[packets[packet_count - 1].data_len - 1] ^= 0x01;

    rns_resource_reassembler_t reassembler;
    rns_resource_reassembler_init(&reassembler);
    uint8_t rebuilt[sizeof(batch)];
    size_t rebuilt_len = 0;
    bool complete = false;
    for (size_t i = 0; i < packet_count - 1; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_resource_reassembler_accept(&reassembler,
                                                          &packets[i],
                                                          rebuilt,
                                                          sizeof(rebuilt),
                                                          &rebuilt_len,
                                                          &complete));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_resource_reassembler_accept(&reassembler,
                                                      &packets[packet_count - 1],
                                                      rebuilt,
                                                      sizeof(rebuilt),
                                                      &rebuilt_len,
                                                      &complete));
}
