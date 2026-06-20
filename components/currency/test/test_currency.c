#include "meshpay/currency.h"
#include "unity.h"
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

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);

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

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);

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

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);

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
