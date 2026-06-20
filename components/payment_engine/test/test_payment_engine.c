#include "meshpay/payment_engine.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_packet_crypto.h"
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

static void remember_wallet_announce(const rns_identity_t *identity,
                                     uint8_t seed)
{
    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(identity,
                                                            &destination));

    uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE];
    fill_sequence(random_hash, sizeof(random_hash), seed);
    const uint8_t app_data[] = "known-sender";

    rns_packet_t announce;
    rns_packet_clear(&announce);
    announce.destination_type = destination.type;
    announce.packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(announce.destination_hash, destination.hash,
           sizeof(announce.destination_hash));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_announce_encode(&destination,
                                          identity,
                                          random_hash,
                                          app_data,
                                          sizeof(app_data) - 1,
                                          announce.data,
                                          sizeof(announce.data),
                                          &announce.data_len));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_announce_verify_and_remember(&announce, NULL));
}

static void make_mint(meshpay_tx_t *tx,
                      uint8_t id_seed,
                      const uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE],
                      const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                      uint32_t amount,
                      uint32_t currency_id)
{
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_MINT;
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    memcpy(tx->from, master, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = 0;
    tx->fee = 0;
    tx->currency_id = currency_id;
    tx->timestamp_ms = 1;
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x40));
}

TEST_CASE("payment engine sends tx and accepts ack in memory", "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d505632);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag_a;
    meshpay_dag_t dag_b;
    meshpay_dag_init(&dag_a);
    meshpay_dag_init(&dag_b);
    meshpay_tx_t mint;
    make_mint(&mint, 0x21, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_a, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_b, &mint));

    meshpay_wallet_t wallet_a;
    meshpay_wallet_t wallet_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_a, alice, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_b, bob, 1));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x01);
    load_identity(&identity_b, 0x31);

    meshpay_payment_engine_t engine_a;
    meshpay_payment_engine_t engine_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_a, &wallet_a,
                                                  &dag_a, &config,
                                                  &identity_a));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_b, &wallet_b,
                                                  &dag_b, &config,
                                                  &identity_b));

    rns_packet_t payment_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(&engine_a, bob,
                                                            100, 1000,
                                                            &payment_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_SENT, engine_a.feedback);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_TX, payment_packet.data[0]);
    TEST_ASSERT_EQUAL_MEMORY(bob, payment_packet.destination_hash,
                             MESHPAY_TX_DESTINATION_HASH_SIZE);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet_a, &config,
                                                           &dag_a, 2000,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(895, balance);

    rns_packet_t ack_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_receive_payment(&engine_b,
                                                             &payment_packet,
                                                             2000,
                                                             &ack_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_RECEIVED, engine_b.feedback);
    TEST_ASSERT_TRUE(engine_b.has_last_received);
    TEST_ASSERT_EQUAL_UINT32(100, engine_b.last_received_tx.amount);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_ACK, ack_packet.data[0]);
    TEST_ASSERT_EQUAL_MEMORY(alice, ack_packet.destination_hash,
                             MESHPAY_TX_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&dag_b));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag_b,
                                                           bob, &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_receive_ack(&engine_a,
                                                         &ack_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_ACKED, engine_a.feedback);
    TEST_ASSERT_FALSE(engine_a.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&wallet_a, 3000));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&dag_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag_a,
                                                           alice, &balance));
    TEST_ASSERT_EQUAL_UINT32(895, balance);
}

TEST_CASE("payment engine reject restores pending payer balance",
          "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x13);
    fill_sequence(alice, sizeof(alice), 0x43);
    fill_sequence(bob, sizeof(bob), 0x73);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d505633);
    config.transfer_fee = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag_a;
    meshpay_dag_t dag_b;
    meshpay_dag_init(&dag_a);
    meshpay_dag_init(&dag_b);

    meshpay_tx_t mint_alice;
    make_mint(&mint_alice, 0x24, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_a, &mint_alice));

    meshpay_wallet_t wallet_a;
    meshpay_wallet_t wallet_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_a, alice, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_b, bob, 1));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x03);
    load_identity(&identity_b, 0x33);

    meshpay_payment_engine_t engine_a;
    meshpay_payment_engine_t engine_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_a, &wallet_a,
                                                  &dag_a, &config,
                                                  &identity_a));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_b, &wallet_b,
                                                  &dag_b, &config,
                                                  &identity_b));

    rns_packet_t payment_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(&engine_a, bob,
                                                            100, 1000,
                                                            &payment_packet));
    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet_a, &config,
                                                           &dag_a, 1001,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(900, balance);

    rns_packet_t reject_packet;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_payment_engine_receive_payment(&engine_b,
                                                             &payment_packet,
                                                             1002,
                                                             &reject_packet));
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_REJECT, reject_packet.data[0]);
    TEST_ASSERT_EQUAL_MEMORY(alice, reject_packet.destination_hash,
                             MESHPAY_TX_DESTINATION_HASH_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(engine_a.pending_tx.id,
                             reject_packet.data + 1,
                             MESHPAY_TX_ID_SIZE);
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_dag_count(&dag_b));

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_receive_ack(&engine_a,
                                                         &reject_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_REJECTED, engine_a.feedback);
    TEST_ASSERT_FALSE(engine_a.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&wallet_a, 1003));
    TEST_ASSERT_EQUAL_UINT32(1, wallet_a.next_seq);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_get_available_balance(&wallet_a, &config,
                                                           &dag_a, 1003,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(1000, balance);
}

