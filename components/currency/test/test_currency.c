#include "meshpay/currency.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_destination.h"
#include "meshpay/rns/rns_identity.h"
#include "test_pool.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

static void make_tx(meshpay_tx_t *tx,
                    meshpay_tx_type_t type,
                    uint8_t id_seed,
                    const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                    const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                    uint32_t amount,
                    uint32_t seq,
                    uint32_t fee,
                    uint32_t currency_id,
                    const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                    uint8_t parent_count)
{
    meshpay_tx_clear(tx);
    tx->type = type;
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    memcpy(tx->from, from, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = fee;
    tx->currency_id = currency_id;
    tx->timestamp_ms = 1000 + seq;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x30));
}

TEST_CASE("currency computes balance and routes fee to first mint authority", "[currency]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d505632);
    config.max_supply = 2000;
    config.transfer_fee = 7;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x20,
            master, alice, 1000, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &mint));

    uint8_t parents[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parents[0], mint.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t transfer;
    make_tx(&transfer, MESHPAY_TX_TYPE_TRANSFER, 0x50,
            alice, bob, 100, 1, config.transfer_fee, config.currency_id,
            parents, 1);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &transfer));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &transfer));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, dag,
                                                           alice, &balance));
    TEST_ASSERT_EQUAL_UINT32(893, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, dag,
                                                           bob, &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, dag,
                                                           master, &balance));
    TEST_ASSERT_EQUAL_UINT32(7, balance);

    uint64_t total_minted = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, dag,
                                                            &total_minted));
    TEST_ASSERT_EQUAL_UINT64(1000, total_minted);
}

TEST_CASE("currency rejects unauthorized mint", "[currency]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t impostor[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x11);
    fill_sequence(impostor, sizeof(impostor), 0x21);
    fill_sequence(alice, sizeof(alice), 0x41);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x22,
            impostor, alice, 1000, 0, 0, config.currency_id, NULL, 0);

    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_NOT_AUTHORITY,
                      meshpay_currency_validate_tx(&config, dag, &mint));
}

TEST_CASE("currency mint authority add is idempotent when full", "[currency]")
{
    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);

    uint8_t authorities[MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES + 1]
                       [MESHPAY_TX_DESTINATION_HASH_SIZE];
    for (uint8_t i = 0; i < MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES + 1; ++i) {
        fill_sequence(authorities[i], sizeof(authorities[i]),
                      (uint8_t)(0x20 + i * 0x10));
    }

    for (uint8_t i = 0; i < MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_currency_add_mint_authority(
                              &config,
                              authorities[i]));
    }
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES,
                            config.mint_authority_count);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(
                          &config,
                          authorities[0]));
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES,
                            config.mint_authority_count);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      meshpay_currency_add_mint_authority(
                          &config,
                          authorities[MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES]));
}

TEST_CASE("currency rejects transfer with insufficient balance", "[currency]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x12);
    fill_sequence(alice, sizeof(alice), 0x42);
    fill_sequence(bob, sizeof(bob), 0x72);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    meshpay_tx_t transfer;
    make_tx(&transfer, MESHPAY_TX_TYPE_TRANSFER, 0x52,
            alice, bob, 100, 1, config.transfer_fee, config.currency_id,
            NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_INSUFFICIENT,
                      meshpay_currency_validate_tx(&config, dag, &transfer));
}

TEST_CASE("currency applies demurrage by bps ticks", "[currency]")
{
    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    config.demurrage_enabled = true;
    config.demurrage_bps = 100;

    TEST_ASSERT_EQUAL_UINT32(990,
        meshpay_currency_apply_demurrage(&config, 1000, 1));
    TEST_ASSERT_EQUAL_UINT32(980,
        meshpay_currency_apply_demurrage(&config, 1000, 2));

    config.demurrage_bps = MESHPAY_CURRENCY_BPS_SCALE;
    TEST_ASSERT_EQUAL_UINT32(0,
        meshpay_currency_apply_demurrage(&config, 1000, 1));
}

