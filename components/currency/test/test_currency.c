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