TEST_CASE("payment engine rejects altered tx from announced sender",
          "[payment_engine]")
{
    rns_announce_known_reset();

    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x11);

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x02);
    load_identity(&identity_b, 0x32);

    rns_destination_t destination_a;
    rns_destination_t destination_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&identity_a,
                                                            &destination_a));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&identity_b,
                                                            &destination_b));

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d31);
    config.transfer_fee = 1;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag_a;
    meshpay_dag_t dag_b;
    meshpay_dag_init(&dag_a);
    meshpay_dag_init(&dag_b);
    meshpay_tx_t mint;
    make_mint(&mint, 0x22, master, destination_a.hash, 500,
              config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_a, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_b, &mint));

    meshpay_wallet_t wallet_a;
    meshpay_wallet_t wallet_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_init(&wallet_a,
                                          destination_a.hash,
                                          1));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_wallet_init(&wallet_b,
                                          destination_b.hash,
                                          1));

    meshpay_payment_engine_t engine_a;
    meshpay_payment_engine_t engine_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_a, &wallet_a,
                                                  &dag_a, &config,
                                                  &identity_a));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_b, &wallet_b,
                                                  &dag_b, &config,
                                                  &identity_b));
    remember_wallet_announce(&identity_a, 0x91);

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(
                          &engine_a,
                          destination_b.hash,
                          80,
                          1000,
                          &packet));

    meshpay_tx_t altered;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_decode(packet.data + 1,
                                                packet.data_len - 1,
                                                &altered));
    altered.amount = 81;
    uint8_t encoded[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t encoded_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_tx_encode(&altered,
                                                encoded,
                                                sizeof(encoded),
                                                &encoded_len));
    packet.data[0] = MESHPAY_PAYMENT_MSG_TX;
    memcpy(packet.data + 1, encoded, encoded_len);
    packet.data_len = encoded_len + 1U;

    rns_packet_t ack;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_payment_engine_receive_payment(&engine_b,
                                                             &packet,
                                                             2000,
                                                             &ack));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_REJECTED, engine_b.feedback);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(&dag_b));

    rns_announce_known_reset();
}

TEST_CASE("payment engine rejects second local payment while pending", "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x12);
    fill_sequence(alice, sizeof(alice), 0x42);
    fill_sequence(bob, sizeof(bob), 0x72);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    config.transfer_fee = 1;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);
    meshpay_tx_t mint;
    make_mint(&mint, 0x23, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &mint));

    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, alice, 1));
    rns_identity_t identity;
    load_identity(&identity, 0x55);

    meshpay_payment_engine_t engine;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine, &wallet, &dag,
                                                  &config, &identity));

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(&engine, bob,
                                                            50, 1000,
                                                            &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_payment_engine_create_payment(&engine, bob,
                                                            50, 2000,
                                                            &packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_REJECTED, engine.feedback);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_cancel_pending(&engine));
    TEST_ASSERT_FALSE(engine.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&wallet, 2001));
    TEST_ASSERT_EQUAL_UINT32(1, wallet.next_seq);
}