/* --- Palier A2 : dérivation de la config depuis un descripteur signé --- */

/* Remplit un corps de descripteur réaliste (founder_public rempli par sign). */
static void fill_descriptor_body(meshpay_currency_descriptor_t *body)
{
    meshpay_currency_descriptor_init(body);
    strncpy(body->name, "Minimistan", sizeof(body->name) - 1);
    strncpy(body->symbol, "MIN", sizeof(body->symbol) - 1);
    body->max_supply = 50000;
    body->transfer_fee = 3;
    body->demurrage_enabled = true;
    body->demurrage_bps = 150;
    body->initial_credit = 100;
    body->created_at_ms = 1716200000000ULL;
}

TEST_CASE("currency derives config from signed descriptor", "[currency]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body);
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));

    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    /* Règles reprises + currency_id dérivé du genesis. */
    TEST_ASSERT_EQUAL_UINT32(desc.currency_id, config.currency_id);
    TEST_ASSERT_EQUAL_UINT64(50000, config.max_supply);
    TEST_ASSERT_EQUAL_UINT32(3, config.transfer_fee);
    TEST_ASSERT_TRUE(config.demurrage_enabled);
    TEST_ASSERT_EQUAL_UINT16(150, config.demurrage_bps);
    TEST_ASSERT_TRUE(config.has_descriptor);

    /* Autorité MINT UNIQUE = hash de DESTINATION wallet du fondateur (son compte),
     * pas son hash d'identité (fix HIGH revue Palier D). */
    TEST_ASSERT_EQUAL_UINT8(1, config.mint_authority_count);
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder, &founder_wallet));
    TEST_ASSERT_TRUE(meshpay_currency_is_mint_authority(&config,
                                                        founder_wallet.hash));

    /* Clés publiques du fondateur copiées (servent à vérifier les MINT). */
    uint8_t founder_pub[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&founder, founder_pub));
    TEST_ASSERT_EQUAL_MEMORY(founder_pub, config.founder_public,
                             sizeof(founder_pub));
}

TEST_CASE("currency config_from_descriptor rejects NULL", "[currency]")
{
    meshpay_currency_config_t config;
    meshpay_currency_descriptor_signed_t desc;
    memset(&config, 0, sizeof(config));
    memset(&desc, 0, sizeof(desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_config_from_descriptor(NULL, &desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_config_from_descriptor(&config, NULL));
}

TEST_CASE("currency config_from_descriptor rejects zero founder key", "[currency]")
{
    /* Descripteur dont la clé fondateur est nulle : founder_hash échoue
     * (rns_identity_load_public rejette une clé tout-zéro). La config ne doit
     * PAS rester à moitié initialisée (has_descriptor doit rester false). */
    meshpay_currency_descriptor_signed_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.currency_id = 0x1234;

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0xAAAA);
    config.has_descriptor = false;

    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_config_from_descriptor(&config, &desc));
    TEST_ASSERT_FALSE(config.has_descriptor);
}

/* --- Palier A3 : durcissement de la validation MINT sous descripteur --- */

TEST_CASE("currency accepts founder-signed mint under descriptor", "[currency]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body);
    body.max_supply = 0; /* illimité : isole la vérif de signature */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));

    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    /* Le fondateur frappe depuis SON compte = hash de destination wallet (=
     * l'autorité MINT), pas depuis son hash d'identité (fix HIGH revue Palier D). */
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder, &founder_wallet));
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(alice, sizeof(alice), 0x40);

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    /* MINT réellement signé par le fondateur -> accepté. */
    meshpay_tx_t mint;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_tx_create_mint(&mint, &founder, founder_wallet.hash, alice, 1000, 0,
                               config.currency_id, NULL, 0, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &mint));
}

