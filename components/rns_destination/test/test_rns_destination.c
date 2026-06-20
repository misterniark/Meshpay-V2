#include "meshpay/rns/rns_destination.h"
#include "unity.h"
#include <string.h>

static void make_private_key(uint8_t out[RNS_IDENTITY_PRIVATE_SIZE])
{
    const uint8_t x25519_private[RNS_CRYPTO_X25519_KEY_SIZE] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
    };
    const uint8_t ed25519_seed[RNS_CRYPTO_ED25519_SEED_SIZE] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55,
    };
    memcpy(out, x25519_private, sizeof(x25519_private));
    memcpy(out + sizeof(x25519_private), ed25519_seed, sizeof(ed25519_seed));
}

static void make_identity(rns_identity_t *identity)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(private_key);
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

TEST_CASE("rns destination builds full dotted names", "[rns_destination]")
{
    const char *aspects[] = {"wallet", "pay"};
    char full_name[RNS_DESTINATION_MAX_FULL_NAME];

    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_build_full_name("meshpay",
                                                              aspects, 2,
                                                              full_name,
                                                              sizeof(full_name)));
    TEST_ASSERT_EQUAL_STRING("meshpay.wallet.pay", full_name);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_destination_build_full_name("meshpay.wallet",
                                                      aspects, 1,
                                                      full_name,
                                                      sizeof(full_name)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_destination_build_full_name("meshpay",
                                                      NULL, 1,
                                                      full_name,
                                                      sizeof(full_name)));
}

TEST_CASE("rns destination computes meshpay wallet name hash", "[rns_destination]")
{
    const char *aspects[] = {RNS_MESHPAY_WALLET_ASPECT};
    const uint8_t expected[RNS_DESTINATION_NAME_HASH_SIZE] = {
        0x04, 0x9d, 0x90, 0x46, 0xc7, 0x4d, 0xe4, 0x6b, 0x50, 0xa3,
    };
    uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE];

    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_name_hash(RNS_MESHPAY_APP_NAME,
                                                        aspects, 1,
                                                        name_hash));
    TEST_ASSERT_EQUAL_MEMORY(expected, name_hash, sizeof(name_hash));
}

TEST_CASE("rns destination creates meshpay wallet single destination", "[rns_destination]")
{
    const uint8_t expected_hash[RNS_DESTINATION_HASH_SIZE] = {
        0x8b, 0x61, 0xde, 0x20, 0x6a, 0x0e, 0xbc, 0xae,
        0x65, 0x42, 0x58, 0x04, 0x79, 0xd8, 0x43, 0xc4,
    };
    const uint8_t expected_name_hash[RNS_DESTINATION_NAME_HASH_SIZE] = {
        0x04, 0x9d, 0x90, 0x46, 0xc7, 0x4d, 0xe4, 0x6b, 0x50, 0xa3,
    };

    rns_identity_t identity;
    make_identity(&identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&identity,
                                                                    &destination));
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_SINGLE, destination.type);
    TEST_ASSERT_EQUAL_STRING(RNS_MESHPAY_WALLET_FULL_NAME, destination.full_name);
    TEST_ASSERT_EQUAL_MEMORY(expected_name_hash, destination.name_hash,
                             sizeof(destination.name_hash));
    TEST_ASSERT_EQUAL_MEMORY(expected_hash, destination.hash, sizeof(destination.hash));
}

TEST_CASE("rns destination creates plain and link destinations", "[rns_destination]")
{
    const char *aspects[] = {RNS_MESHPAY_WALLET_ASPECT};
    const uint8_t expected_plain[RNS_DESTINATION_HASH_SIZE] = {
        0x53, 0xe3, 0x95, 0x99, 0x5a, 0xac, 0x95, 0x9a,
        0xc4, 0xab, 0x78, 0x7b, 0xe5, 0x2a, 0x9d, 0xea,
    };
    const uint8_t link_hash[RNS_DESTINATION_HASH_SIZE] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };

    rns_destination_t plain;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_plain(RNS_MESHPAY_APP_NAME,
                                                           aspects, 1,
                                                           &plain));
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_PLAIN, plain.type);
    TEST_ASSERT_EQUAL_MEMORY(expected_plain, plain.hash, sizeof(plain.hash));

    rns_destination_t link;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_link(link_hash, &link));
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_LINK, link.type);
    TEST_ASSERT_EQUAL_MEMORY(link_hash, link.hash, sizeof(link.hash));
    TEST_ASSERT_TRUE(rns_destination_hash_equal(link_hash, link.hash));
    link.hash[0] ^= 0x01;
    TEST_ASSERT_FALSE(rns_destination_hash_equal(link_hash, link.hash));
}