TEST_CASE("payment engine expires pending payment after wallet timeout",
          "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x18);
    fill_sequence(alice, sizeof(alice), 0x48);
    fill_sequence(bob, sizeof(bob), 0x78);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d34);
    config.transfer_fee = 1;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);
    meshpay_tx_t mint;
    make_mint(&mint, 0x28, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &mint));

    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, alice, 1));
    rns_identity_t identity;
    load_identity(&identity, 0x58);

    meshpay_payment_engine_t engine;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine, &wallet, &dag,
                                                  &config, &identity));

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(&engine, bob,
                                                            50, 1000,
                                                            &packet));
    TEST_ASSERT_TRUE(engine.has_pending);
    TEST_ASSERT_EQUAL_UINT32(1, engine.pending_tx.seq);
    TEST_ASSERT_EQUAL_UINT32(2, wallet.next_seq);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_payment_engine_create_payment(
                          &engine, bob, 60,
                          1000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS - 1,
                          &packet));
    TEST_ASSERT_TRUE(engine.has_pending);
    TEST_ASSERT_EQUAL_UINT32(2, wallet.next_seq);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_payment(
                          &engine, bob, 60,
                          1000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS,
                          &packet));
    TEST_ASSERT_TRUE(engine.has_pending);
    TEST_ASSERT_EQUAL_UINT32(2, engine.pending_tx.seq);
    TEST_ASSERT_EQUAL_UINT32(3, wallet.next_seq);
    TEST_ASSERT_TRUE(meshpay_wallet_lock_active(
        &wallet, 1000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS + 1));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_SENT, engine.feedback);
}

TEST_CASE("payment engine sends encrypted tx and receiver decrypts it",
          "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x16);
    fill_sequence(alice, sizeof(alice), 0x46);
    fill_sequence(bob, sizeof(bob), 0x76);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d32);
    config.transfer_fee = 2;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag_a;
    meshpay_dag_t dag_b;
    meshpay_dag_init(&dag_a);
    meshpay_dag_init(&dag_b);
    meshpay_tx_t mint;
    make_mint(&mint, 0x26, master, alice, 600, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_a, &mint));
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag_b, &mint));

    meshpay_wallet_t wallet_a;
    meshpay_wallet_t wallet_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_a, alice, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet_b, bob, 1));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x06);
    load_identity(&identity_b, 0x36);

    meshpay_payment_engine_t engine_a;
    meshpay_payment_engine_t engine_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_a, &wallet_a,
                                                  &dag_a, &config,
                                                  &identity_a));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine_b, &wallet_b,
                                                  &dag_b, &config,
                                                  &identity_b));

    rns_packet_t payment_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_create_encrypted_payment(
                          &engine_a, bob, &identity_b, 75, 1000,
                          &payment_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_SENT, engine_a.feedback);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(RNS_PACKET_CRYPTO_MIN_TOKEN_SIZE,
                                        payment_packet.data_len);
    TEST_ASSERT_EQUAL_MEMORY(bob, payment_packet.destination_hash,
                             MESHPAY_TX_DESTINATION_HASH_SIZE);

    rns_packet_t ack_packet;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_receive_payment(&engine_b,
                                                             &payment_packet,
                                                             2000,
                                                             &ack_packet));
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_RECEIVED, engine_b.feedback);
    TEST_ASSERT_TRUE(engine_b.has_last_received);
    TEST_ASSERT_EQUAL_UINT32(75, engine_b.last_received_tx.amount);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_ACK, ack_packet.data[0]);
    TEST_ASSERT_EQUAL_MEMORY(alice, ack_packet.destination_hash,
                             MESHPAY_TX_DESTINATION_HASH_SIZE);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config, &dag_b,
                                                           bob, &balance));
    TEST_ASSERT_EQUAL_UINT32(75, balance);
}

TEST_CASE("payment engine preserves sequence when payment is rejected",
          "[payment_engine]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x17);
    fill_sequence(alice, sizeof(alice), 0x47);
    fill_sequence(bob, sizeof(bob), 0x77);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 0x4d33);
    config.transfer_fee = 1;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    meshpay_dag_t dag;
    meshpay_dag_init(&dag);
    meshpay_tx_t mint;
    make_mint(&mint, 0x27, master, alice, 20, config.currency_id);
    TEST_ASSERT_EQUAL(MESHPAY_DAG_MERGE_OK,
                      meshpay_dag_merge_tx(&dag, &mint));

    meshpay_wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_init(&wallet, alice, 7));

    rns_identity_t identity;
    load_identity(&identity, 0x07);

    meshpay_payment_engine_t engine;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_payment_engine_init(&engine,
                                                  &wallet,
                                                  &dag,
                                                  &config,
                                                  &identity));

    rns_packet_t packet;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_payment_engine_create_payment(&engine,
                                                            bob,
                                                            50,
                                                            1000,
                                                            &packet));
    TEST_ASSERT_FALSE(engine.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&wallet, 1001));
    TEST_ASSERT_EQUAL_UINT32(7, wallet.next_seq);
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_FEEDBACK_REJECTED, engine.feedback);
}
