#include "meshpay/rns/rns_announce.h"
#include "unity.h"
#include <string.h>

static void load_fixture_identity(rns_identity_t *identity)
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

/* Charge une identité DIFFÉRENTE de la fixture (premier octet de la clé privée
 * modifié) : sert aux tests d'itération qui exigent deux destinations
 * distinctes dans l'annuaire. */
static void load_second_identity(rns_identity_t *identity)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE] = {
        0x78, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
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

/* Fabrique un paquet ANNOUNCE signé et valide pour `identity`, portant
 * `app_data`, et le mémorise dans l'annuaire via verify_and_remember.
 * Renvoie la destination correspondante dans `destination`. */
static void remember_announce(rns_identity_t *identity,
                              const uint8_t *app_data,
                              size_t app_data_len,
                              rns_destination_t *destination)
{
    const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE] = {
        0xb0, 0xb1, 0xb2, 0xb3, 0xb4,
        0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
    };
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(identity, destination));

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(packet.destination_hash, destination->hash, RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_encode(destination,
                                                  identity,
                                                  random_hash,
                                                  app_data,
                                                  app_data_len,
                                                  packet.data,
                                                  sizeof(packet.data),
                                                  &packet.data_len));
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_verify_and_remember(&packet, NULL));
}

TEST_CASE("rns announce encodes verifies and remembers app data", "[rns_announce]")
{
    const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
        0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    };
    const uint8_t expected_announce[] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
        0x70, 0x0e, 0x2c, 0xe7, 0xc4, 0xb6, 0x74, 0x42,
        0x7e, 0xab, 0x27, 0xba, 0x82, 0x0b, 0xcf, 0x6f,
        0x0f, 0xae, 0xbe, 0x68, 0xe0, 0x9f, 0xe8, 0x56,
        0x42, 0x92, 0x11, 0x4e, 0x41, 0xdc, 0x6a, 0x41,
        0x04, 0x9d, 0x90, 0x46, 0xc7, 0x4d, 0xe4, 0x6b,
        0x50, 0xa3, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
        0xa6, 0xa7, 0xa8, 0xa9, 0x56, 0xf5, 0x74, 0x01,
        0xe5, 0x87, 0x8b, 0xcd, 0xd0, 0x7c, 0x2b, 0x87,
        0x13, 0x81, 0x88, 0xac, 0x57, 0x37, 0x15, 0xcb,
        0x63, 0x7b, 0x64, 0x47, 0x7f, 0x43, 0x30, 0x33,
        0xeb, 0xcc, 0xe3, 0x9f, 0x10, 0x63, 0x5e, 0x0d,
        0x33, 0xc0, 0x05, 0x21, 0x64, 0x3d, 0x2a, 0x46,
        0x79, 0xc9, 0xbb, 0xf9, 0x7c, 0xc7, 0x94, 0x05,
        0x9d, 0xa2, 0xab, 0x11, 0x0d, 0x1e, 0xef, 0x31,
        0x23, 0x07, 0x50, 0x02, 0x41, 0x6c, 0x69, 0x63,
        0x65,
    };
    const uint8_t app_data[] = "Alice";

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&identity, &destination));

    uint8_t encoded[RNS_PACKET_MAX_DATA_SIZE];
    size_t encoded_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_encode(&destination,
                                                  &identity,
                                                  random_hash,
                                                  app_data,
                                                  sizeof(app_data) - 1,
                                                  encoded,
                                                  sizeof(encoded),
                                                  &encoded_len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected_announce), encoded_len);
    TEST_ASSERT_EQUAL_MEMORY(expected_announce, encoded, sizeof(expected_announce));

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(packet.destination_hash, destination.hash, RNS_DESTINATION_HASH_SIZE);
    memcpy(packet.data, encoded, encoded_len);
    packet.data_len = encoded_len;

    rns_announce_known_reset();
    rns_announce_t announce;
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_verify_and_remember(&packet, &announce));
    TEST_ASSERT_EQUAL_MEMORY(destination.hash, announce.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(app_data, announce.app_data, sizeof(app_data) - 1);
    TEST_ASSERT_EQUAL_UINT32(sizeof(app_data) - 1, announce.app_data_len);

    TEST_ASSERT_EQUAL_UINT32(1, rns_announce_known_count());
    rns_announce_known_destination_t known;
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_recall_copy(destination.hash, &known));
    TEST_ASSERT_EQUAL_MEMORY(app_data, known.app_data, sizeof(app_data) - 1);
    TEST_ASSERT_EQUAL_UINT32(sizeof(app_data) - 1, known.app_data_len);
    TEST_ASSERT_NOT_EQUAL(0, known.packet_hash[0] | known.packet_hash[31]);
}

