#include "meshpay/meshpay_tx.h"
#include "unity.h"
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void load_identity(rns_identity_t *identity, uint8_t seed_base)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(private_key, sizeof(private_key), seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

static void load_public_identity(const rns_identity_t *private_identity,
                                 rns_identity_t *public_identity)
{
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(private_identity,
                                                          public_key));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_public(public_identity,
                                                       public_key));
}

static void fill_transfer_inputs(uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE])
{
    fill_sequence(from, MESHPAY_TX_DESTINATION_HASH_SIZE, 0x10);
    fill_sequence(to, MESHPAY_TX_DESTINATION_HASH_SIZE, 0x40);
    fill_sequence(parents[0], MESHPAY_TX_PARENT_ID_SIZE, 0x70);
    fill_sequence(parents[1], MESHPAY_TX_PARENT_ID_SIZE, 0xa0);
}

TEST_CASE("meshpay tx encodes decodes and verifies signed transfer", "[meshpay_tx]")
{
    rns_identity_t signer;
    rns_identity_t signer_public;
    load_identity(&signer, 0x01);
    load_public_identity(&signer, &signer_public);

    uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
    fill_transfer_inputs(from, to, parents);

    meshpay_tx_t tx;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_transfer(&tx, &signer,
                                                         from, to,
                                                         1234, 42, 7,
                                                         0x4d505632,
                                                         parents, 2,
                                                         1716200000123ULL));
    TEST_ASSERT_EQUAL(MESHPAY_TX_TYPE_TRANSFER, tx.type);
    TEST_ASSERT_EQUAL_UINT32(1234, tx.amount);
    TEST_ASSERT_EQUAL_UINT8(2, tx.parent_count);

    uint8_t signable[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t signable_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_encode_signable(&tx, signable,
                                                         sizeof(signable),
                                                         &signable_len));
    TEST_ASSERT_EQUAL_HEX8(0xa9, signable[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, signable[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, signable[2]);

    uint8_t wire[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_encode(&tx, wire, sizeof(wire),
                                                &wire_len));
    TEST_ASSERT_TRUE(wire_len < MESHPAY_TX_CBOR_MAX_SIZE);
    TEST_ASSERT_EQUAL_HEX8(0xab, wire[0]);

    meshpay_tx_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_decode(wire, wire_len, &decoded));
    TEST_ASSERT_EQUAL(MESHPAY_TX_TYPE_TRANSFER, decoded.type);
    TEST_ASSERT_EQUAL_UINT32(tx.amount, decoded.amount);
    TEST_ASSERT_EQUAL_UINT32(tx.seq, decoded.seq);
    TEST_ASSERT_EQUAL_UINT32(tx.fee, decoded.fee);
    TEST_ASSERT_EQUAL_UINT32(tx.currency_id, decoded.currency_id);
    TEST_ASSERT_EQUAL_UINT64(tx.timestamp_ms, decoded.timestamp_ms);
    TEST_ASSERT_EQUAL_UINT8(tx.parent_count, decoded.parent_count);
    TEST_ASSERT_EQUAL_MEMORY(tx.from, decoded.from, sizeof(tx.from));
    TEST_ASSERT_EQUAL_MEMORY(tx.to, decoded.to, sizeof(tx.to));
    TEST_ASSERT_EQUAL_MEMORY(tx.parents, decoded.parents, sizeof(tx.parents));
    TEST_ASSERT_EQUAL_MEMORY(tx.id, decoded.id, sizeof(tx.id));
    TEST_ASSERT_EQUAL_MEMORY(tx.signature, decoded.signature,
                             sizeof(tx.signature));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_verify(&decoded, &signer_public));
}

TEST_CASE("meshpay tx signature rejects altered field", "[meshpay_tx]")
{
    rns_identity_t signer;
    rns_identity_t signer_public;
    load_identity(&signer, 0x21);
    load_public_identity(&signer, &signer_public);

    uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
    fill_transfer_inputs(from, to, parents);

    meshpay_tx_t tx;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_transfer(&tx, &signer,
                                                         from, to,
                                                         500, 3, 1, 9,
                                                         parents, 1, 99));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_verify(&tx, &signer_public));

    tx.to[0] ^= 0x55;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_tx_verify(&tx, &signer_public));
}

TEST_CASE("meshpay tx creates mint with zero fee", "[meshpay_tx]")
{
    rns_identity_t signer;
    rns_identity_t signer_public;
    load_identity(&signer, 0x41);
    load_public_identity(&signer, &signer_public);

    uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(from, sizeof(from), 0x22);
    fill_sequence(to, sizeof(to), 0x52);

    meshpay_tx_t tx;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_mint(&tx, &signer,
                                                     from, to,
                                                     100000, 0,
                                                     0x4d505632,
                                                     NULL, 0, 1));
    TEST_ASSERT_EQUAL(MESHPAY_TX_TYPE_MINT, tx.type);
    TEST_ASSERT_EQUAL_UINT32(0, tx.fee);
    TEST_ASSERT_EQUAL_UINT8(0, tx.parent_count);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_verify(&tx, &signer_public));

    tx.fee = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_tx_verify(&tx, &signer_public));
}

TEST_CASE("meshpay tx rejects degenerate transfer fee", "[meshpay_tx]")
{
    rns_identity_t signer;
    load_identity(&signer, 0x61);

    uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
    fill_transfer_inputs(from, to, parents);

    meshpay_tx_t tx;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_tx_create_transfer(&tx, &signer,
                                                 from, to,
                                                 50, 1, 50, 1,
                                                 parents, 1, 1));
}