TEST_CASE("currency rejects mint forged with founder hash but wrong signature", "[currency]")
{
    rns_identity_t founder;
    rns_identity_t attacker;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&attacker));

    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body);
    body.max_supply = 0;
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));

    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    /* from = compte du fondateur (hash de destination wallet = l'autorité). */
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder, &founder_wallet));
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(alice, sizeof(alice), 0x41);

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    /* L'attaquant forge un MINT avec from = compte fondateur (public) mais le
     * signe avec SA clé : from passe is_mint_authority, mais la signature ne
     * vérifie pas contre la clé du fondateur -> rejet (la faille est fermée). */
    meshpay_tx_t forged;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_tx_create_mint(&forged, &attacker, founder_wallet.hash, alice, 1000, 0,
                               config.currency_id, NULL, 0, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
                      meshpay_currency_validate_tx(&config, dag, &forged));
}

TEST_CASE("currency without descriptor does not require mint signature", "[currency]")
{
    /* Rétro-compat : une config classique (sans descripteur) garde l'ancien
     * comportement (pas de vérif de signature), sinon les flux existants
     * casseraient. */
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x13);
    fill_sequence(alice, sizeof(alice), 0x43);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    TEST_ASSERT_FALSE(config.has_descriptor);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t *dag = test_pool_dag(0); /* DAG du pool partagé (slot 0) */

    meshpay_tx_t mint; /* signature factice, mais has_descriptor=false */
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x23,
            master, alice, 1000, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &mint));
}

/* --- Palier C2 : validation & solde du crédit initial (CLAIM) --- */

/* Une CLAIM réflexive au montant EXACT du crédit initial est acceptée, crédite
 * le membre et est comptée dans l'offre. Couvre « CLAIM crédite » + « solde
 * reflète le crédit » + « comptée dans total_minted ». */
TEST_CASE("currency accepts reflexive claim and credits initial credit", "[currency][c2]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body); /* initial_credit = 100, max_supply = 50000 */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));

    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));
    TEST_ASSERT_EQUAL_UINT32(100, config.initial_credit);

    /* Membre M (≠ fondateur), signature factice : validate_tx ne vérifie pas la
     * signature membre (comme un TRANSFER). */
    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(member, sizeof(member), 0x40);

    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x60,
            member, member, 100, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &claim));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, dag,
                                                           member, &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);

    uint64_t total = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, dag, &total));
    TEST_ASSERT_EQUAL_UINT64(100, total);
}

/* Le montant doit être EXACTEMENT initial_credit : un montant arbitraire est
 * rejeté (BAD_AMOUNT). */
TEST_CASE("currency rejects claim with wrong amount", "[currency][c2]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body); /* initial_credit = 100 */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(member, sizeof(member), 0x41);
    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t claim; /* 101 != initial_credit 100 */
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x61,
            member, member, 101, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_AMOUNT,
                      meshpay_currency_validate_tx(&config, dag, &claim));
}

/* La CLAIM est bornée par max_supply : si l'offre déjà frappée + le crédit
 * dépasse le plafond, elle est rejetée (SUPPLY_EXCEEDED). */
TEST_CASE("currency rejects claim exceeding max supply", "[currency][c2]")
{
    uint8_t founder_hash[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(founder_hash, sizeof(founder_hash), 0x10);
    fill_sequence(member, sizeof(member), 0x42);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d505632);
    config.max_supply = 150;
    config.initial_credit = 100;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, founder_hash));

    meshpay_dag_t *dag = test_pool_dag(0);

    /* MINT initial de 100 (fondateur) -> total_minted = 100. */
    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x20,
            founder_hash, member, 100, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &mint));

    /* CLAIM 100 (montant correct) mais 100 + 100 = 200 > 150 -> rejet. */
    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x62,
            member, member, 100, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_SUPPLY_EXCEEDED,
                      meshpay_currency_validate_tx(&config, dag, &claim));
}

