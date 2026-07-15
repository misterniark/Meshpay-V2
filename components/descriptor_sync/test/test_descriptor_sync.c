#include "meshpay/descriptor_sync.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/rns/rns_identity.h"
#include "unity.h"
#include <string.h>

/* Adresse de destination/source « réaliste » non nulle, réutilisée par les
 * tests (16 octets). */
static void fill_hash(uint8_t out[MESHPAY_TX_DESTINATION_HASH_SIZE], uint8_t seed)
{
    for (size_t i = 0; i < MESHPAY_TX_DESTINATION_HASH_SIZE; ++i) {
        out[i] = (uint8_t)(seed + i);
    }
}

/* Forge un descripteur signé prêt à être offert. */
static void make_signed(meshpay_currency_descriptor_signed_t *signed_desc)
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    strncpy(body.name, "Minimistan", sizeof(body.name) - 1);
    strncpy(body.symbol, "MIN", sizeof(body.symbol) - 1);
    body.max_supply = 1000000ULL;
    body.transfer_fee = 5;
    body.demurrage_enabled = true;
    body.demurrage_bps = 250;
    body.initial_credit = 100;
    body.created_at_ms = 1716200000123ULL;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(signed_desc, &body, &founder));
}

TEST_CASE("descriptor sync request round-trip", "[descriptor_sync]")
{
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(source, 0x40);
    const uint32_t currency_id = 0xDEADBEEFU;

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_request(currency_id, source,
                                                            &packet));
    /* Type + taille exacte attendus. */
    TEST_ASSERT_EQUAL_HEX8(MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST, packet.data[0]);
    TEST_ASSERT_EQUAL_size_t(MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE, packet.data_len);

    uint32_t decoded_id = 0;
    uint8_t decoded_source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_parse_request(&packet, &decoded_id,
                                                            decoded_source));
    TEST_ASSERT_EQUAL_UINT32(currency_id, decoded_id);
    TEST_ASSERT_EQUAL_MEMORY(source, decoded_source, sizeof(source));
}

TEST_CASE("descriptor sync offer round-trip restitue et verifie le descripteur", "[descriptor_sync]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(source, 0x10);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_offer(&signed_desc, source, &packet));
    TEST_ASSERT_EQUAL_HEX8(MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER, packet.data[0]);
    /* Un seul paquet : la taille reste dans le MDU. */
    TEST_ASSERT_TRUE(packet.data_len <= RNS_PACKET_MAX_DATA_SIZE);
    /* Diffusé PLAIN : provenance = adresse de l'émetteur. */
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_PLAIN, packet.destination_type);
    TEST_ASSERT_EQUAL_MEMORY(source, packet.destination_hash, sizeof(source));

    meshpay_currency_descriptor_signed_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_parse_offer(&packet, &decoded));
    /* Le descripteur décodé coïncide à l'octet près et vérifie (bout en bout). */
    TEST_ASSERT_EQUAL_MEMORY(&signed_desc.body, &decoded.body, sizeof(signed_desc.body));
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.genesis_hash, decoded.genesis_hash,
                             sizeof(signed_desc.genesis_hash));
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.founder_signature, decoded.founder_signature,
                             sizeof(signed_desc.founder_signature));
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, decoded.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&decoded));
}

TEST_CASE("descriptor sync parse_request rejette mauvais type et taille", "[descriptor_sync]")
{
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(source, 0x40);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_request(0x12345678U, source,
                                                            &packet));

    /* Mauvais octet de type. */
    rns_packet_t wrong_type = packet;
    wrong_type.data[0] = MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_request(&wrong_type, NULL, NULL));

    /* Taille trop courte. */
    rns_packet_t short_pkt = packet;
    short_pkt.data_len = MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE - 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_request(&short_pkt, NULL, NULL));

    /* Taille trop longue. */
    rns_packet_t long_pkt = packet;
    long_pkt.data_len = MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_request(&long_pkt, NULL, NULL));
}

TEST_CASE("descriptor sync parse_offer rejette mauvais type et wire corrompu", "[descriptor_sync]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);
    uint8_t dest[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(dest, 0x10);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_offer(&signed_desc, dest, &packet));

    meshpay_currency_descriptor_signed_t decoded;

    /* Mauvais octet de type. */
    rns_packet_t wrong_type = packet;
    wrong_type.data[0] = MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_offer(&wrong_type, &decoded));

    /* Wire du descripteur corrompu (1 octet du CBOR retourné) -> decode échoue. */
    rns_packet_t corrupt = packet;
    corrupt.data[3] ^= 0xFF;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_descriptor_sync_parse_offer(&corrupt, &decoded));

    /* Payload vide (juste le type, pas de descripteur). */
    rns_packet_t empty = packet;
    empty.data_len = 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_descriptor_sync_parse_offer(&empty, &decoded));
}

TEST_CASE("descriptor sync rejette source/destination nuls et arguments NULL", "[descriptor_sync]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);
    uint8_t hash[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(hash, 0x40);
    uint8_t zero[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    rns_packet_t packet;

    /* build_request : source nul / NULL / packet NULL. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_request(1, zero, &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_request(1, NULL, &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_request(1, hash, NULL));

    /* build_offer : descripteur NULL / destination nulle / NULL. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_offer(NULL, hash, &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_offer(&signed_desc, zero, &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_offer(&signed_desc, hash, NULL));

    /* parse_* : packet NULL. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_request(NULL, NULL, NULL));
    meshpay_currency_descriptor_signed_t decoded;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_offer(NULL, &decoded));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Palier E1 — message DISCOVER (découverte des monnaies à portée)
 * ══════════════════════════════════════════════════════════════════════════ */

TEST_CASE("descriptor sync discover round-trip", "[descriptor_sync][e1]")
{
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(source, 0x60);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_discover(source, &packet));
    /* Type + taille exacte + diffusion PLAIN broadcast (donnée publique). */
    TEST_ASSERT_EQUAL_HEX8(MESHPAY_DESCRIPTOR_SYNC_MSG_DISCOVER, packet.data[0]);
    TEST_ASSERT_EQUAL_size_t(MESHPAY_DESCRIPTOR_SYNC_DISCOVER_SIZE,
                             packet.data_len);
    TEST_ASSERT_EQUAL(RNS_PACKET_PROPAGATION_BROADCAST, packet.propagation_type);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_PLAIN, packet.destination_type);

    uint8_t decoded_source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_parse_discover(&packet,
                                                             decoded_source));
    TEST_ASSERT_EQUAL_MEMORY(source, decoded_source, sizeof(source));
}

TEST_CASE("descriptor sync parse_discover rejette type, taille et args",
          "[descriptor_sync][e1]")
{
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_hash(source, 0x61);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_discover(source, &packet));

    /* Mauvais type (un REQUEST n'est pas un DISCOVER). */
    rns_packet_t wrong = packet;
    wrong.data[0] = MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_discover(&wrong, NULL));

    /* Taille tronquée / rallongée. */
    rns_packet_t truncated = packet;
    truncated.data_len -= 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_discover(&truncated, NULL));
    rns_packet_t inflated = packet;
    inflated.data_len += 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_discover(&inflated, NULL));

    /* Arguments invalides côté build : source NULL, source nulle, packet NULL. */
    uint8_t zero_source[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_discover(NULL, &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_discover(zero_source,
                                                             &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_build_discover(source, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_descriptor_sync_parse_discover(NULL, NULL));
}
