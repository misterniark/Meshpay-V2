#include "meshpay/rns/rns_packet.h"
#include "unity.h"
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

TEST_CASE("rns packet packs and unpacks header type 1 data packet", "[rns_packet]")
{
    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.propagation_type = RNS_PACKET_PROPAGATION_BROADCAST;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_DATA;
    packet.hops = 7;
    packet.context = 0xab;
    fill_sequence(packet.destination_hash, sizeof(packet.destination_hash), 0x10);
    memcpy(packet.data, "hi", 2);
    packet.data_len = 2;

    uint8_t wire[RNS_PACKET_MTU];
    size_t written = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_pack(&packet, wire, sizeof(wire), &written));
    TEST_ASSERT_EQUAL_UINT32(21, written);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[0]);
    TEST_ASSERT_EQUAL_HEX8(0x07, wire[1]);
    TEST_ASSERT_EQUAL_MEMORY(packet.destination_hash, wire + 2, RNS_PACKET_ADDRESS_SIZE);
    TEST_ASSERT_EQUAL_HEX8(0xab, wire[18]);
    TEST_ASSERT_EQUAL_MEMORY("hi", wire + 19, 2);

    rns_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_unpack(wire, written, &decoded));
    TEST_ASSERT_EQUAL(RNS_PACKET_HEADER_TYPE_1, decoded.header_type);
    TEST_ASSERT_FALSE(decoded.context_flag);
    TEST_ASSERT_EQUAL(RNS_PACKET_PROPAGATION_BROADCAST, decoded.propagation_type);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_SINGLE, decoded.destination_type);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_DATA, decoded.packet_type);
    TEST_ASSERT_EQUAL_UINT8(7, decoded.hops);
    TEST_ASSERT_EQUAL_UINT8(0xab, decoded.context);
    TEST_ASSERT_EQUAL_UINT32(2, decoded.data_len);
    TEST_ASSERT_EQUAL_MEMORY(packet.destination_hash, decoded.destination_hash,
                             RNS_PACKET_ADDRESS_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(packet.data, decoded.data, packet.data_len);
}

TEST_CASE("rns packet packs and unpacks header type 2 transport packet", "[rns_packet]")
{
    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_2;
    packet.context_flag = true;
    packet.propagation_type = RNS_PACKET_PROPAGATION_TRANSPORT;
    packet.destination_type = RNS_DESTINATION_TYPE_GROUP;
    packet.packet_type = RNS_PACKET_TYPE_PROOF;
    packet.hops = 4;
    packet.context = RNS_PACKET_CONTEXT_NONE;
    fill_sequence(packet.destination_hash, sizeof(packet.destination_hash), 0x20);
    fill_sequence(packet.transport_id, sizeof(packet.transport_id), 0x40);

    uint8_t wire[RNS_PACKET_MTU];
    size_t written = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_pack(&packet, wire, sizeof(wire), &written));
    TEST_ASSERT_EQUAL_UINT32(35, written);
    TEST_ASSERT_EQUAL_HEX8(0x77, wire[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, wire[1]);
    TEST_ASSERT_EQUAL_MEMORY(packet.transport_id, wire + 2, RNS_PACKET_ADDRESS_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(packet.destination_hash, wire + 18, RNS_PACKET_ADDRESS_SIZE);

    rns_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_unpack(wire, written, &decoded));
    TEST_ASSERT_EQUAL(RNS_PACKET_HEADER_TYPE_2, decoded.header_type);
    TEST_ASSERT_TRUE(decoded.context_flag);
    TEST_ASSERT_EQUAL(RNS_PACKET_PROPAGATION_TRANSPORT, decoded.propagation_type);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_GROUP, decoded.destination_type);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_PROOF, decoded.packet_type);
    TEST_ASSERT_EQUAL_UINT8(4, decoded.hops);
    TEST_ASSERT_EQUAL_MEMORY(packet.destination_hash, decoded.destination_hash,
                             RNS_PACKET_ADDRESS_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(packet.transport_id, decoded.transport_id,
                             RNS_PACKET_ADDRESS_SIZE);
}

TEST_CASE("rns packet enforces mtu data and ifac limits", "[rns_packet]")
{
    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.data_len = RNS_PACKET_MAX_DATA_SIZE + 1;

    uint8_t wire[RNS_PACKET_MTU];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rns_packet_pack(&packet, wire,
                                                            sizeof(wire), NULL));

    uint8_t ifac_wire[19] = {0};
    ifac_wire[0] = RNS_PACKET_HEADER_IFAC_MASK;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, rns_packet_unpack(ifac_wire,
                                                               sizeof(ifac_wire),
                                                               &packet));
}

TEST_CASE("rns packet context constants match Reticulum", "[rns_packet]")
{
    TEST_ASSERT_EQUAL_UINT32(500, RNS_PACKET_MTU);
    TEST_ASSERT_EQUAL_UINT32(464, RNS_PACKET_MDU);
    TEST_ASSERT_EQUAL_UINT32(RNS_PACKET_MDU, RNS_PACKET_MAX_DATA_SIZE);
    TEST_ASSERT_EQUAL_HEX8(0x01, RNS_PACKET_CONTEXT_RESOURCE);
    TEST_ASSERT_EQUAL_HEX8(0x06, RNS_PACKET_CONTEXT_RESOURCE_ICL);
    TEST_ASSERT_EQUAL_HEX8(0x07, RNS_PACKET_CONTEXT_RESOURCE_RCL);
    TEST_ASSERT_EQUAL_HEX8(0x08, RNS_PACKET_CONTEXT_CACHE_REQUEST);
    TEST_ASSERT_EQUAL_HEX8(0x09, RNS_PACKET_CONTEXT_REQUEST);
    TEST_ASSERT_EQUAL_HEX8(0x0a, RNS_PACKET_CONTEXT_RESPONSE);
    TEST_ASSERT_EQUAL_HEX8(0xfd, RNS_PACKET_CONTEXT_LINKPROOF);
    TEST_ASSERT_EQUAL_HEX8(0xfe, RNS_PACKET_CONTEXT_LRRTT);
    TEST_ASSERT_EQUAL_HEX8(0xff, RNS_PACKET_CONTEXT_LRPROOF);
}

TEST_CASE("rns packet hop increment handles overflow", "[rns_packet]")
{
    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.hops = 41;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_increment_hops(&packet));
    TEST_ASSERT_EQUAL_UINT8(42, packet.hops);
    packet.hops = UINT8_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_packet_increment_hops(&packet));
}

TEST_CASE("rns packet hash ignores hop count like Reticulum", "[rns_packet]")
{
    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_DATA;
    fill_sequence(packet.destination_hash, sizeof(packet.destination_hash), 0x10);
    memcpy(packet.data, "hash me", 7);
    packet.data_len = 7;

    uint8_t hash_a[RNS_CRYPTO_SHA256_SIZE];
    uint8_t hash_b[RNS_CRYPTO_SHA256_SIZE];
    packet.hops = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_hash(&packet, hash_a));
    packet.hops = 9;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_hash(&packet, hash_b));
    TEST_ASSERT_EQUAL_MEMORY(hash_a, hash_b, sizeof(hash_a));

    packet.data[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_hash(&packet, hash_b));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(hash_a, hash_b, sizeof(hash_a)));
}