/* Défense en profondeur (constat #1/#3 de la revue) : une CLAIM au montant FORGÉ
 * (!= initial_credit) mergée dans le DAG SANS passer par validate_tx — comme le
 * ferait le chemin de sync (apply_batch) — doit être IGNORÉE par la comptabilité
 * (solde ET total_minted), sinon inflation arbitraire à l'ingestion. */
TEST_CASE("currency balance ignores a forged claim amount", "[currency][c2]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body); /* initial_credit = 100 */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(member, sizeof(member), 0x44);
    meshpay_dag_t *dag = test_pool_dag(0);

    /* CLAIM forgée à 999 (!= 100), mergée directement (bypass validate_tx). */
    meshpay_tx_t forged;
    make_tx(&forged, MESHPAY_TX_TYPE_CLAIM, 0x64,
            member, member, 999, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &forged));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, dag,
                                                           member, &balance));
    TEST_ASSERT_EQUAL_UINT32(0, balance); /* montant forgé ignoré */

    uint64_t total = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, dag, &total));
    TEST_ASSERT_EQUAL_UINT64(0, total); /* pas d'inflation comptable */
}

/* Autorisation UNIVERSELLE : une CLAIM d'un membre qui n'est PAS le fondateur
 * est acceptée, même sous descripteur (has_descriptor=true). Prouve que la CLAIM
 * ne passe PAS par la vérif de clé fondateur (inconditionnelle pour un MINT). */
TEST_CASE("currency accepts claim from non founder without founder check", "[currency][c2]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body); /* initial_credit = 100 */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));
    TEST_ASSERT_TRUE(config.has_descriptor);

    /* Membre quelconque, distinct de l'autorité MINT, signature factice. */
    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(member, sizeof(member), 0x43);
    TEST_ASSERT_FALSE(meshpay_currency_is_mint_authority(&config, member));

    meshpay_dag_t *dag = test_pool_dag(0);

    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x63,
            member, member, 100, 0, 0, config.currency_id, NULL, 0);
    /* Un MINT ainsi forgé (from non-autorité) serait NOT_AUTHORITY ; une CLAIM
     * signée par un non-fondateur est OK (pas de vérif fondateur, signature
     * membre vérifiée à l'ingestion). */
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, dag, &claim));
}

/* --- Palier F2 : appartenance dérivée de la DAG (is_member / member_count) --- */

/* Une CLAIM valide confère l'appartenance ; une CLAIM forgée (mauvais montant)
 * non ; l'autorité MINT est membre même sans CLAIM ; un inconnu non. */
