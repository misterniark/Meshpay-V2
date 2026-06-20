#include "meshpay/wallet.h"
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
                    uint32_t currency_id)
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
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x40));
}

TEST_CASE("wallet reports available balance with local lock", "[wallet]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);
    meshpay_tx_t mint;
    make_tx(&mint, MESHPAY_TX_TYPE_MINT, 0x20,
            master, alice, 1000, 0, 0, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &mint));

    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, alice, 1));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet, &config,
                                                           &dag, 1000,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(1000, balance);

    uint8_t tx_id[MESHPAY_TX_ID_SIZE];
    fill_sequence(tx_id, sizeof(tx_id), 0x80);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_lock(&wallet, tx_id,
                                                  205, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet, &config,
                                                           &dag, 2000,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(795, balance);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_unlock(&wallet, tx_id));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet, &config,
                                                           &dag, 3000,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(1000, balance);
}

TEST_CASE("wallet lock prevents local double spend until timeout", "[wallet]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x33);
    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, owner, 9));

    uint8_t tx_a[MESHPAY_TX_ID_SIZE];
    uint8_t tx_b[MESHPAY_TX_ID_SIZE];
    fill_sequence(tx_a, sizeof(tx_a), 0x01);
    fill_sequence(tx_b, sizeof(tx_b), 0x55);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_lock(&wallet, tx_a, 100, 1000));
    TEST_ASSERT_TRUE(meshpay_wallet_lock_active(&wallet, 999));
    TEST_ASSERT_TRUE(meshpay_wallet_lock_active(&wallet, 2000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_wallet_lock(&wallet, tx_b, 50, 2000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_wallet_lock(&wallet, tx_b, 50, 999));

    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(
        &wallet, 1000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_lock(&wallet, tx_b, 50,
                                          1000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS));
}

TEST_CASE("wallet allocates monotonic sequence", "[wallet]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x44);
    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, owner, 41));

    uint32_t seq = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_allocate_seq(&wallet, &seq));
    TEST_ASSERT_EQUAL_UINT32(41, seq);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_allocate_seq(&wallet, &seq));
    TEST_ASSERT_EQUAL_UINT32(42, seq);
}

TEST_CASE("wallet pin policy locks after repeated failures", "[wallet]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x45);
    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, owner, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_set_pin(&wallet, "1234", 4));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      meshpay_wallet_verify_pin(&wallet, "0000", 4));
    TEST_ASSERT_EQUAL_UINT8(1, wallet.pin_failures);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      meshpay_wallet_verify_pin(&wallet, "1111", 4));
    TEST_ASSERT_EQUAL_UINT8(2, wallet.pin_failures);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      meshpay_wallet_verify_pin(&wallet, "2222", 4));
    TEST_ASSERT_TRUE(wallet.pin_locked);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_wallet_verify_pin(&wallet, "1234", 4));
}

TEST_CASE("wallet pin success resets failures", "[wallet]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x46);
    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, owner, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_set_pin(&wallet, "9876", 4));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      meshpay_wallet_verify_pin(&wallet, "0000", 4));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_verify_pin(&wallet, "9876", 4));
    TEST_ASSERT_EQUAL_UINT8(0, wallet.pin_failures);
}

TEST_CASE("wallet loads persisted pin hash", "[wallet]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x47);

    meshpay_wallet_t original;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&original, owner, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_set_pin(&original, "2468", 4));

    meshpay_wallet_t restored;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&restored, owner, 1));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_load_pin_hash(&restored,
                                                   original.pin_hash));
    TEST_ASSERT_TRUE(restored.has_pin);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_verify_pin(&restored,
                                                        "2468", 4));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      meshpay_wallet_verify_pin(&restored,
                                                "1357", 4));
}
