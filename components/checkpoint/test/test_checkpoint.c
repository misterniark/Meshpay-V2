#include "meshpay/checkpoint.h"
#include "unity.h"
#include <string.h>

/* --- Chantier Phase B (P1) : format checkpoint signé fondateur --- */

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

/* Construit un checkpoint plausible : le fondateur (clé nulle = « voir
 * descripteur ») + un membre avec sa clé d'annuaire. */
static void build_checkpoint(meshpay_checkpoint_t *cp,
                             const rns_identity_t *member)
{
    meshpay_checkpoint_init(cp);
    cp->currency_id = 0xc5c42609;
    cp->generation = 1;
    cp->created_at_ms = 123456;
    fill_sequence(cp->horizon_digest, sizeof(cp->horizon_digest), 0xD0);
    cp->account_count = 2;
    /* Fondateur : clé d'annuaire NULLE (sa clé est celle du descripteur). */
    fill_sequence(cp->accounts[0].account, RNS_IDENTITY_HASH_SIZE, 0x10);
    cp->accounts[0].balance = 12;
    cp->accounts[0].seq_floor = 5;
    /* Membre : clé réelle publiée par sa CLAIM (l'annuaire survit). */
    fill_sequence(cp->accounts[1].account, RNS_IDENTITY_HASH_SIZE, 0x20);
    cp->accounts[1].balance = 8;
    cp->accounts[1].seq_floor = 3;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(member,
                                                  cp->accounts[1].member_public));
}

TEST_CASE("checkpoint sign verify roundtrip via wire", "[checkpoint]")
{
    rns_identity_t founder;
    rns_identity_t member;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&member));

    meshpay_checkpoint_t cp;
    build_checkpoint(&cp, &member);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&cp, &founder));

    uint8_t wire[MESHPAY_CHECKPOINT_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_encode(&cp, wire,
                                                        sizeof(wire),
                                                        &wire_len));
    TEST_ASSERT_TRUE(wire_len > MESHPAY_CHECKPOINT_PREFIX_SIZE);

    meshpay_checkpoint_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_checkpoint_decode(wire, wire_len, &decoded));
    TEST_ASSERT_EQUAL_UINT32(cp.currency_id, decoded.currency_id);
    TEST_ASSERT_EQUAL_UINT32(1, decoded.generation);
    TEST_ASSERT_EQUAL_UINT16(2, decoded.account_count);
    TEST_ASSERT_EQUAL_UINT32(12, decoded.accounts[0].balance);
    TEST_ASSERT_EQUAL_UINT32(3, decoded.accounts[1].seq_floor);
    TEST_ASSERT_EQUAL_MEMORY(cp.accounts[1].member_public,
                             decoded.accounts[1].member_public,
                             RNS_IDENTITY_PUBLIC_SIZE);

    /* Vérification contre la clé du DESCRIPTEUR (racine de confiance). */
    uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&founder, founder_public));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_checkpoint_verify(&decoded, founder_public));

    /* find_account : présent et absent. */
    TEST_ASSERT_NOT_NULL(
        meshpay_checkpoint_find_account(&decoded, cp.accounts[1].account));
    uint8_t unknown[RNS_IDENTITY_HASH_SIZE];
    fill_sequence(unknown, sizeof(unknown), 0x77);
    TEST_ASSERT_NULL(meshpay_checkpoint_find_account(&decoded, unknown));
}

TEST_CASE("checkpoint rejects forgery and tampering", "[checkpoint]")
{
    rns_identity_t founder;
    rns_identity_t member;
    rns_identity_t attacker;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&member));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&attacker));

    meshpay_checkpoint_t cp;
    build_checkpoint(&cp, &member);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&cp, &founder));
    uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&founder, founder_public));

    /* Altérer un SOLDE après signature : verify échoue. */
    meshpay_checkpoint_t tampered = cp;
    tampered.accounts[1].balance += 1000;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_checkpoint_verify(&tampered, founder_public));

    /* Altérer un PLANCHER (rejouer de vieilles tx) : verify échoue. */
    tampered = cp;
    tampered.accounts[1].seq_floor = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_checkpoint_verify(&tampered, founder_public));

    /* Substituer la clé d'annuaire d'un membre : verify échoue. */
    tampered = cp;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(
                          &attacker, tampered.accounts[1].member_public));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_checkpoint_verify(&tampered, founder_public));

    /* Checkpoint signé par un IMPOSTEUR : refusé contre la clé du fondateur. */
    meshpay_checkpoint_t forged;
    build_checkpoint(&forged, &member);
    forged.generation = 2;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&forged, &attacker));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_checkpoint_verify(&forged, founder_public));
}

TEST_CASE("checkpoint rejects malformed bodies", "[checkpoint]")
{
    rns_identity_t founder;
    rns_identity_t member;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&member));

    meshpay_checkpoint_t cp;

    /* generation 0 : réservée « aucun checkpoint » — refusée à la signature. */
    build_checkpoint(&cp, &member);
    cp.generation = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&cp, &founder));

    /* Comptes en DOUBLON : table ambiguë, refusée. */
    build_checkpoint(&cp, &member);
    memcpy(cp.accounts[1].account, cp.accounts[0].account,
           RNS_IDENTITY_HASH_SIZE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&cp, &founder));

    /* Wire complet exigé signé : encode sans signature refuse. */
    build_checkpoint(&cp, &member);
    uint8_t wire[MESHPAY_CHECKPOINT_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_checkpoint_encode(&cp, wire, sizeof(wire),
                                                &wire_len));

    /* Décodage : magic inconnu / version future / octets orphelins. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(&cp, &founder));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_encode(&cp, wire,
                                                        sizeof(wire),
                                                        &wire_len));
    meshpay_checkpoint_t decoded;
    uint8_t bad[MESHPAY_CHECKPOINT_CBOR_MAX];
    memcpy(bad, wire, wire_len);
    bad[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_checkpoint_decode(bad, wire_len, &decoded));
    memcpy(bad, wire, wire_len);
    bad[4] = 9;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION,
                      meshpay_checkpoint_decode(bad, wire_len, &decoded));
    memcpy(bad, wire, wire_len);
    bad[wire_len] = 0x00;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_checkpoint_decode(bad, wire_len + 1, &decoded));

    /* Altération d'un octet du corps : le CBOR casse OU la validation ; en
     * dernier recours la signature (verify) — jamais un état partiel. */
    memcpy(bad, wire, wire_len);
    bad[10] ^= 0x01;
    if (meshpay_checkpoint_decode(bad, wire_len, &decoded) == ESP_OK) {
        uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE];
        TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&founder,
                                                              founder_public));
        TEST_ASSERT_NOT_EQUAL(ESP_OK,
                              meshpay_checkpoint_verify(&decoded,
                                                        founder_public));
    }
}