TEST_CASE("currency membership follows valid claims and mint authority",
          "[currency][f2]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body); /* initial_credit = 100 */
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, &founder));
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, &desc));

    uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t forger[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t stranger[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(member, sizeof(member), 0x40);
    fill_sequence(forger, sizeof(forger), 0x41);
    fill_sequence(stranger, sizeof(stranger), 0x42);

    meshpay_dag_t *dag = test_pool_dag(0);

    /* CLAIM valide du membre + CLAIM forgée (montant 5000 != 100) du forgeur
     * arrivée par sync (le merge ne valide pas — P0 connu). */
    meshpay_tx_t claim;
    make_tx(&claim, MESHPAY_TX_TYPE_CLAIM, 0x60,
            member, member, 100, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim));
    meshpay_tx_t forged;
    make_tx(&forged, MESHPAY_TX_TYPE_CLAIM, 0x61,
            forger, forger, 5000, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &forged));

    TEST_ASSERT_TRUE(meshpay_currency_is_member(&config, dag, member));
    TEST_ASSERT_FALSE(meshpay_currency_is_member(&config, dag, forger));
    TEST_ASSERT_FALSE(meshpay_currency_is_member(&config, dag, stranger));
    /* Le fondateur (autorité MINT = hash de sa destination wallet) est membre
     * même sans CLAIM dans la DAG. */
    TEST_ASSERT_TRUE(config.mint_authority_count > 0);
    TEST_ASSERT_TRUE(meshpay_currency_is_member(&config, dag,
                                                config.mint_authorities[0]));

    /* member_count : 1 CLAIM valide + 1 autorité sans CLAIM = 2 (la CLAIM
     * forgée ne compte pas). */
    TEST_ASSERT_EQUAL_size_t(2, meshpay_currency_member_count(&config, dag));

    /* Le fondateur réclame son crédit : sa CLAIM remplace son « siège »
     * d'autorité sans le double-compter. */
    meshpay_tx_t founder_claim;
    make_tx(&founder_claim, MESHPAY_TX_TYPE_CLAIM, 0x62,
            config.mint_authorities[0], config.mint_authorities[0],
            100, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(dag, &founder_claim));
    TEST_ASSERT_EQUAL_size_t(2, meshpay_currency_member_count(&config, dag));

    /* Arguments NULL : jamais membre, jamais de compte. */
    TEST_ASSERT_FALSE(meshpay_currency_is_member(NULL, dag, member));
    TEST_ASSERT_EQUAL_size_t(0, meshpay_currency_member_count(&config, NULL));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Durcissement ingestion (I2) — annuaire des clés + gate crypto
 * ══════════════════════════════════════════════════════════════════════════ */

/* Monte une monnaie ancrée (descripteur signé) + un membre M dont la CLAIM
 * v2 réelle (signée, clé embarquée) est déjà dans la DAG. Sort les identités
 * et les hash de compte pour forger les attaques. */
static void ingest_fixture(meshpay_currency_config_t *config,
                           meshpay_dag_t *dag,
                           rns_identity_t *founder,
                           rns_identity_t *member,
                           uint8_t member_account[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(founder));
    meshpay_currency_descriptor_t body;
    fill_descriptor_body(&body);
    meshpay_currency_descriptor_signed_t desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&desc, &body, founder));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(config, &desc));

    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(member));
    rns_destination_t member_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(member,
                                                            &member_wallet));
    memcpy(member_account, member_wallet.hash,
           MESHPAY_TX_DESTINATION_HASH_SIZE);

    /* CLAIM v2 réelle : amount == initial_credit (100), clé embarquée. */
    meshpay_tx_t claim;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_claim(&claim, member, member_account,
                                              config->initial_credit,
                                              config->currency_id, NULL, 0,
                                              1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_ingest_check(config, dag, &claim));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &claim));
}

TEST_CASE("currency member key resolves founder and claimed members",
          "[currency][i2]")
{
    meshpay_currency_config_t config;
    meshpay_dag_t *dag = test_pool_dag(0);
    rns_identity_t founder;
    rns_identity_t member;
    uint8_t member_account[MESHPAY_TX_DESTINATION_HASH_SIZE];
    ingest_fixture(&config, dag, &founder, &member, member_account);

    /* Fondateur : clé lue dans le descripteur, sous son compte wallet. */
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder,
                                                            &founder_wallet));
    uint8_t resolved[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_member_key(&config, dag,
                                                  founder_wallet.hash,
                                                  resolved));
    TEST_ASSERT_EQUAL_MEMORY(config.founder_public, resolved,
                             sizeof(resolved));

    /* Membre : clé lue dans sa CLAIM. */
    uint8_t member_pub[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&member, member_pub));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_member_key(&config, dag, member_account,
                                                  resolved));
    TEST_ASSERT_EQUAL_MEMORY(member_pub, resolved, sizeof(resolved));

    /* Compte jamais vu : NOT_FOUND (motif transitoire pour l'appelant). */
    uint8_t stranger[MESHPAY_TX_DESTINATION_HASH_SIZE];
    memset(stranger, 0x5A, sizeof(stranger));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_currency_member_key(&config, dag, stranger,
                                                  resolved));

    /* Config de repli (pas de descripteur) : pas d'annuaire. */
    meshpay_currency_config_t fallback;
    meshpay_currency_config_init(&fallback, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_currency_member_key(&fallback, dag, stranger,
                                                  resolved));
}

