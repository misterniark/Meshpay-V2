#include "meshpay/rns/rns_identity.h"
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

static esp_err_t deterministic_rng(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(private_key);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(private_key), len);
    memcpy(out, private_key, len);
    return ESP_OK;
}

TEST_CASE("rns identity loads private key and derives public material", "[rns_identity]")
{
    const uint8_t expected_public[RNS_IDENTITY_PUBLIC_SIZE] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
        0x70, 0x0e, 0x2c, 0xe7, 0xc4, 0xb6, 0x74, 0x42,
        0x7e, 0xab, 0x27, 0xba, 0x82, 0x0b, 0xcf, 0x6f,
        0x0f, 0xae, 0xbe, 0x68, 0xe0, 0x9f, 0xe8, 0x56,
        0x42, 0x92, 0x11, 0x4e, 0x41, 0xdc, 0x6a, 0x41,
    };
    const uint8_t expected_hash[RNS_IDENTITY_HASH_SIZE] = {
        0x37, 0xba, 0x56, 0x5d, 0xb3, 0x79, 0x14, 0xb0,
        0xf5, 0xbf, 0xdd, 0x17, 0xc4, 0x42, 0x0d, 0x6f,
    };

    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(private_key);

    rns_identity_t identity;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&identity, private_key));

    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t hash[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&identity, public_key));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&identity, hash));
    TEST_ASSERT_EQUAL_MEMORY(expected_public, public_key, sizeof(public_key));
    TEST_ASSERT_EQUAL_MEMORY(expected_hash, hash, sizeof(hash));

    uint8_t exported_private[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&identity, exported_private));
    TEST_ASSERT_EQUAL_MEMORY(private_key, exported_private, sizeof(private_key));
}

TEST_CASE("rns identity generated key uses injectable rng", "[rns_identity]")
{
    uint8_t expected_private[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(expected_private);

    rns_crypto_set_rng(deterministic_rng, NULL);
    rns_identity_t identity;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&identity));
    rns_crypto_set_rng(NULL, NULL);

    uint8_t exported_private[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&identity, exported_private));
    TEST_ASSERT_EQUAL_MEMORY(expected_private, exported_private, sizeof(exported_private));
}

TEST_CASE("rns identity signs and verifies with public-only identity", "[rns_identity]")
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(private_key);

    rns_identity_t signer;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&signer, private_key));

    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&signer, public_key));

    rns_identity_t verifier;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_public(&verifier, public_key));

    const uint8_t message[] = "announce me";
    uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_sign(&signer, message,
                                                sizeof(message) - 1, signature));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_verify(&verifier, message,
                                                  sizeof(message) - 1, signature));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_identity_sign(&verifier, message,
                                        sizeof(message) - 1, signature));
}

TEST_CASE("rns identity shared secret is symmetric", "[rns_identity]")
{
    uint8_t alice_private[RNS_IDENTITY_PRIVATE_SIZE];
    make_private_key(alice_private);

    uint8_t bob_private[RNS_IDENTITY_PRIVATE_SIZE];
    for (size_t i = 0; i < sizeof(bob_private); ++i) {
        bob_private[i] = (uint8_t)(0xa0 + i);
    }

    rns_identity_t alice;
    rns_identity_t bob;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&alice, alice_private));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&bob, bob_private));

    uint8_t alice_public[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t bob_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&alice, alice_public));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&bob, bob_public));

    uint8_t alice_shared[RNS_CRYPTO_X25519_SHARED_SIZE];
    uint8_t bob_shared[RNS_CRYPTO_X25519_SHARED_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_shared_secret(&alice, bob_public, alice_shared));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_shared_secret(&bob, alice_public, bob_shared));
    TEST_ASSERT_EQUAL_MEMORY(alice_shared, bob_shared, sizeof(alice_shared));
}

TEST_CASE("rns identity rejects invalid states and nulls", "[rns_identity]")
{
    rns_identity_t identity;
    rns_identity_clear(&identity);
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    uint8_t hash[RNS_IDENTITY_HASH_SIZE];
    make_private_key(private_key);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rns_identity_load_private(NULL, private_key));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rns_identity_load_public(&identity, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_identity_get_hash(&identity, hash));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_identity_get_private_key(&identity, private_key));

    uint8_t zero_private[RNS_IDENTITY_PRIVATE_SIZE] = {0};
    uint8_t zero_public[RNS_IDENTITY_PUBLIC_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_private(&identity, zero_private));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_public(&identity, zero_public));

    uint8_t half_zero_private[RNS_IDENTITY_PRIVATE_SIZE];
    memcpy(half_zero_private, private_key, sizeof(half_zero_private));
    memset(half_zero_private, 0, RNS_CRYPTO_X25519_KEY_SIZE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_private(&identity, half_zero_private));
    memcpy(half_zero_private, private_key, sizeof(half_zero_private));
    memset(half_zero_private + RNS_CRYPTO_X25519_KEY_SIZE, 0,
           RNS_CRYPTO_ED25519_SEED_SIZE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_private(&identity, half_zero_private));

    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&identity, private_key));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&identity, public_key));
    uint8_t half_zero_public[RNS_IDENTITY_PUBLIC_SIZE];
    memcpy(half_zero_public, public_key, sizeof(half_zero_public));
    memset(half_zero_public, 0, RNS_CRYPTO_X25519_KEY_SIZE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_public(&identity, half_zero_public));
    memcpy(half_zero_public, public_key, sizeof(half_zero_public));
    memset(half_zero_public + RNS_CRYPTO_X25519_KEY_SIZE, 0,
           RNS_CRYPTO_ED25519_PUBLIC_SIZE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_identity_load_public(&identity, half_zero_public));

    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&identity, private_key));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&identity, public_key));
    rns_identity_clear(&identity);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_identity_get_public_key(&identity, public_key));
}
