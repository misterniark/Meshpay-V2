#include "meshpay/rns/rns_crypto.h"
#include "unity.h"
#include <string.h>

static void assert_hex(const uint8_t *expected, const uint8_t *actual, size_t len)
{
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, len);
}

TEST_CASE("rns crypto profile is explicit", "[rns_crypto]")
{
    TEST_ASSERT_EQUAL_UINT32(1, RNS_CRYPTO_SIGNATURE_WIRE_VERSION);
    TEST_ASSERT_EQUAL_STRING("meshpay-reticulum-monocypher-4.0.2-ed25519",
                             RNS_CRYPTO_SIGNATURE_SCHEME);
    TEST_ASSERT_EQUAL_STRING("Monocypher 4.0.2 crypto_ed25519_*",
                             RNS_CRYPTO_SIGNATURE_PROVIDER);
}

TEST_CASE("rns crypto hashes known values", "[rns_crypto]")
{
    const uint8_t msg[] = "hello";
    const uint8_t expected_sha256[RNS_CRYPTO_SHA256_SIZE] = {
        0x2c, 0xf2, 0x4d, 0xba, 0x5f, 0xb0, 0xa3, 0x0e,
        0x26, 0xe8, 0x3b, 0x2a, 0xc5, 0xb9, 0xe2, 0x9e,
        0x1b, 0x16, 0x1e, 0x5c, 0x1f, 0xa7, 0x42, 0x5e,
        0x73, 0x04, 0x33, 0x62, 0x93, 0x8b, 0x98, 0x24,
    };
    const uint8_t expected_sha512[RNS_CRYPTO_SHA512_SIZE] = {
        0x9b, 0x71, 0xd2, 0x24, 0xbd, 0x62, 0xf3, 0x78,
        0x5d, 0x96, 0xd4, 0x6a, 0xd3, 0xea, 0x3d, 0x73,
        0x31, 0x9b, 0xfb, 0xc2, 0x89, 0x0c, 0xaa, 0xda,
        0xe2, 0xdf, 0xf7, 0x25, 0x19, 0x67, 0x3c, 0xa7,
        0x23, 0x23, 0xc3, 0xd9, 0x9b, 0xa5, 0xc1, 0x1d,
        0x7c, 0x7a, 0xcc, 0x6e, 0x14, 0xb8, 0xc5, 0xda,
        0x0c, 0x46, 0x63, 0x47, 0x5c, 0x2e, 0x5c, 0x3a,
        0xde, 0xf4, 0x6f, 0x73, 0xbc, 0xde, 0xc0, 0x43,
    };

    uint8_t out256[RNS_CRYPTO_SHA256_SIZE];
    uint8_t out512[RNS_CRYPTO_SHA512_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_sha256(msg, sizeof(msg) - 1, out256));
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_sha512(msg, sizeof(msg) - 1, out512));
    assert_hex(expected_sha256, out256, sizeof(out256));
    assert_hex(expected_sha512, out512, sizeof(out512));
}

TEST_CASE("rns crypto hmac sha256 matches RFC4231 vector", "[rns_crypto]")
{
    const uint8_t key[20] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b,
    };
    const uint8_t data[] = "Hi There";
    const uint8_t expected[RNS_CRYPTO_HMAC_SHA256_SIZE] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };

    uint8_t out[RNS_CRYPTO_HMAC_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_hmac_sha256(key, sizeof(key),
                                                     data, sizeof(data) - 1,
                                                     out));
    assert_hex(expected, out, sizeof(out));
}

TEST_CASE("rns crypto hkdf sha256 matches RFC5869 vector", "[rns_crypto]")
{
    const uint8_t ikm[22] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    const uint8_t salt[13] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    };
    const uint8_t info[10] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
        0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    };
    const uint8_t expected[42] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a,
        0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
        0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
        0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf,
        0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18,
        0x58, 0x65,
    };

    uint8_t out[42];
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_hkdf_sha256(ikm, sizeof(ikm),
                                                     salt, sizeof(salt),
                                                     info, sizeof(info),
                                                     out, sizeof(out)));
    assert_hex(expected, out, sizeof(out));
}