TEST_CASE("currency ingest check accepts genuine txs and refuses forgeries",
          "[currency][i2]")
{
    meshpay_currency_config_t config;
    meshpay_dag_t *dag = test_pool_dag(0);
    rns_identity_t founder;
    rns_identity_t member;
    uint8_t member_account[MESHPAY_TX_DESTINATION_HASH_SIZE];
    ingest_fixture(&config, dag, &founder, &member, member_account);

    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&founder,
                                                            &founder_wallet));

    /* MINT authentique du fondateur : accepté. */
    meshpay_tx_t mint;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_mint(&mint, &founder,
                                             founder_wallet.hash,
                                             member_account, 500, 7,
                                             config.currency_id, NULL, 0,
                                             2000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_ingest_check(&config, dag, &mint));

    /* MINT forgé (from = autorité publique, signature bidon) : refusé. */
    meshpay_tx_t forged_mint = mint;
    forged_mint.signature[0] ^= 0x01;
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
                      meshpay_currency_ingest_check(&config, dag,
                                                    &forged_mint));

    /* TRANSFER authentique du membre (fee = 3 du descripteur) : accepté. */
    meshpay_tx_t pay;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&pay, &member, member_account,
                                                 founder_wallet.hash, 10, 1,
                                                 config.transfer_fee,
                                                 config.currency_id, NULL, 0,
                                                 3000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_ingest_check(&config, dag, &pay));

    /* TRANSFER au fee faux : refus définitif. */
    meshpay_tx_t bad_fee;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&bad_fee, &member,
                                                 member_account,
                                                 founder_wallet.hash, 10, 2, 1,
                                                 config.currency_id, NULL, 0,
                                                 3100));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_FEE,
                      meshpay_currency_ingest_check(&config, dag, &bad_fee));

    /* TRANSFER usurpé : from = compte du membre, signé par un imposteur. */
    rns_identity_t imposter;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&imposter));
    meshpay_tx_t stolen;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&stolen, &imposter,
                                                 member_account,
                                                 founder_wallet.hash, 10, 3,
                                                 config.transfer_fee,
                                                 config.currency_id, NULL, 0,
                                                 3200));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
                      meshpay_currency_ingest_check(&config, dag, &stolen));

    /* TRANSFER d'un compte hors annuaire : motif TRANSITOIRE dédié. */
    rns_destination_t imposter_wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&imposter,
                                                            &imposter_wallet));
    meshpay_tx_t unknown;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_transfer(&unknown, &imposter,
                                                 imposter_wallet.hash,
                                                 founder_wallet.hash, 10, 1,
                                                 config.transfer_fee,
                                                 config.currency_id, NULL, 0,
                                                 3300));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_UNKNOWN_MEMBER,
                      meshpay_currency_ingest_check(&config, dag, &unknown));

    /* CLAIM au mauvais montant : refusée AVANT toute crypto. */
    meshpay_tx_t greedy;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_claim(&greedy, &imposter,
                                              imposter_wallet.hash,
                                              config.initial_credit + 1,
                                              config.currency_id, NULL, 0,
                                              3400));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_AMOUNT,
                      meshpay_currency_ingest_check(&config, dag, &greedy));

    /* CLAIM d'usurpation : l'imposteur publie SA clé sous le COMPTE du membre
     * (from == compte de M, signer == imposteur) : le lien clé<->compte casse. */
    meshpay_tx_t hijack;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_tx_create_claim(&hijack, &imposter,
                                              member_account,
                                              config.initial_credit,
                                              config.currency_id, NULL, 0,
                                              3500));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
                      meshpay_currency_ingest_check(&config, dag, &hijack));

    /* Mauvais registre : WRONG_ID avant tout le reste. */
    meshpay_tx_t alien = pay;
    alien.currency_id = config.currency_id + 1;
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_WRONG_ID,
                      meshpay_currency_ingest_check(&config, dag, &alien));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Phase B (P2) — checkpoint : coupe totale refondatrice
 * ══════════════════════════════════════════════════════════════════════════ */