TEST_CASE("rns announce recall copy renvoie une copie detachee de la table", "[rns_announce]")
{
    const uint8_t app_data[] = "Alice";

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_announce_known_reset();
    rns_destination_t destination;
    remember_announce(&identity, app_data, sizeof(app_data) - 1, &destination);

    /* Garde-fous d'arguments : NULL est rejeté sans toucher la table. */
    rns_announce_known_destination_t copy;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rns_announce_recall_copy(NULL, &copy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_announce_recall_copy(destination.hash, NULL));

    /* La copie reflète le slot vivant au moment de l'appel. */
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_recall_copy(destination.hash, &copy));
    TEST_ASSERT_EQUAL_MEMORY(destination.hash, copy.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_UINT32(sizeof(app_data) - 1, copy.app_data_len);
    TEST_ASSERT_EQUAL_MEMORY(app_data, copy.app_data, sizeof(app_data) - 1);

    uint8_t expected_public[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&identity, expected_public));
    TEST_ASSERT_EQUAL_MEMORY(expected_public, copy.public_key,
                             RNS_ANNOUNCE_PUBLIC_KEY_SIZE);

    /* Preuve que c'est une COPIE et non un pointeur déguisé : on efface la
     * table vivante, la copie doit rester intacte (l'ancien API à pointeur
     * brut aurait ici exposé un slot zéroïsé — c'était la course de données). */
    rns_announce_known_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      rns_announce_recall_copy(destination.hash, &copy));
    TEST_ASSERT_EQUAL_MEMORY(app_data, copy.app_data, sizeof(app_data) - 1);
    TEST_ASSERT_EQUAL_MEMORY(destination.hash, copy.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);
}

TEST_CASE("rns announce recall info expose une vue reduite et tronquee", "[rns_announce]")
{
    /* app_data volontairement plus longue que la capacité de la vue (32) pour
     * vérifier la troncature — 40 octets imprimables. */
    const uint8_t app_data[] = "0123456789012345678901234567890123456789";

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_announce_known_reset();
    rns_destination_t destination;
    remember_announce(&identity, app_data, sizeof(app_data) - 1, &destination);

    rns_announce_peer_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rns_announce_recall_info(NULL, &info));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_announce_recall_info(destination.hash, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_recall_info(destination.hash, &info));
    TEST_ASSERT_EQUAL_MEMORY(destination.hash, info.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);
    /* La vue tronque à RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX sans déborder. */
    TEST_ASSERT_EQUAL_UINT32(RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX,
                             info.app_data_len);
    TEST_ASSERT_EQUAL_MEMORY(app_data, info.app_data,
                             RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX);

    uint8_t expected_public[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&identity, expected_public));
    TEST_ASSERT_EQUAL_MEMORY(expected_public, info.public_key,
                             RNS_ANNOUNCE_PUBLIC_KEY_SIZE);

    /* Une destination inconnue ne remplit rien et le signale. */
    uint8_t unknown_hash[RNS_DESTINATION_HASH_SIZE];
    memcpy(unknown_hash, destination.hash, sizeof(unknown_hash));
    unknown_hash[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      rns_announce_recall_info(unknown_hash, &info));
}