TEST_CASE("rns crypto pbkdf2 sha256 matches known vector", "[rns_crypto]")
{
    const uint8_t password[] = "password";
    const uint8_t salt[] = "salt";
    const uint8_t expected[32] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48,
        0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b,
    };

    uint8_t out[32];
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_pbkdf2_sha256(password, sizeof(password) - 1,
                                                       salt, sizeof(salt) - 1,
                                                       1, out, sizeof(out)));
    assert_hex(expected, out, sizeof(out));
}

TEST_CASE("rns crypto aes256 cbc encrypts and decrypts NIST vector", "[rns_crypto]")
{
    const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE] = {
        0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
        0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
        0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
        0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
    };
    const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    const uint8_t plain[RNS_CRYPTO_AES_BLOCK_SIZE] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    };
    const uint8_t expected_cipher[RNS_CRYPTO_AES_BLOCK_SIZE] = {
        0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba,
        0x77, 0x9e, 0xab, 0xfb, 0x5f, 0x7b, 0xfb, 0xd6,
    };

    uint8_t cipher[RNS_CRYPTO_AES_BLOCK_SIZE];
    uint8_t decrypted[RNS_CRYPTO_AES_BLOCK_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_aes256_cbc_encrypt(key, iv, plain,
                                                            sizeof(plain), cipher));
    assert_hex(expected_cipher, cipher, sizeof(cipher));

    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_aes256_cbc_decrypt(key, iv, cipher,
                                                            sizeof(cipher), decrypted));
    assert_hex(plain, decrypted, sizeof(decrypted));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      rns_crypto_aes256_cbc_encrypt(key, iv, plain, 15, cipher));
}

TEST_CASE("rns crypto ed25519 derives monocypher regression key and verifies", "[rns_crypto]")
{
    const uint8_t seed[RNS_CRYPTO_ED25519_SEED_SIZE] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55,
    };
    const uint8_t expected_pubkey[RNS_CRYPTO_ED25519_PUBLIC_SIZE] = {
        0x70, 0x0e, 0x2c, 0xe7, 0xc4, 0xb6, 0x74, 0x42,
        0x7e, 0xab, 0x27, 0xba, 0x82, 0x0b, 0xcf, 0x6f,
        0x0f, 0xae, 0xbe, 0x68, 0xe0, 0x9f, 0xe8, 0x56,
        0x42, 0x92, 0x11, 0x4e, 0x41, 0xdc, 0x6a, 0x41,
    };
    const uint8_t message[] = "reticulum meshpay signature";

    uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE];
    uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE];
    uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE];

    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_ed25519_keypair_from_seed(seed, private_key, public_key));
    assert_hex(seed, private_key, sizeof(seed));
    assert_hex(expected_pubkey, public_key, sizeof(public_key));

    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_ed25519_sign(private_key, message,
                                                      sizeof(message) - 1, signature));
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_ed25519_verify(public_key, message,
                                                        sizeof(message) - 1, signature));
    signature[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_crypto_ed25519_verify(public_key, message,
                                                sizeof(message) - 1, signature));
}

TEST_CASE("rns crypto x25519 shared secret is symmetric", "[rns_crypto]")
{
    const uint8_t alice_private[RNS_CRYPTO_X25519_KEY_SIZE] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
    };
    const uint8_t bob_private[RNS_CRYPTO_X25519_KEY_SIZE] = {
        0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
        0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
        0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
        0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
    };

    uint8_t alice_public[RNS_CRYPTO_X25519_KEY_SIZE];
    uint8_t bob_public[RNS_CRYPTO_X25519_KEY_SIZE];
    uint8_t alice_shared[RNS_CRYPTO_X25519_SHARED_SIZE];
    uint8_t bob_shared[RNS_CRYPTO_X25519_SHARED_SIZE];

    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_x25519_public_key(alice_private, alice_public));
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_x25519_public_key(bob_private, bob_public));
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_x25519_shared_secret(alice_private, bob_public,
                                                              alice_shared));
    TEST_ASSERT_EQUAL(ESP_OK, rns_crypto_x25519_shared_secret(bob_private, alice_public,
                                                              bob_shared));
    assert_hex(alice_shared, bob_shared, sizeof(alice_shared));

    uint8_t zero[RNS_CRYPTO_X25519_KEY_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_crypto_x25519_public_key(zero, alice_public));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_crypto_x25519_shared_secret(alice_private, zero,
                                                      alice_shared));
}