TEST_CASE("checkpoint rebuild keeps balances directory and floors",
          "[currency][p2]")
{
    meshpay_currency_config_t config;
    meshpay_dag_t *dag = test_pool_dag(0);
    rns_identity_t founder;
    rns_identity_t member;
    uint8_t member_account[MESHPAY_TX_DESTINATION_HASH_SIZE];
    ingest_fixture(&config, dag, &founder, &member, member_account);
    const uint8_t *founder_account = config.mint_authorities[0];

    /* Le membre paie 10 (+fee 1) au fondateur : soldes membre 89, fondateur
     * 11 (10 + le fee). initial_credit de la fixture = 100, fee = 1. */
    meshpay_tx_t pay;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_transfer(
                                  &pay, &member, member_account,
                                  founder_account, 10, 1,
                                  config.transfer_fee, config.currency_id,
                                  NULL, 0, 2000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_ingest_check(&config, dag, &pay));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &pay));

    uint32_t member_before = 0;
    uint32_t founder_before = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(
                                  &config, dag, member_account,
                                  &member_before));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(
                                  &config, dag, founder_account,
                                  &founder_before));
    uint64_t minted_before = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, dag,
                                                            &minted_before));
    size_t members_before = meshpay_currency_member_count(&config, dag);
    size_t count_before = meshpay_dag_count(dag);
    TEST_ASSERT_TRUE(count_before >= 2);

    /* Le FONDATEUR refonde : build (générations, comptes, planchers) + sign. */
    meshpay_checkpoint_t *cp = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(cp);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_build_checkpoint(
                                  &config, dag, 5000, cp));
    TEST_ASSERT_EQUAL_UINT32(1, cp->generation);
    const meshpay_checkpoint_account_t *m_acct =
        meshpay_checkpoint_find_account(cp, member_account);
    const meshpay_checkpoint_account_t *f_acct =
        meshpay_checkpoint_find_account(cp, founder_account);
    TEST_ASSERT_NOT_NULL(m_acct);
    TEST_ASSERT_NOT_NULL(f_acct);
    TEST_ASSERT_EQUAL_UINT32(member_before, m_acct->balance);
    TEST_ASSERT_EQUAL_UINT32(founder_before, f_acct->balance);
    TEST_ASSERT_EQUAL_UINT32(1, m_acct->seq_floor); /* CLAIM seq0 + pay seq1 */
    /* Annuaire : la clé du membre survit, l'autorité reste « au descripteur ». */
    uint8_t member_public[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_identity_get_public_key(&member, member_public));
    TEST_ASSERT_EQUAL_MEMORY(member_public, m_acct->member_public,
                             RNS_IDENTITY_PUBLIC_SIZE);
    uint8_t zero_key[RNS_IDENTITY_PUBLIC_SIZE] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zero_key, f_acct->member_public,
                             RNS_IDENTITY_PUBLIC_SIZE);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(cp, &founder));

    /* ADOPTION : coupe TOTALE — la fenêtre se vide, l'état ne bouge PAS. */
    size_t purged = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_adopt_checkpoint(dag, cp, &purged));
    TEST_ASSERT_EQUAL_size_t(count_before, purged);
    TEST_ASSERT_EQUAL_size_t(0, meshpay_dag_count(dag));

    uint32_t member_after = 0;
    uint32_t founder_after = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(
                                  &config, dag, member_account, &member_after));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(
                                  &config, dag, founder_account,
                                  &founder_after));
    TEST_ASSERT_EQUAL_UINT32(member_before, member_after);
    TEST_ASSERT_EQUAL_UINT32(founder_before, founder_after);
    uint64_t minted_after = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, dag,
                                                            &minted_after));
    TEST_ASSERT_EQUAL_UINT64(minted_before, minted_after);
    TEST_ASSERT_EQUAL_size_t(members_before,
                             meshpay_currency_member_count(&config, dag));
    TEST_ASSERT_TRUE(meshpay_currency_is_member(&config, dag, member_account));
    uint8_t resolved[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_member_key(
                                  &config, dag, member_account, resolved));
    TEST_ASSERT_EQUAL_MEMORY(member_public, resolved,
                             RNS_IDENTITY_PUBLIC_SIZE);

    /* ANTI-REJEU : la CLAIM et le paiement élagués, re-livrés par un pair en
     * retard (ou rejoués), sont REFUSÉS définitivement. */
    meshpay_tx_t replay_claim;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_claim(
                                  &replay_claim, &member, member_account,
                                  config.initial_credit, config.currency_id,
                                  NULL, 0, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_REPLAY,
                      meshpay_currency_ingest_check(&config, dag,
                                                    &replay_claim));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_REPLAY,
                      meshpay_currency_ingest_check(&config, dag, &pay));

    /* La VIE CONTINUE post-horizon : un nouveau paiement (seq au-dessus du
     * plancher) passe le gate, s'applique sur une fenêtre vide (parents
     * pendants tolérés) et les soldes suivent depuis la base refondée. */
    meshpay_tx_t next;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_create_transfer(
                                  &next, &member, member_account,
                                  founder_account, 5, 2, config.transfer_fee,
                                  config.currency_id, NULL, 0, 6000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_ingest_check(&config, dag, &next));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK, meshpay_dag_merge_tx(dag, &next));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(
                                  &config, dag, member_account, &member_after));
    TEST_ASSERT_EQUAL_UINT32(member_before - 5 - config.transfer_fee,
                             member_after);

    /* Génération 2 : la refonte se compose par récurrence. */
    meshpay_checkpoint_t *cp2 = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(cp2);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_build_checkpoint(
                                  &config, dag, 7000, cp2));
    TEST_ASSERT_EQUAL_UINT32(2, cp2->generation);
    const meshpay_checkpoint_account_t *m2 =
        meshpay_checkpoint_find_account(cp2, member_account);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_UINT32(member_after, m2->balance);
    TEST_ASSERT_EQUAL_UINT32(2, m2->seq_floor);
    TEST_ASSERT_EQUAL_MEMORY(member_public, m2->member_public,
                             RNS_IDENTITY_PUBLIC_SIZE);
    free(cp2);
    free(cp);
}

TEST_CASE("checkpoint adoption is monotonic and scoped", "[currency][p2]")
{
    meshpay_currency_config_t config;
    meshpay_dag_t *dag = test_pool_dag(0);
    rns_identity_t founder;
    rns_identity_t member;
    uint8_t member_account[MESHPAY_TX_DESTINATION_HASH_SIZE];
    ingest_fixture(&config, dag, &founder, &member, member_account);

    meshpay_checkpoint_t *cp = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(cp);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_build_checkpoint(&config, dag, 100, cp));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_checkpoint_sign(cp, &founder));
    size_t purged = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_adopt_checkpoint(dag, cp, &purged));

    /* Ré-adopter la MÊME génération (ou une plus vieille) : refus monotone. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_dag_adopt_checkpoint(dag, cp, &purged));

    /* Génération 0 : jamais adoptable. */
    meshpay_checkpoint_t *bad = malloc(sizeof(meshpay_checkpoint_t));
    TEST_ASSERT_NOT_NULL(bad);
    meshpay_checkpoint_init(bad);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_dag_adopt_checkpoint(dag, bad, &purged));
    free(bad);
    free(cp);
}
