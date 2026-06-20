#include "meshpay/rns/rns_link_request.h"
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

static void load_destination_identity(rns_identity_t *identity)
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

TEST_CASE("rns link request rejects unsupported signalling and destination type",
          "[rns_link_request]")
{
    uint8_t signalling[RNS_LINK_MTU_SIGNAL_SIZE];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_link_signalling_bytes(0,
                                                RNS_LINK_MODE_AES256_CBC,
                                                signalling));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_link_signalling_bytes(RNS_PACKET_MTU + 1,
                                                RNS_LINK_MODE_AES256_CBC,
                                                signalling));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_link_signalling_bytes(RNS_PACKET_MTU,
                                                0xff,
                                                signalling));

    rns_packet_t request;
    rns_packet_clear(&request);
    request.destination_type = RNS_DESTINATION_TYPE_LINK;
    request.packet_type = RNS_PACKET_TYPE_LINK_REQUEST;
    request.data_len = RNS_LINK_REQUEST_MIN_SIZE;

    uint8_t link_id[RNS_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_link_request_link_id(&request, link_id));
}

TEST_CASE("rns link request completes simulated handshake", "[rns_link_request]")
{
    uint8_t rng_bytes[128];
    for (size_t i = 0; i < sizeof(rng_bytes); ++i) {
        rng_bytes[i] = (uint8_t)(0x11 + i);
    }
    deterministic_rng_t rng = {
        .stream = rng_bytes,
        .len = sizeof(rng_bytes),
        .pos = 0,
    };
    rns_crypto_set_rng(deterministic_rng, &rng);

    rns_identity_t destination_identity;
    load_destination_identity(&destination_identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&destination_identity,
                                                                    &destination));

    rns_link_t initiator;
    rns_packet_t request;
    TEST_ASSERT_EQUAL(ESP_OK, rns_link_request_create(&destination,
                                                      RNS_PACKET_MTU,
                                                      &initiator,
                                                      &request));
    TEST_ASSERT_EQUAL_UINT32(RNS_LINK_REQUEST_MAX_SIZE, request.data_len);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_LINK_REQUEST, request.packet_type);
    TEST_ASSERT_EQUAL(RNS_LINK_STATUS_PENDING, initiator.status);

    rns_link_t responder;
    rns_packet_t proof;
    TEST_ASSERT_EQUAL(ESP_OK, rns_link_request_accept(&destination_identity,
                                                      &request,
                                                      RNS_PACKET_MTU,
                                                      &responder,
                                                      &proof));
    rns_crypto_set_rng(NULL, NULL);

    TEST_ASSERT_EQUAL_UINT32(sizeof(rng_bytes), rng.pos);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_PROOF, proof.packet_type);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_LINK, proof.destination_type);
    TEST_ASSERT_EQUAL_UINT8(RNS_PACKET_CONTEXT_LRPROOF, proof.context);
    TEST_ASSERT_EQUAL_UINT32(RNS_LINK_PROOF_MAX_SIZE, proof.data_len);
    TEST_ASSERT_EQUAL_MEMORY(initiator.link_id, responder.link_id, RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(initiator.link_id, proof.destination_hash, RNS_DESTINATION_HASH_SIZE);

    TEST_ASSERT_EQUAL(ESP_OK, rns_link_request_validate_proof(&destination_identity,
                                                              &proof,
                                                              &initiator));
    TEST_ASSERT_EQUAL(RNS_LINK_STATUS_ACTIVE, initiator.status);
    TEST_ASSERT_EQUAL(RNS_LINK_STATUS_ACTIVE, responder.status);
    TEST_ASSERT_EQUAL_MEMORY(initiator.shared_key,
                             responder.shared_key,
                             RNS_CRYPTO_X25519_SHARED_SIZE);
    TEST_ASSERT_EQUAL_UINT32(RNS_PACKET_MTU, initiator.mtu);
}

TEST_CASE("rns link request rejects altered proof signature", "[rns_link_request]")
{
    uint8_t rng_bytes[128];
    for (size_t i = 0; i < sizeof(rng_bytes); ++i) {
        rng_bytes[i] = (uint8_t)(0x61 + i);
    }
    deterministic_rng_t rng = {
        .stream = rng_bytes,
        .len = sizeof(rng_bytes),
        .pos = 0,
    };
    rns_crypto_set_rng(deterministic_rng, &rng);

    rns_identity_t destination_identity;
    load_destination_identity(&destination_identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&destination_identity,
                                                                    &destination));

    rns_link_t initiator;
    rns_packet_t request;
    rns_link_t responder;
    rns_packet_t proof;
    TEST_ASSERT_EQUAL(ESP_OK, rns_link_request_create(&destination,
                                                      RNS_PACKET_MTU,
                                                      &initiator,
                                                      &request));
    TEST_ASSERT_EQUAL(ESP_OK, rns_link_request_accept(&destination_identity,
                                                      &request,
                                                      RNS_PACKET_MTU,
                                                      &responder,
                                                      &proof));
    rns_crypto_set_rng(NULL, NULL);

    proof.data[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_link_request_validate_proof(&destination_identity,
                                                      &proof,
                                                      &initiator));
}
