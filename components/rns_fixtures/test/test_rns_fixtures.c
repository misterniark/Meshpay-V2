#include "meshpay/rns/rns_fixtures.h"
#include "unity.h"

TEST_CASE("rns fixtures expose schema and canary vector", "[rns_fixtures]")
{
    TEST_ASSERT_EQUAL_STRING("meshpay-rns-fixtures-v1", rns_fixtures_schema());
    TEST_ASSERT_EQUAL_STRING("local-port-c-fixtures", rns_fixtures_source());
    TEST_ASSERT_EQUAL_UINT32(6, rns_fixtures_count());

    const rns_fixture_t *fixture = rns_fixtures_get(0);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_STRING("schema-canary-v1", fixture->name);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_SCHEMA_CANARY, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(8, fixture->len);
    TEST_ASSERT_EQUAL_HEX8(0x4d, fixture->bytes[0]);
    fixture = rns_fixtures_get(1);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_STRING("meshpay-wallet-name-hash", fixture->name);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_DESTINATION_NAME_HASH, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(10, fixture->len);
    TEST_ASSERT_EQUAL_HEX8(0x04, fixture->bytes[0]);

    fixture = rns_fixtures_get(2);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_STRING("meshpay-wallet-destination-hash", fixture->name);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_DESTINATION_HASH, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(16, fixture->len);
    TEST_ASSERT_EQUAL_HEX8(0x8b, fixture->bytes[0]);

    fixture = rns_fixtures_get(3);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_STRING("data-packet-type1-raw", fixture->name);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_PACKET_RAW, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(21, fixture->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, fixture->bytes[0]);
    TEST_ASSERT_EQUAL_HEX8(0x07, fixture->bytes[1]);

    fixture = rns_fixtures_get(4);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_ANNOUNCE_RAW, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(153, fixture->len);

    fixture = rns_fixtures_get(5);
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL(RNS_FIXTURE_KIND_ENCRYPTED_TOKEN, fixture->kind);
    TEST_ASSERT_EQUAL_UINT32(128, fixture->len);

    TEST_ASSERT_NULL(rns_fixtures_get(6));
}