TEST_CASE("rns announce known info itere les pairs par index ordinal", "[rns_announce]")
{
    const uint8_t alias_a[] = "Alice";
    const uint8_t alias_b[] = "Bob";

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_fixture_identity(&identity_a);
    load_second_identity(&identity_b);

    rns_announce_known_reset();
    rns_destination_t destination_a;
    rns_destination_t destination_b;
    remember_announce(&identity_a, alias_a, sizeof(alias_a) - 1, &destination_a);
    remember_announce(&identity_b, alias_b, sizeof(alias_b) - 1, &destination_b);
    TEST_ASSERT_EQUAL_UINT32(2, rns_announce_known_count());

    /* Les deux identités doivent produire des destinations distinctes, sinon
     * le test d'itération ne prouve rien. */
    TEST_ASSERT_FALSE(rns_destination_hash_equal(destination_a.hash,
                                                 destination_b.hash));

    rns_announce_peer_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rns_announce_known_info(0, NULL));

    /* Chaque index ordinal renvoie un pair, chaque pair attendu est vu une
     * seule fois (l'ordre interne des slots n'est pas contractuel). */
    bool seen_a = false;
    bool seen_b = false;
    for (size_t i = 0; i < 2; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, rns_announce_known_info(i, &info));
        if (rns_destination_hash_equal(info.destination_hash, destination_a.hash)) {
            TEST_ASSERT_FALSE(seen_a);
            TEST_ASSERT_EQUAL_UINT32(sizeof(alias_a) - 1, info.app_data_len);
            TEST_ASSERT_EQUAL_MEMORY(alias_a, info.app_data, sizeof(alias_a) - 1);
            seen_a = true;
        } else if (rns_destination_hash_equal(info.destination_hash,
                                              destination_b.hash)) {
            TEST_ASSERT_FALSE(seen_b);
            TEST_ASSERT_EQUAL_UINT32(sizeof(alias_b) - 1, info.app_data_len);
            TEST_ASSERT_EQUAL_MEMORY(alias_b, info.app_data, sizeof(alias_b) - 1);
            seen_b = true;
        } else {
            TEST_FAIL_MESSAGE("pair inattendu dans l'annuaire");
        }
    }
    TEST_ASSERT_TRUE(seen_a);
    TEST_ASSERT_TRUE(seen_b);

    /* Au-delà du dernier pair occupé : NOT_FOUND, jamais de lecture hors table. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, rns_announce_known_info(2, &info));
}

TEST_CASE("rns announce rejects altered signatures and destination mismatch", "[rns_announce]")
{
    const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
        0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    };
    const uint8_t app_data[] = "Alice";

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&identity, &destination));

    rns_packet_t packet;
    rns_packet_clear(&packet);
    packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    packet.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(packet.destination_hash, destination.hash, RNS_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, rns_announce_encode(&destination,
                                                  &identity,
                                                  random_hash,
                                                  app_data,
                                                  sizeof(app_data) - 1,
                                                  packet.data,
                                                  sizeof(packet.data),
                                                  &packet.data_len));

    packet.data[RNS_ANNOUNCE_BASE_SIZE - 1] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_announce_verify(&packet, NULL));
    packet.data[RNS_ANNOUNCE_BASE_SIZE - 1] ^= 0x01;

    packet.destination_hash[0] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, rns_announce_verify(&packet, NULL));
}

TEST_CASE("rns announce rejects zero random hash", "[rns_announce]")
{
    const uint8_t zero_random[RNS_ANNOUNCE_RANDOM_HASH_SIZE] = {0};
    const uint8_t app_data[] = "Alice";

    rns_identity_t identity;
    load_fixture_identity(&identity);

    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(&identity, &destination));

    uint8_t encoded[RNS_PACKET_MAX_DATA_SIZE];
    size_t encoded_len = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_announce_encode(&destination,
                                          &identity,
                                          zero_random,
                                          app_data,
                                          sizeof(app_data) - 1,
                                          encoded,
                                          sizeof(encoded),
                                          &encoded_len));
}
