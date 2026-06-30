#include "meshpay/currency.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_identity.h"
#include "unity.h"
#include <string.h>

/* DAG de test PARTAGÉE (une seule instance, file-scope) : meshpay_dag_t fait
 * ~57 Ko (fenêtre de 250 TX). La placer sur la pile déborderait la pile du main
 * (corruption mémoire) ; une instance static PAR fonction déborderait la .bss
 * DRAM. Une seule instance, réinitialisée via meshpay_dag_init() au début de
 * chaque test qui l'utilise. */
static meshpay_dag_t dag;

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

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x20,
            master, alice, 1000, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, &dag, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &mint));

    uint8_t parents[1][MESHPAY_TX_PARENT_ID_SIZE];
    memcpy(parents[0], mint.id, MESHPAY_TX_PARENT_ID_SIZE);
    meshpay_tx_t transfer;
    make_tx(&transfer, MESHPAY_TX_TYPE_TRANSFER, 0x50,
            alice, bob, 100, 1, config.transfer_fee, config.currency_id,
            parents, 1);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, &dag, &transfer));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &transfer));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag,
                                                           alice, &balance));
    TEST_ASSERT_EQUAL_UINT32(893, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag,
                                                           bob, &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag,
                                                           master, &balance));
    TEST_ASSERT_EQUAL_UINT32(7, balance);

    uint64_t total_minted = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_total_minted(&config, &dag,
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

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x22,
            impostor, alice, 1000, 0, 0, config.currency_id, NULL, 0);

    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_NOT_AUTHORITY,
                      meshpay_currency_validate_tx(&config, &dag, &mint));
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

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    meshpay_tx_t transfer;
    make_tx(&transfer, MESHPAY_TX_TYPE_TRANSFER, 0x52,
            alice, bob, 100, 1, config.transfer_fee, config.currency_id,
            NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_INSUFFICIENT,
                      meshpay_currency_validate_tx(&config, &dag, &transfer));
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

    /* Autorité MINT UNIQUE = hash d'identité du fondateur. */
    TEST_ASSERT_EQUAL_UINT8(1, config.mint_authority_count);
    uint8_t founder_hash[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, founder_hash));
    TEST_ASSERT_TRUE(meshpay_currency_is_mint_authority(&config, founder_hash));

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

    uint8_t founder_hash[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, founder_hash));
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(alice, sizeof(alice), 0x40);

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    /* MINT réellement signé par le fondateur -> accepté. */
    meshpay_tx_t mint;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_tx_create_mint(&mint, &founder, founder_hash, alice, 1000, 0,
                               config.currency_id, NULL, 0, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, &dag, &mint));
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

    uint8_t founder_hash[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, founder_hash));
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(alice, sizeof(alice), 0x41);

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    /* L'attaquant forge un MINT avec from = hash fondateur (public) mais le
     * signe avec SA clé : from passe is_mint_authority, mais la signature ne
     * vérifie pas contre la clé du fondateur -> rejet (la faille est fermée). */
    meshpay_tx_t forged;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_tx_create_mint(&forged, &attacker, founder_hash, alice, 1000, 0,
                               config.currency_id, NULL, 0, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
                      meshpay_currency_validate_tx(&config, &dag, &forged));
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

    meshpay_dag_init(&dag); /* DAG partagée (file-scope), réinitialisée ici */

    meshpay_tx_t mint; /* signature factice, mais has_descriptor=false */
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x23,
            master, alice, 1000, 0, 0, config.currency_id, NULL, 0);
    TEST_ASSERT_EQUAL(MESHPAY_CURRENCY_OK,
                      meshpay_currency_validate_tx(&config, &dag, &mint));
}
