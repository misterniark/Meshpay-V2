#include "meshpay/rns/rns_packet_crypto.h"
#include "unity.h"
#include <string.h>

typedef struct {
    const uint8_t *stream;
    size_t len;
    size_t pos;
} deterministic_rng_t;

static esp_err_t deterministic_rng(void *ctx, uint8_t *out, size_t len)
{
    deterministic_rng_t *rng = (deterministic_rng_t *)ctx;
    if (rng == NULL || out == NULL || rng->pos + len > rng->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, rng->stream + rng->pos, len);
    rng->pos += len;
    return ESP_OK;
}

TEST_CASE("rns packet crypto encrypts and decrypts single destination fixture", "[rns_packet_crypto]")
{
    const uint8_t recipient_private[RNS_IDENTITY_PRIVATE_SIZE] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55,
    };
    const uint8_t rng_stream[] = {
        0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
        0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
        0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
        0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    const uint8_t expected_token[] = {
        0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
        0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
        0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
        0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x4a, 0xc0, 0x98, 0xed, 0xf4, 0x65, 0xa8, 0x4a,
        0x53, 0x16, 0x45, 0x15, 0xdd, 0xc1, 0xd0, 0x29,
        0x0c, 0x93, 0x98, 0x1c, 0xe4, 0xd4, 0x65, 0x4c,
        0xd4, 0x32, 0xb2, 0xa0, 0x02, 0x6e, 0x03, 0x02,
        0xb6, 0x4f, 0x49, 0x7e, 0xe1, 0xaf, 0xdf, 0x07,
        0x8d, 0x90, 0x6f, 0xbe, 0x61, 0x6e, 0x86, 0xe2,
        0x2f, 0x65, 0x6e, 0x98, 0xd4, 0x79, 0x53, 0x2c,
        0x29, 0xaa, 0xd6, 0x9e, 0x18, 0x5f, 0x7f, 0xf7,
        0x0d, 0xcb, 0xde, 0xd7, 0x98, 0x1a, 0xf8, 0xd2,
        0x21, 0x9a, 0x39, 0xfc, 0x2a, 0x91, 0x26, 0x78,
    };
    const uint8_t plaintext[] = "meshpay reticulum encrypted data";

    rns_identity_t recipient;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&recipient, recipient_private));
    uint8_t recipient_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&recipient, recipient_public));

    rns_identity_t recipient_public_only;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_public(&recipient_public_only, recipient_public));

    deterministic_rng_t rng = {
        .stream = rng_stream,
        .len = sizeof(rng_stream),
        .pos = 0,
    };
    rns_crypto_set_rng(deterministic_rng, &rng);

    uint8_t token[RNS_PACKET_CRYPTO_MAX_TOKEN_SIZE];
    size_t token_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_crypto_encrypt_single(&recipient_public_only,
                                                               plaintext,
                                                               sizeof(plaintext) - 1,
                                                               token,
                                                               sizeof(token),
                                                               &token_len));
    rns_crypto_set_rng(NULL, NULL);

    TEST_ASSERT_EQUAL_UINT32(sizeof(expected_token), token_len);
    TEST_ASSERT_EQUAL_MEMORY(expected_token, token, sizeof(expected_token));
    TEST_ASSERT_EQUAL_UINT32(sizeof(rng_stream), rng.pos);

    uint8_t decrypted[64];
    size_t decrypted_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_packet_crypto_decrypt_single(&recipient,
                                                               token,
                                                               token_len,
                                                               decrypted,
                                                               sizeof(decrypted),
                                                               &decrypted_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(plaintext) - 1, decrypted_len);
    TEST_ASSERT_EQUAL_MEMORY(plaintext, decrypted, decrypted_len);
}

TEST_CASE("rns packet crypto rejects tampering and oversize plaintext", "[rns_packet_crypto]")
{
    rns_identity_t empty_identity;
    rns_identity_clear(&empty_identity);

    uint8_t token[RNS_PACKET_CRYPTO_MAX_TOKEN_SIZE] = {0};
    uint8_t plaintext[1] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      rns_packet_crypto_decrypt_single(&empty_identity,
                                                       token,
                                                       sizeof(token),
                                                       plaintext,
                                                       sizeof(plaintext),
                                                       NULL));

    const uint8_t recipient_private[RNS_IDENTITY_PRIVATE_SIZE] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55,
    };
    const uint8_t zero_rng_stream[48] = {0};
    const uint8_t rng_stream[48] = {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
        0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
        0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60,
    };
    uint8_t too_large[RNS_PACKET_CRYPTO_MAX_PLAINTEXT_SIZE + 1] = {0};

    rns_identity_t recipient;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(&recipient, recipient_private));

    const uint8_t small_plaintext[] = "x";
    deterministic_rng_t zero_rng = {
        .stream = zero_rng_stream,
        .len = sizeof(zero_rng_stream),
        .pos = 0,
    };
    rns_crypto_set_rng(deterministic_rng, &zero_rng);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_packet_crypto_encrypt_single(&recipient,
                                                       small_plaintext,
                                                       sizeof(small_plaintext) - 1,
                                                       token,
                                                       sizeof(token),
                                                       NULL));
    rns_crypto_set_rng(NULL, NULL);

    deterministic_rng_t rng = {
        .stream = rng_stream,
        .len = sizeof(rng_stream),
        .pos = 0,
    };
    rns_crypto_set_rng(deterministic_rng, &rng);
    size_t token_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_packet_crypto_encrypt_single(&recipient,
                                                       small_plaintext,
                                                       sizeof(small_plaintext) - 1,
                                                       token,
                                                       sizeof(token),
                                                       &token_len));
    rns_crypto_set_rng(NULL, NULL);

    token[token_len - 1] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_packet_crypto_decrypt_single(&recipient,
                                                       token,
                                                       token_len,
                                                       plaintext,
                                                       sizeof(plaintext),
                                                       NULL));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      rns_packet_crypto_encrypt_single(&recipient,
                                                       too_large,
                                                       sizeof(too_large),
                                                       token,
                                                       sizeof(token),
                                                       NULL));
}
