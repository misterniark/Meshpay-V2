#include "meshpay/app_main_logic.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/descriptor_sync.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include "meshpay/rns/rns_node.h"
#include "meshpay/rns/rns_packet_crypto.h"
#include "meshpay/wallet.h"
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

static void load_identity(rns_identity_t *identity, uint8_t seed_base)
{
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(private_key, sizeof(private_key), seed_base);
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_load_private(identity, private_key));
}

static void make_mint(meshpay_tx_t *tx,
                      const uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE],
                      const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                      uint32_t amount,
                      uint32_t currency_id)
{
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_MINT;
    fill_sequence(tx->id, sizeof(tx->id), 0x22);
    memcpy(tx->from, master, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->currency_id = currency_id;
    fill_sequence(tx->signature, sizeof(tx->signature), 0x66);
}

static void make_transfer(meshpay_tx_t *tx,
                          uint8_t id_seed,
                          const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                          const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                          uint32_t amount,
                          uint32_t seq,
                          const uint8_t parent[MESHPAY_TX_PARENT_ID_SIZE])
{
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_TRANSFER;
    fill_sequence(tx->id, sizeof(tx->id), id_seed);
    memcpy(tx->from, from, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = 1;
    tx->currency_id = 1;
    tx->timestamp_ms = 2000 + seq;
    tx->parent_count = 1;
    memcpy(tx->parents[0], parent, MESHPAY_TX_PARENT_ID_SIZE);
    fill_sequence(tx->signature, sizeof(tx->signature), (uint8_t)(id_seed + 0x20));
}

static void build_wallet_announce_packet(const rns_identity_t *identity,
                                         uint8_t seed,
                                         rns_packet_t *announce)
{
    rns_destination_t destination;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(identity,
                                                            &destination));

    uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE];
    fill_sequence(random_hash, sizeof(random_hash), seed);
    const uint8_t app_data[] = "test-peer";

    rns_packet_clear(announce);
    announce->destination_type = destination.type;
    announce->packet_type = RNS_PACKET_TYPE_ANNOUNCE;
    memcpy(announce->destination_hash, destination.hash,
           sizeof(announce->destination_hash));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_announce_encode(&destination,
                                          identity,
                                          random_hash,
                                          app_data,
                                          sizeof(app_data) - 1,
                                          announce->data,
                                          sizeof(announce->data),
                                          &announce->data_len));
}

static void remember_announced_wallet(const rns_identity_t *identity,
                                      uint8_t seed)
{
    rns_packet_t announce;
    build_wallet_announce_packet(identity, seed, &announce);
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_announce_verify_and_remember(&announce, NULL));
}

typedef struct {
    uint32_t count;
    rns_packet_t last_packet;
} packet_tx_probe_t;

typedef struct {
    uint32_t count;
    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS + 1];
} packet_list_probe_t;

static esp_err_t packet_tx_probe_cb(const rns_packet_t *packet, void *ctx)
{
    packet_tx_probe_t *probe = (packet_tx_probe_t *)ctx;
    probe->count++;
    probe->last_packet = *packet;
    return ESP_OK;
}

static esp_err_t packet_list_probe_cb(const rns_packet_t *packet, void *ctx)
{
    packet_list_probe_t *probe = (packet_list_probe_t *)ctx;
    if (probe->count >= sizeof(probe->packets) / sizeof(probe->packets[0])) {
        return ESP_ERR_NO_MEM;
    }
    probe->packets[probe->count++] = *packet;
    return ESP_OK;
}

static esp_err_t failing_storage_write(void *ctx,
                                       const char *key,
                                       const void *data,
                                       size_t len)
{
    (void)ctx;
    (void)key;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static esp_err_t fixed_alias_rng(void *ctx, uint8_t *out, size_t len)
{
    const uint8_t *seed = (const uint8_t *)ctx;
    if (out == NULL || seed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = seed[i % 2U];
    }
    return ESP_OK;
}

typedef struct {
    rns_node_t *peer;
    meshpay_app_runtime_t *runtime;
} node_bridge_t;

static esp_err_t runtime_node_packet_tx(const rns_packet_t *packet, void *ctx)
{
    rns_node_t *node = (rns_node_t *)ctx;
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return rns_node_send_packet(node, packet);
}

static esp_err_t bridge_node_tx(rns_node_t *node,
                                const rns_packet_t *packet,
                                void *ctx)
{
    (void)node;
    node_bridge_t *bridge = (node_bridge_t *)ctx;
    if (bridge == NULL || bridge->peer == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rns_transport_rx_result_t result = RNS_TRANSPORT_RX_ACCEPTED;
    return rns_node_receive_packet(bridge->peer, packet, &result);
}

static esp_err_t bridge_node_rx(rns_node_t *node,
                                const rns_packet_t *packet,
                                void *ctx)
{
    (void)node;
    node_bridge_t *bridge = (node_bridge_t *)ctx;
    if (bridge == NULL || bridge->runtime == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const meshpay_app_event_t event = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 1000,
        .packet = *packet,
    };
    return meshpay_app_runtime_post(bridge->runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM,
                                    &event,
                                    0);
}

static void process_reticulum_until_idle(meshpay_app_runtime_t *runtime)
{
    while (meshpay_app_runtime_queue_depth(runtime,
                                           MESHPAY_APP_QUEUE_RETICULUM) > 0) {
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                      runtime,
                                      MESHPAY_APP_QUEUE_RETICULUM,
                                      0));
    }
}

TEST_CASE("app main simulated boot announce and payment", "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x10);
    fill_sequence(alice, sizeof(alice), 0x40);
    fill_sequence(bob, sizeof(bob), 0x70);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x01);
    load_identity(&identity_b, 0x21);

    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_b, bob, &identity_b,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HOME, app_a->ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_APP_UI_TASK_NAME[0], 'u');
    TEST_ASSERT_EQUAL(MESHPAY_APP_CORE_TASK_NAME[0], 'c');

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_b, &mint));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_announce(app_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_announce(app_b));
    TEST_ASSERT_TRUE(app_a->announced);
    TEST_ASSERT_TRUE(app_b->announced);
    TEST_ASSERT_EQUAL_UINT8(0, app_a->ui.network_peers);
    TEST_ASSERT_EQUAL_UINT8(0, app_b->ui.network_peers);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_pay(app_a, app_b, 100, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED,
                      app_a->ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HISTORY, app_a->ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b->ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_RECEIVE, app_b->ui.screen);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&app_a->currency,
                                                           &app_a->dag,
                                                           alice,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(895, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&app_b->currency,
                                                           &app_b->dag,
                                                           bob,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&app_a->dag));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&app_b->dag));
}

TEST_CASE("app runtime creates queues and non recursive mutex", "[app_main]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x31);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 7);

    rns_identity_t identity;
    load_identity(&identity, 0x71);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, owner, &identity,
                                               &config, 1, true));

    meshpay_app_runtime_config_t runtime_config =
        meshpay_app_runtime_default_config();
    runtime_config.ui_queue_length = 2;
    runtime_config.reticulum_queue_length = 2;
    runtime_config.core_queue_length = 2;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app,
                                                       &runtime_config));
    TEST_ASSERT_NOT_NULL(runtime.ui_queue);
    TEST_ASSERT_NOT_NULL(runtime.reticulum_queue);
    TEST_ASSERT_NOT_NULL(runtime.core_queue);
    TEST_ASSERT_NOT_NULL(runtime.lock);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(runtime.lock, 0));
    TEST_ASSERT_EQUAL(pdFALSE, xSemaphoreTake(runtime.lock, 0));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(runtime.lock));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime learns peer from announce packet", "[app_main]")
{
    rns_announce_known_reset();

    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(alice, sizeof(alice), 0x34);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 8);

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x29);
    load_identity(&identity_b, 0x69);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL_UINT8(0, app->ui.network_peers);

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));

    rns_packet_t announce;
    build_wallet_announce_packet(&identity_b, 0xa0, &announce);
    const meshpay_app_event_t rx = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 1200,
        .packet = announce,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &rx, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, rns_announce_known_count());
    TEST_ASSERT_EQUAL_UINT8(1, app->ui.network_peers);
    TEST_ASSERT_EQUAL_UINT8(1, app->ui.payment_peer_count);
    TEST_ASSERT_EQUAL_STRING("test-peer", app->ui.payment_peer_label);
    TEST_ASSERT_EQUAL_UINT32(1, runtime.processed_reticulum);

    meshpay_app_runtime_destroy(&runtime);
    rns_announce_known_reset();
}

TEST_CASE("app runtime processes core ui and reticulum queues", "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x12);
    fill_sequence(alice, sizeof(alice), 0x42);
    fill_sequence(bob, sizeof(bob), 0x72);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 9);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity;
    load_identity(&identity, 0x33);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    packet_tx_probe_t tx_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime,
                                  packet_tx_probe_cb,
                                  &tx_probe));

    const meshpay_app_event_t announce = {
        .type = MESHPAY_APP_EVENT_CORE_ANNOUNCE,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime, MESHPAY_APP_QUEUE_CORE,
                                  &announce, 0));
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_app_runtime_queue_depth(
                                    &runtime, MESHPAY_APP_QUEUE_CORE));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime, MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_TRUE(app->announced);
    TEST_ASSERT_EQUAL_UINT32(1, runtime.processed_core);

    const meshpay_app_event_t refresh = {
        .type = MESHPAY_APP_EVENT_UI_REFRESH,
        .now_ms = 1000,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime, MESHPAY_APP_QUEUE_UI,
                                  &refresh, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime, MESHPAY_APP_QUEUE_UI, 0));
    TEST_ASSERT_EQUAL_UINT32(1000, app->ui.balance);
    TEST_ASSERT_EQUAL_UINT32(1, runtime.processed_ui);

    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 100,
        .now_ms = 2000,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime, MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime, MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(895, app->ui.balance);
    TEST_ASSERT_EQUAL_UINT32(2, runtime.processed_core);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_app_runtime_queue_depth(
                                    &runtime, MESHPAY_APP_QUEUE_RETICULUM));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime, MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, runtime.processed_reticulum);
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_DATA, tx_probe.last_packet.packet_type);
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_MSG_TX, tx_probe.last_packet.data[0]);
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_app_runtime_queue_depth(
                                    &runtime, MESHPAY_APP_QUEUE_RETICULUM));

    rns_packet_t ack_packet;
    rns_packet_clear(&ack_packet);
    ack_packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    ack_packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    ack_packet.packet_type = RNS_PACKET_TYPE_PROOF;
    memcpy(ack_packet.destination_hash, alice,
           sizeof(ack_packet.destination_hash));
    ack_packet.data[0] = MESHPAY_PAYMENT_MSG_ACK;
    memcpy(ack_packet.data + 1, app->payments.pending_tx.id,
           MESHPAY_TX_ID_SIZE);
    ack_packet.data_len = 1U + MESHPAY_TX_ID_SIZE;

    const meshpay_app_event_t ack = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 2001,
        .packet = ack_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime, MESHPAY_APP_QUEUE_RETICULUM,
                                  &ack, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime, MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED,
                      app->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(100, app->ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(895, app->ui.balance);
    TEST_ASSERT_EQUAL_STRING("pair 7273", app->ui.last_peer_label);
    TEST_ASSERT_FALSE(app->payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(2, runtime.processed_reticulum);

    const meshpay_app_event_t dag_summary = {
        .type = MESHPAY_APP_EVENT_CORE_DAG_SUMMARY,
        .now_ms = 3000,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &dag_summary, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_EQUAL_UINT32(3, runtime.processed_core);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_app_runtime_queue_depth(
                                    &runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(2, tx_probe.count);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_PLAIN,
                      tx_probe.last_packet.destination_type);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_DAG_SYNC_MSG_SUMMARY,
                            tx_probe.last_packet.data[0]);
    TEST_ASSERT_EQUAL_UINT32(3, runtime.processed_reticulum);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime persists wallet sequence after creating payment",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x14);
    fill_sequence(alice, sizeof(alice), 0x44);
    fill_sequence(bob, sizeof(bob), 0x74);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 13);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity;
    load_identity(&identity, 0x37);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&identity,
                                                           private_key));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record,
                                                                  private_key));
    record.next_seq = app->wallet.next_seq;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_storage(&runtime,
                                                              &backend,
                                                              &record));
    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 50,
        .now_ms = 2100,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_TRUE(app->payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(2, app->wallet.next_seq);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);

    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_EQUAL_UINT32(2, loaded.next_seq);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime allows a second committed payment",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x16);
    fill_sequence(alice, sizeof(alice), 0x46);
    fill_sequence(bob, sizeof(bob), 0x76);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 16);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity;
    load_identity(&identity, 0x39);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 100, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    packet_tx_probe_t tx_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime,
                                  packet_tx_probe_cb,
                                  &tx_probe));

    meshpay_app_event_t first_payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 40,
        .now_ms = 1000,
    };
    memcpy(first_payment.destination, bob, sizeof(first_payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &first_payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_TRUE(app->payments.has_pending);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(60, app->ui.balance);
    uint8_t pending_id[MESHPAY_TX_ID_SIZE];
    memcpy(pending_id, app->payments.pending_tx.id, sizeof(pending_id));

    /* Option A : un 2e paiement n'est plus bloqué — il est committé lui aussi,
     * indépendamment du premier (le suivi de reçu pointe désormais la 2e tx). */
    meshpay_app_event_t second_payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 20,
        .now_ms = 1001,
    };
    memcpy(second_payment.destination, bob, sizeof(second_payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &second_payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_TRUE(app->payments.has_pending);
    TEST_ASSERT_TRUE(memcmp(pending_id, app->payments.pending_tx.id,
                            sizeof(pending_id)) != 0);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app->ui.feedback);
    /* 100 - 40 - 20 = 40 : les deux paiements ont débité le solde. */
    TEST_ASSERT_EQUAL_UINT32(40, app->ui.balance);
    TEST_ASSERT_EQUAL_UINT32(3, app->wallet.next_seq);
    /* Les deux tx sont en file d'émission directe. */
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_app_runtime_queue_depth(
                                    &runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM));
    uint8_t pending_id2[MESHPAY_TX_ID_SIZE];
    memcpy(pending_id2, app->payments.pending_tx.id, sizeof(pending_id2));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(2, tx_probe.count);

    /* L'ACK de la 2e tx ne fait que confirmer le reçu (déjà committée). */
    rns_packet_t ack_packet;
    rns_packet_clear(&ack_packet);
    ack_packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    ack_packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    ack_packet.packet_type = RNS_PACKET_TYPE_DATA;
    memcpy(ack_packet.destination_hash, alice,
           sizeof(ack_packet.destination_hash));
    ack_packet.data[0] = MESHPAY_PAYMENT_MSG_ACK;
    memcpy(ack_packet.data + 1, pending_id2, MESHPAY_TX_ID_SIZE);
    ack_packet.data_len = 1U + MESHPAY_TX_ID_SIZE;

    const meshpay_app_event_t ack = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 1003,
        .packet = ack_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &ack, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_FALSE(app->payments.has_pending);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED, app->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(20, app->ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(40, app->ui.balance);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime cancels payment when sequence persistence fails",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x15);
    fill_sequence(alice, sizeof(alice), 0x45);
    fill_sequence(bob, sizeof(bob), 0x75);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 14);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity;
    load_identity(&identity, 0x38);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    meshpay_storage_backend_t backend = {
        .write_blob = failing_storage_write,
    };
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&identity,
                                                           private_key));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record,
                                                                  private_key));
    record.next_seq = app->wallet.next_seq;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_storage(&runtime,
                                                              &backend,
                                                              &record));
    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 50,
        .now_ms = 2200,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_FAIL, meshpay_app_runtime_process_one(
                                    &runtime,
                                    MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_FALSE(app->payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app->wallet, 2201));
    TEST_ASSERT_EQUAL_UINT32(1, app->wallet.next_seq);
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_app_runtime_queue_depth(
                                    &runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime accepts incoming payment packet", "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x18);
    fill_sequence(alice, sizeof(alice), 0x48);
    fill_sequence(bob, sizeof(bob), 0x78);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 11);
    config.transfer_fee = 5;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x13);
    load_identity(&identity_b, 0x53);

    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_b, &mint));

    rns_packet_t payment_packet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_payment_engine_create_payment(
                                  &app_a->payments, bob, 100, 1000,
                                  &payment_packet));

    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       app_b, NULL));
    packet_tx_probe_t tx_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_b,
                                  packet_tx_probe_cb,
                                  &tx_probe));

    meshpay_app_event_t rx = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 1001,
        .packet = payment_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_b, MESHPAY_APP_QUEUE_RETICULUM,
                                  &rx, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_b, MESHPAY_APP_QUEUE_RETICULUM,
                                  0));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b->ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_RECEIVE, app_b->ui.screen);
    TEST_ASSERT_EQUAL_UINT32(100, app_b->ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(100, app_b->ui.balance);
    TEST_ASSERT_EQUAL_STRING("pair 4849", app_b->ui.last_peer_label);
    TEST_ASSERT_EQUAL_UINT32(1, runtime_b.processed_reticulum);
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_DATA, tx_probe.last_packet.packet_type);
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_MSG_ACK, tx_probe.last_packet.data[0]);
    TEST_ASSERT_EQUAL_UINT32(1U + MESHPAY_TX_ID_SIZE,
                             tx_probe.last_packet.data_len);
    TEST_ASSERT_EQUAL_MEMORY(app_a->payments.pending_tx.id,
                             tx_probe.last_packet.data + 1,
                             MESHPAY_TX_ID_SIZE);

    meshpay_app_runtime_destroy(&runtime_b);
}

TEST_CASE("app runtime reject packet does not undo committed payment",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x19);
    fill_sequence(alice, sizeof(alice), 0x49);
    fill_sequence(bob, sizeof(bob), 0x79);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 17);
    config.transfer_fee = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x14);
    load_identity(&identity_b, 0x54);

    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_a, &mint));

    meshpay_app_runtime_t runtime_a;
    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       app_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       app_b, NULL));

    packet_tx_probe_t tx_probe = {0};
    packet_tx_probe_t reject_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_a,
                                  packet_tx_probe_cb,
                                  &tx_probe));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_b,
                                  packet_tx_probe_cb,
                                  &reject_probe));

    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 100,
        .now_ms = 5000,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_a, MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a, MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_EQUAL_UINT32(900, app_a->ui.balance);
    TEST_ASSERT_TRUE(app_a->payments.has_pending);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);

    meshpay_app_event_t rx_payment = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 5001,
        .packet = tx_probe.last_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_b,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &rx_payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_b,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, reject_probe.count);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_REJECT,
                            reject_probe.last_packet.data[0]);
    TEST_ASSERT_EQUAL_UINT32(0, app_b->ui.balance);
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_dag_count(&app_b->dag));

    meshpay_app_event_t rx_reject = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 5002,
        .packet = reject_probe.last_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_a,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &rx_reject, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    /* Option A : un REJECT du destinataire n'annule PAS la tx déjà committée.
     * Le solde (900) et le seq (2) ne sont PAS restaurés ; la tx reste en DAG. */
    TEST_ASSERT_FALSE(app_a->payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app_a->wallet, 5003));
    TEST_ASSERT_EQUAL_UINT32(900, app_a->ui.balance);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED, app_a->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(2, app_a->wallet.next_seq);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&app_a->dag));

    meshpay_app_runtime_destroy(&runtime_b);
    meshpay_app_runtime_destroy(&runtime_a);
}

TEST_CASE("app runtime ui refresh expires receipt tracking without undoing payment",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x1e);
    fill_sequence(alice, sizeof(alice), 0x4e);
    fill_sequence(bob, sizeof(bob), 0x7e);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 18);
    config.transfer_fee = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity;
    load_identity(&identity, 0x15);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    packet_tx_probe_t tx_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime,
                                  packet_tx_probe_cb,
                                  &tx_probe));

    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 100,
        .now_ms = 6000,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_TRUE(app->payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(900, app->ui.balance);

    meshpay_app_event_t refresh = {
        .type = MESHPAY_APP_EVENT_UI_REFRESH,
        .now_ms = 6000 + MESHPAY_PAYMENT_RECEIPT_TIMEOUT_MS,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_UI,
                                  &refresh, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_UI, 0));
    /* Option A : l'expiration du suivi de reçu est NON destructive — le solde
     * reste celui de la tx committée (900) et le seq n'est pas restauré (2).
     * Le feedback reste « envoyé » (le paiement a réussi, seul l'accusé manque). */
    TEST_ASSERT_FALSE(app->payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app->wallet,
                                                 refresh.now_ms + 1));
    TEST_ASSERT_EQUAL_UINT32(900, app->ui.balance);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(2, app->wallet.next_seq);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime encrypts payment when destination announce is known",
          "[app_main]")
{
    rns_announce_known_reset();

    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x1c);
    fill_sequence(alice, sizeof(alice), 0x4c);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 12);
    config.transfer_fee = 3;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(&config, master));

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x19);
    load_identity(&identity_b, 0x59);

    rns_destination_t destination_b;
    TEST_ASSERT_EQUAL(ESP_OK,
                      rns_destination_create_meshpay_wallet(&identity_b,
                                                            &destination_b));
    memcpy(bob, destination_b.hash, sizeof(bob));
    remember_announced_wallet(&identity_b, 0x90);

    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 900, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_b, &mint));

    meshpay_app_runtime_t runtime_a;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       app_a, NULL));
    packet_tx_probe_t tx_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_a,
                                  packet_tx_probe_cb,
                                  &tx_probe));

    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 120,
        .now_ms = 3000,
    };
    memcpy(payment.destination, bob, sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_a, MESHPAY_APP_QUEUE_CORE,
                                  &payment, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a, MESHPAY_APP_QUEUE_CORE, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a, MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(RNS_PACKET_CRYPTO_MIN_TOKEN_SIZE,
                                        tx_probe.last_packet.data_len);
    TEST_ASSERT_EQUAL_MEMORY(bob, tx_probe.last_packet.destination_hash,
                             sizeof(bob));

    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       app_b, NULL));
    packet_tx_probe_t ack_probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_b,
                                  packet_tx_probe_cb,
                                  &ack_probe));

    const meshpay_app_event_t encrypted_rx = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 3001,
        .packet = tx_probe.last_packet,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_b,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &encrypted_rx, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_b,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b->ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(120, app_b->ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(120, app_b->ui.balance);
    TEST_ASSERT_TRUE(app_b->payments.has_last_received);
    TEST_ASSERT_EQUAL_UINT32(120, app_b->payments.last_received_tx.amount);
    TEST_ASSERT_EQUAL_UINT32(1, ack_probe.count);
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_PAYMENT_MSG_ACK,
                            ack_probe.last_packet.data[0]);

    meshpay_app_runtime_destroy(&runtime_b);
    meshpay_app_runtime_destroy(&runtime_a);
    rns_announce_known_reset();
}

TEST_CASE("app runtime pays through in memory reticulum nodes",
          "[app_main]")
{
    rns_announce_known_reset();

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x1b);
    load_identity(&identity_b, 0x5b);

    rns_node_t node_a;
    rns_node_t node_b;
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_init(&node_a, &identity_a));
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_init(&node_b, &identity_b));

    const rns_destination_t *destination_a = rns_node_destination(&node_a);
    const rns_destination_t *destination_b = rns_node_destination(&node_b);
    TEST_ASSERT_NOT_NULL(destination_a);
    TEST_ASSERT_NOT_NULL(destination_b);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 15);
    config.transfer_fee = 4;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_add_mint_authority(
                          &config,
                          destination_a->hash));

    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_a,
                                               destination_a->hash,
                                               &identity_a,
                                               &config,
                                               1,
                                               true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app_b,
                                               destination_b->hash,
                                               &identity_b,
                                               &config,
                                               1,
                                               true));

    meshpay_tx_t mint;
    make_mint(&mint, destination_a->hash, destination_a->hash, 700,
              config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app_b, &mint));

    meshpay_app_runtime_t runtime_a;
    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       app_a,
                                                       NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       app_b,
                                                       NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_a,
                                  runtime_node_packet_tx,
                                  &node_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_b,
                                  runtime_node_packet_tx,
                                  &node_b));

    node_bridge_t bridge_a = {
        .peer = &node_b,
        .runtime = &runtime_a,
    };
    node_bridge_t bridge_b = {
        .peer = &node_a,
        .runtime = &runtime_b,
    };
    const rns_node_callbacks_t callbacks_a = {
        .tx = bridge_node_tx,
        .rx = bridge_node_rx,
        .proof = bridge_node_rx,
        .request = bridge_node_rx,
        .ctx = &bridge_a,
    };
    const rns_node_callbacks_t callbacks_b = {
        .tx = bridge_node_tx,
        .rx = bridge_node_rx,
        .proof = bridge_node_rx,
        .request = bridge_node_rx,
        .ctx = &bridge_b,
    };
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_set_callbacks(&node_a, &callbacks_a));
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_set_callbacks(&node_b, &callbacks_b));

    const uint8_t alias_a[] = "Alice";
    const uint8_t alias_b[] = "Bob";
    TEST_ASSERT_EQUAL(ESP_OK, rns_node_announce(&node_b,
                                                alias_b,
                                                sizeof(alias_b) - 1));
    process_reticulum_until_idle(&runtime_a);
    process_reticulum_until_idle(&runtime_b);
    TEST_ASSERT_EQUAL_UINT8(1, app_a->ui.network_peers);

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_announce(&node_a,
                                                alias_a,
                                                sizeof(alias_a) - 1));
    process_reticulum_until_idle(&runtime_a);
    process_reticulum_until_idle(&runtime_b);
    TEST_ASSERT_EQUAL_UINT8(1, app_b->ui.network_peers);

    meshpay_app_event_t payment = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .amount = 90,
        .now_ms = 5000,
    };
    memcpy(payment.destination,
           destination_b->hash,
           sizeof(payment.destination));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime_a,
                                  MESHPAY_APP_QUEUE_CORE,
                                  &payment,
                                  0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime_a,
                                  MESHPAY_APP_QUEUE_CORE,
                                  0));
    process_reticulum_until_idle(&runtime_a);
    process_reticulum_until_idle(&runtime_b);
    process_reticulum_until_idle(&runtime_a);

    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED,
                      app_a->ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b->ui.feedback);
    /* Alice = autorité MINT, donc destinataire de la fee (fee_recipient =
     * mint_authorities[0]). En s'envoyant -90 -4(fee) puis en récupérant +4 en
     * tant qu'autorité, son solde net ne baisse que de 90 : 700 - 90 = 610.
     * (L'attente 606 d'origine oubliait ce retour de fee à l'émetteur-autorité.) */
    TEST_ASSERT_EQUAL_UINT32(610, app_a->ui.balance);
    TEST_ASSERT_EQUAL_UINT32(90, app_b->ui.balance);
    TEST_ASSERT_EQUAL_STRING("Bob", app_a->ui.last_peer_label);
    TEST_ASSERT_EQUAL_STRING("Alice", app_b->ui.last_peer_label);
    TEST_ASSERT_FALSE(app_a->payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(90, app_b->ui.last_amount);
    TEST_ASSERT_TRUE(app_b->payments.has_last_received);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config,
                                                           &app_b->dag,
                                                           destination_b->hash,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(90, balance);

    meshpay_app_runtime_destroy(&runtime_b);
    meshpay_app_runtime_destroy(&runtime_a);
    rns_announce_known_reset();
}

TEST_CASE("app runtime requests and applies dag sync resource batch",
          "[app_main]")
{
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t alice[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t bob[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(master, sizeof(master), 0x1d);
    fill_sequence(alice, sizeof(alice), 0x4d);
    fill_sequence(bob, sizeof(bob), 0x7d);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);

    rns_identity_t identity_a;
    rns_identity_t identity_b;
    load_identity(&identity_a, 0x1a);
    load_identity(&identity_b, 0x5a);

    meshpay_app_t *full = test_pool_app(0);
    meshpay_app_t *slow = test_pool_app(1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(full, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(slow, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    make_mint(&tx0, master, alice, 1000, config.currency_id);
    make_transfer(&tx1, 0xa1, alice, bob, 100, 1, tx0.id);
    make_transfer(&tx2, 0xb1, bob, alice, 40, 1, tx1.id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(full, &tx0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(full, &tx1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(full, &tx2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(slow, &tx0));
    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(&full->dag));
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(&slow->dag));

    meshpay_app_runtime_t slow_runtime;
    meshpay_app_runtime_t full_runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&slow_runtime,
                                                       slow, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&full_runtime,
                                                       full, NULL));

    packet_list_probe_t slow_tx = {0};
    packet_list_probe_t full_tx = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &slow_runtime,
                                  packet_list_probe_cb,
                                  &slow_tx));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &full_runtime,
                                  packet_list_probe_cb,
                                  &full_tx));

    rns_packet_t summary;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_build_summary(&full->dag,
                                                     alice,
                                                     &summary));
    const meshpay_app_event_t summary_rx = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 4000,
        .packet = summary,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &slow_runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &summary_rx, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &slow_runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, slow_tx.count);
    TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_REQUEST, slow_tx.packets[0].context);
    TEST_ASSERT_EQUAL(RNS_DESTINATION_TYPE_LINK,
                      slow_tx.packets[0].destination_type);
    TEST_ASSERT_EQUAL_MEMORY(alice, slow_tx.packets[0].destination_hash,
                             sizeof(alice));
    uint16_t request_known_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_request_known_count(&slow_tx.packets[0],
                                                           &request_known_count));
    /* known=0 par conception : le runtime émet TOUJOURS known=0 (cf.
     * app_main_logic.c, handler de summary), car le découpage du batch côté
     * répondeur est POSITIONNEL et repart de l'offset 0 — c'est le fix de
     * réconciliation DAG sous fork. (L'attente 1 d'origine reflétait l'ancien
     * comportement « known = count(DAG locale) ».) */
    TEST_ASSERT_EQUAL_UINT16(0, request_known_count);
    bool request_has_source = false;
    uint8_t request_source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_dag_sync_request_source(&slow_tx.packets[0],
                                                      request_source,
                                                      &request_has_source));
    TEST_ASSERT_TRUE(request_has_source);
    TEST_ASSERT_EQUAL_MEMORY(bob, request_source, sizeof(bob));

    const meshpay_app_event_t request_rx = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 4001,
        .packet = slow_tx.packets[0],
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &full_runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM,
                                  &request_rx, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &full_runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_GREATER_THAN_UINT32(0, full_tx.count);
    for (uint32_t i = 0; i < full_tx.count; ++i) {
        TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_RESOURCE,
                          full_tx.packets[i].context);
        TEST_ASSERT_EQUAL_MEMORY(bob, full_tx.packets[i].destination_hash,
                                 sizeof(bob));
        const meshpay_app_event_t resource_rx = {
            .type = MESHPAY_APP_EVENT_RETICULUM_RX,
            .now_ms = 4002 + i,
            .packet = full_tx.packets[i],
        };
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                      &slow_runtime,
                                      MESHPAY_APP_QUEUE_RETICULUM,
                                      &resource_rx, 0));
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                      &slow_runtime,
                                      MESHPAY_APP_QUEUE_RETICULUM, 0));
    }

    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(&slow->dag));
    TEST_ASSERT_TRUE(meshpay_dag_contains(&slow->dag, tx2.id));
    TEST_ASSERT_EQUAL_UINT32(2, slow_runtime.dag_sync_merged);

    meshpay_app_runtime_destroy(&full_runtime);
    meshpay_app_runtime_destroy(&slow_runtime);
}

TEST_CASE("app alias generation creates animal quality label", "[app_main]")
{
    uint8_t seed[2] = {0, 0};
    rns_crypto_set_rng(fixed_alias_rng, seed);
    char alias[MESHPAY_STORAGE_ALIAS_MAX] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_generate_alias(alias,
                                                         sizeof(alias)));
    rns_crypto_set_rng(NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("renard malin", alias);
    TEST_ASSERT_NOT_NULL(strchr(alias, ' '));
    TEST_ASSERT_FALSE(meshpay_app_alias_needs_generation(alias));
    TEST_ASSERT_TRUE(meshpay_app_alias_needs_generation(""));
    TEST_ASSERT_TRUE(meshpay_app_alias_needs_generation(MESHPAY_APP_LEGACY_ALIAS));
}

TEST_CASE("app alias ensure replaces legacy alias once", "[app_main]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(
                                  &record,
                                  MESHPAY_APP_LEGACY_ALIAS));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);

    uint8_t seed[2] = {0, 0};
    rns_crypto_set_rng(fixed_alias_rng, seed);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_ensure_record_alias(&backend,
                                                              &record));
    rns_crypto_set_rng(NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("renard malin", record.alias);
    TEST_ASSERT_EQUAL_UINT32(2, mock.write_count);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_ensure_record_alias(&backend,
                                                              &record));
    TEST_ASSERT_EQUAL_UINT32(2, mock.write_count);
}

TEST_CASE("app bootstrap creates and reloads persistent identity", "[app_main]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    rns_identity_t first;
    meshpay_storage_record_t first_record;
    bool created = false;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_bootstrap_identity(
                                  &backend, "Alice", &first,
                                  &first_record, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_TRUE(first.has_private);
    TEST_ASSERT_TRUE(first_record.has_identity);
    TEST_ASSERT_EQUAL_STRING("Alice", first_record.alias);
    TEST_ASSERT_EQUAL_UINT32(1, first_record.next_seq);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);

    uint8_t first_private[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&first,
                                                           first_private));

    rns_identity_t second;
    meshpay_storage_record_t second_record;
    created = true;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_bootstrap_identity(
                                  &backend, "Ignored", &second,
                                  &second_record, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);
    TEST_ASSERT_EQUAL_STRING("Alice", second_record.alias);

    uint8_t second_private[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&second,
                                                           second_private));
    TEST_ASSERT_EQUAL_MEMORY(first_private, second_private,
                             sizeof(first_private));
}

TEST_CASE("app bootstrap rejects stored record without identity", "[app_main]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&record,
                                                               "NoIdentity"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));

    rns_identity_t identity;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_app_bootstrap_identity(&backend, "Fallback",
                                                     &identity, NULL, NULL));
}

/* --- Palier A5 : config monnaie au boot depuis le record (repli sûr) --- *
 * Tag dédié [a5] : ces tests n'utilisent que record + config (petites
 * structures) et peuvent être lancés seuls, sans le reste de la suite
 * [app_main] qui place de gros meshpay_app_t (~57 Ko de DAG) sur la pile. */

/* Construit un record portant un descripteur signé par 'founder'. */
static void record_with_descriptor(meshpay_storage_record_t *record,
                                   const rns_identity_t *founder)
{
    meshpay_storage_record_init(record);

    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    strncpy(body.name, "Minimistan", sizeof(body.name) - 1);
    strncpy(body.symbol, "MIN", sizeof(body.symbol) - 1);
    body.max_supply = 12345;
    body.transfer_fee = 2;

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                       founder));
    uint8_t wire[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_encode(&signed_desc, wire,
                                                         sizeof(wire), &wire_len));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_record_set_currency_descriptor(record, wire,
                                                                     wire_len));
}

TEST_CASE("app currency_from_record derives from valid descriptor", "[app_main][a5]")
{
    rns_identity_t founder;
    load_identity(&founder, 0x30);

    meshpay_storage_record_t record;
    record_with_descriptor(&record, &founder);

    meshpay_currency_config_t fallback;
    meshpay_currency_config_init(&fallback, 1); /* repli codé en dur */

    meshpay_currency_config_t out;
    bool from_desc = false;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_app_currency_from_record(&record, &fallback, &out, &from_desc));

    TEST_ASSERT_TRUE(from_desc);
    TEST_ASSERT_TRUE(out.has_descriptor);
    TEST_ASSERT_EQUAL_UINT64(12345, out.max_supply); /* dérivé, != repli (0) */
    /* Autorité MINT unique = hash du fondateur. */
    uint8_t founder_hash[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, founder_hash));
    TEST_ASSERT_EQUAL_UINT8(1, out.mint_authority_count);
    TEST_ASSERT_TRUE(meshpay_currency_is_mint_authority(&out, founder_hash));
}

TEST_CASE("app currency_from_record falls back without descriptor", "[app_main][a5]")
{
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record); /* aucun descripteur */
    TEST_ASSERT_FALSE(record.has_currency_descriptor);

    meshpay_currency_config_t fallback;
    meshpay_currency_config_init(&fallback, 7);
    fallback.transfer_fee = 9;

    meshpay_currency_config_t out;
    bool from_desc = true;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_app_currency_from_record(&record, &fallback, &out, &from_desc));

    TEST_ASSERT_FALSE(from_desc);
    TEST_ASSERT_FALSE(out.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(7, out.currency_id);   /* config de repli conservée */
    TEST_ASSERT_EQUAL_UINT32(9, out.transfer_fee);
}

TEST_CASE("app currency_from_record falls back on corrupt descriptor", "[app_main][a5]")
{
    rns_identity_t founder;
    load_identity(&founder, 0x31);

    meshpay_storage_record_t record;
    record_with_descriptor(&record, &founder);
    /* Corrompre un octet du corps signé : la vérif échoue -> repli (pas vierge,
     * pas de blocage du boot). */
    record.currency_descriptor[5] ^= 0xFF;

    meshpay_currency_config_t fallback;
    meshpay_currency_config_init(&fallback, 7);

    meshpay_currency_config_t out;
    bool from_desc = true;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_app_currency_from_record(&record, &fallback, &out, &from_desc));

    TEST_ASSERT_FALSE(from_desc);
    TEST_ASSERT_FALSE(out.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(7, out.currency_id);   /* repli */
}

TEST_CASE("app currency_from_record rejects NULL", "[app_main][a5]")
{
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    meshpay_currency_config_t cfg;
    meshpay_currency_config_init(&cfg, 1);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        meshpay_app_currency_from_record(NULL, &cfg, &cfg, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        meshpay_app_currency_from_record(&record, NULL, &cfg, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        meshpay_app_currency_from_record(&record, &cfg, NULL, NULL));
}

/* ======================================================================== */
/* Palier B4 — machine à états de rejointe de monnaie                        */
/* ======================================================================== */

/* Signe un descripteur « Minimistan » par le fondateur de graine donnée. */
static void sign_min_descriptor(rns_identity_t *founder, uint8_t founder_seed,
                                meshpay_currency_descriptor_signed_t *out)
{
    load_identity(founder, founder_seed);
    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    strncpy(body.name, "Minimistan", sizeof(body.name) - 1);
    strncpy(body.symbol, "MIN", sizeof(body.symbol) - 1);
    body.max_supply = 12345;
    body.transfer_fee = 2;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(out, &body, founder));
}

/* Construit un paquet OFFER (PLAIN broadcast) portant le descripteur. */
static void build_join_offer(const meshpay_currency_descriptor_signed_t *signed_desc,
                             rns_packet_t *offer)
{
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(source, sizeof(source), 0xF0); /* provenance (fondateur) */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_descriptor_sync_build_offer(signed_desc, source, offer));
}

/* Injecte un paquet radio dans la file RETICULUM puis le traite (chemin réel). */
static void inject_reticulum_packet(meshpay_app_runtime_t *runtime,
                                    const rns_packet_t *packet, uint64_t now_ms)
{
    meshpay_app_event_t ev = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = now_ms,
    };
    ev.packet = *packet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  runtime, MESHPAY_APP_QUEUE_RETICULUM, &ev, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  runtime, MESHPAY_APP_QUEUE_RETICULUM, 0));
}

/*
 * Monte un runtime « membre » vierge (config repli id=1, sans descripteur) avec
 * un backend storage mock. `mock` appartient à l'appelant (le backend copié dans
 * le runtime pointe dessus). identité et record sont locaux (copiés à l'init).
 */
static void member_runtime_init(meshpay_app_runtime_t *runtime,
                                meshpay_app_t *app,
                                meshpay_storage_mock_t *mock)
{
    uint8_t me[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(me, sizeof(me), 0x50);
    rns_identity_t self;
    load_identity(&self, 0x50);
    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1); /* repli : pas de descripteur */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, me, &self, &config, 1, true));

    meshpay_storage_mock_init(mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(mock);
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t priv[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&self, priv));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record, priv));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(runtime, app, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_storage(runtime, &backend,
                                                              &record));
}

/* Fait rejoindre le runtime à une monnaie (arme l'ancre + importe l'OFFER). */
static void join_a_currency(meshpay_app_runtime_t *runtime, meshpay_app_t *app,
                            meshpay_storage_mock_t *mock, rns_identity_t *founder,
                            meshpay_currency_descriptor_signed_t *signed_out)
{
    member_runtime_init(runtime, app, mock);
    sign_min_descriptor(founder, 0x30, signed_out);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  runtime, signed_out->genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));
    rns_packet_t offer;
    build_join_offer(signed_out, &offer);
    inject_reticulum_packet(runtime, &offer, 1000);
    TEST_ASSERT_TRUE(app->currency.has_descriptor); /* pré-condition membre */
}

TEST_CASE("join nominal via invite code imports descriptor", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    /* Le fondateur affiche un CODE d'invitation ; le membre le saisit -> arme. */
    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join(&runtime, code, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_ARMED,
                      meshpay_app_runtime_join_state(&runtime));

    uint32_t writes_before = mock.write_count;
    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 1500);

    /* Devenu membre : config dérivée du descripteur, autorité MINT = fondateur. */
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_MEMBER,
                      meshpay_app_runtime_join_state(&runtime));
    TEST_ASSERT_TRUE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, app->currency.currency_id);
    uint8_t founder_hash[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, founder_hash));
    TEST_ASSERT_EQUAL_UINT8(1, app->currency.mint_authority_count);
    TEST_ASSERT_TRUE(meshpay_currency_is_mint_authority(&app->currency, founder_hash));
    TEST_ASSERT_FALSE(runtime.join_armed);
    /* Persisté sur le record DU RUNTIME (celui que persist_wallet_state réécrit). */
    TEST_ASSERT_TRUE(runtime.storage_record.has_currency_descriptor);
    TEST_ASSERT_TRUE(mock.write_count > writes_before);
    /* Et durablement écrit : rechargé depuis le backend, le blob doit se
     * RE-DÉCODER en la MÊME config (survie au reboot vérifiée de bout en bout,
     * pas seulement le drapeau). Un slice/offset erroné donnerait un blob
     * corrompu -> repli silencieux au boot ; ce contrôle l'attraperait. */
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);
    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_TRUE(loaded.has_currency_descriptor);
    meshpay_currency_config_t fallback_cfg;
    meshpay_currency_config_init(&fallback_cfg, 1);
    meshpay_currency_config_t reloaded_cfg;
    bool reloaded_from_desc = false;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_currency_from_record(
                                  &loaded, &fallback_cfg, &reloaded_cfg,
                                  &reloaded_from_desc));
    TEST_ASSERT_TRUE(reloaded_from_desc);
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, reloaded_cfg.currency_id);
    TEST_ASSERT_EQUAL_UINT64(12345, reloaded_cfg.max_supply);
    TEST_ASSERT_TRUE(meshpay_currency_is_mint_authority(&reloaded_cfg, founder_hash));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join offer with non-matching anchor rejected", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    /* Armé sur la monnaie A, on reçoit un OFFER de la monnaie B (autre fondateur
     * -> autre genèse -> autre ancre). */
    rns_identity_t founder_a, founder_b;
    meshpay_currency_descriptor_signed_t desc_a, desc_b;
    sign_min_descriptor(&founder_a, 0x30, &desc_a);
    sign_min_descriptor(&founder_b, 0x31, &desc_b);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, desc_a.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));

    uint32_t writes_before = mock.write_count;
    rns_packet_t offer_b;
    build_join_offer(&desc_b, &offer_b);
    inject_reticulum_packet(&runtime, &offer_b, 1000);

    /* Rejeté sans mutation ni persistance ; reste armé. */
    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(writes_before, mock.write_count);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_ARMED,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join offer with bad signature rejected", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));

    /* Corrompre la SIGNATURE (pas le corps) : l'ancre matche toujours (genèse
     * inchangée), mais verify doit échouer -> prouve que matches_anchor seul ne
     * suffit pas. */
    signed_desc.founder_signature[0] ^= 0x55;
    uint32_t writes_before = mock.write_count;
    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 1000);

    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(writes_before, mock.write_count);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_ARMED,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join idempotent when already member", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    join_a_currency(&runtime, app, &mock, &founder, &signed_desc);

    /* Déjà membre : un nouvel OFFER (même monnaie) est ignoré, aucun re-save. */
    uint32_t writes_after_join = mock.write_count;
    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 2000);

    TEST_ASSERT_EQUAL_UINT32(writes_after_join, mock.write_count);
    TEST_ASSERT_TRUE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_MEMBER,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("arm join rejected when already member", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    join_a_currency(&runtime, app, &mock, &founder, &signed_desc);

    /* Mono-monnaie strict : on ne ré-arme pas alors qu'on est déjà membre. */
    uint8_t other_anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    fill_sequence(other_anchor, sizeof(other_anchor), 0x99);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_app_runtime_arm_join_anchor(
                          &runtime, other_anchor, sizeof(other_anchor), 3000));
    TEST_ASSERT_FALSE(runtime.join_armed);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_MEMBER,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join does not pollute mint authority after import", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    join_a_currency(&runtime, app, &mock, &founder, &signed_desc);

    /* Autorité unique (fondateur) après import. */
    TEST_ASSERT_EQUAL_UINT8(1, app->currency.mint_authority_count);

    /* Un announce de pair arrive : sous config à descripteur, le pair NE DOIT
     * PAS être ajouté comme autorité MINT (durcissement single-authority). */
    rns_identity_t peer;
    load_identity(&peer, 0x88);
    rns_packet_t announce;
    build_wallet_announce_packet(&peer, 0x88, &announce);
    inject_reticulum_packet(&runtime, &announce, 4000);

    TEST_ASSERT_EQUAL_UINT8(1, app->currency.mint_authority_count);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join offer ignored when not armed", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    /* Pas d'ancre armée : un OFFER valide ne doit rien importer. */
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    uint32_t writes_before = mock.write_count;
    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 1000);

    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(writes_before, mock.write_count);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_IDLE,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("emit join request broadcasts request for armed currency", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    packet_tx_probe_t probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime, packet_tx_probe_cb, &probe));

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    /* Non armé -> refus. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_app_runtime_emit_join_request(&runtime, 1000));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_app_runtime_emit_join_request(&runtime, 1500));

    /* Une REQUEST 0x33 a été émise, avec le currency_id dérivé de l'ancre. */
    TEST_ASSERT_EQUAL_HEX8(MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST,
                           probe.last_packet.data[0]);
    uint32_t cid = 0;
    uint8_t src[MESHPAY_TX_DESTINATION_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_descriptor_sync_parse_request(
                                  &probe.last_packet, &cid, src));
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, cid);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("arm join rejects invalid invite code", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    /* Code mal formé (longueur/alphabet/checksum) -> pas d'armement. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_app_runtime_arm_join(&runtime, "PAS-UN-CODE", 1000));
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_IDLE,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join import rolls back and stays armed when save fails", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock);

    /* Substitue l'écriture par une qui ÉCHOUE toujours : l'import doit rollback
     * le record ET ne pas appliquer la config (atomicité). */
    runtime.storage_backend.write_blob = failing_storage_write;

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));

    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 1000);

    /* Save KO -> pas membre, record revenu à l'état antérieur, toujours armé. */
    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_FALSE(runtime.storage_record.has_currency_descriptor);
    TEST_ASSERT_EQUAL_size_t(0, runtime.storage_record.currency_descriptor_len);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_ARMED,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join offer refused without a storage backend", "[app_main][b4]")
{
    uint8_t me[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(me, sizeof(me), 0x50);
    rns_identity_t self;
    load_identity(&self, 0x50);
    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, me, &self, &config, 1, true));

    /* Runtime SANS set_storage -> has_storage=false : une rejointe ne pourrait
     * pas survivre au reboot, l'import doit donc refuser. */
    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));

    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 1000);

    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_ARMED,
                      meshpay_app_runtime_join_state(&runtime));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("emit join request rejected when no packet_tx is set", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock); /* n'attache PAS de packet_tx */

    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));

    /* Armé mais aucun émetteur radio câblé -> refus explicite. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_app_runtime_emit_join_request(&runtime, 1000));

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("open mesh adds announced peer as mint authority without descriptor", "[app_main][b4]")
{
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    member_runtime_init(&runtime, app, &mock); /* config repli id=1, 0 autorité */

    TEST_ASSERT_FALSE(app->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT8(0, app->currency.mint_authority_count);

    /* Sans descripteur (maillage ouvert), un pair annoncé DEVIENT autorité MINT
     * — c'est l'ancien comportement, préservé par le gate !has_descriptor. */
    rns_identity_t peer;
    load_identity(&peer, 0x88);
    rns_packet_t announce;
    build_wallet_announce_packet(&peer, 0x88, &announce);
    inject_reticulum_packet(&runtime, &announce, 1000);

    TEST_ASSERT_EQUAL_UINT8(1, app->currency.mint_authority_count);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("join recomputes balance under the joined currency", "[app_main][b4]")
{
    uint8_t me[MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t master[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(me, sizeof(me), 0x50);
    fill_sequence(master, sizeof(master), 0x20);
    rns_identity_t self;
    load_identity(&self, 0x50);

    /* Config de repli id=1 AVEC une autorité, pour créditer un solde initial. */
    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_add_mint_authority(&config, master));

    meshpay_app_t *app = test_pool_app(0);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, me, &self, &config, 1, true));
    /* MINT vers le membre sous l'ANCIEN currency_id=1 -> solde non nul. */
    meshpay_tx_t mint;
    make_mint(&mint, master, me, 500, 1);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(app, &mint));

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_get_available_balance(
                                  &app->wallet, &app->currency, &app->dag, 1000,
                                  &balance));
    TEST_ASSERT_TRUE(balance > 0); /* solde sous id=1 avant rejointe */

    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t priv[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&self, priv));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record, priv));
    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, app, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_storage(&runtime, &backend,
                                                              &record));

    /* Rejoint une monnaie au currency_id DIFFÉRENT (via le descripteur). */
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);
    TEST_ASSERT_NOT_EQUAL(1u, signed_desc.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));
    rns_packet_t offer;
    build_join_offer(&signed_desc, &offer);
    inject_reticulum_packet(&runtime, &offer, 2000);
    TEST_ASSERT_TRUE(app->currency.has_descriptor);

    /* Les TX de l'ancienne monnaie sont filtrées par currency_id : solde = 0,
     * et refresh_balance (appelé à l'import) a bien poussé 0 dans l'UI. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_wallet_get_available_balance(
                                  &app->wallet, &app->currency, &app->dag, 2000,
                                  &balance));
    TEST_ASSERT_EQUAL_UINT32(0, balance);
    TEST_ASSERT_EQUAL_UINT32(0, app->ui.balance);

    meshpay_app_runtime_destroy(&runtime);
}

/* ======================================================================== */
/* Palier B5 — côté fondateur / répondeur                                    */
/* ======================================================================== */

/*
 * Monte un runtime DÉJÀ MEMBRE d'une monnaie (config dérivée du descripteur +
 * blob en storage) : il peut donc servir un OFFER sur demande. `mock` appartient
 * à l'appelant. `addr_seed` distingue l'adresse locale (pour ponter 2 runtimes).
 */
static void founder_runtime_init(meshpay_app_runtime_t *runtime,
                                 meshpay_app_t *app,
                                 meshpay_storage_mock_t *mock,
                                 const meshpay_currency_descriptor_signed_t *signed_desc,
                                 uint8_t addr_seed)
{
    uint8_t addr[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(addr, sizeof(addr), addr_seed);
    rns_identity_t self;
    load_identity(&self, addr_seed);
    /* Config dérivée du descripteur -> has_descriptor = true (membre). */
    meshpay_currency_config_t config;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_config_from_descriptor(&config, signed_desc));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(app, addr, &self, &config, 1, true));

    meshpay_storage_mock_init(mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(mock);
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t priv[RNS_IDENTITY_PRIVATE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_private_key(&self, priv));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record, priv));
    /* Blob du descripteur en storage : c'est lui que le répondeur ressert. */
    uint8_t wire[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode(signed_desc, wire,
                                                                 sizeof(wire),
                                                                 &wire_len));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_currency_descriptor(
                                  &record, wire, wire_len));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(runtime, app, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_storage(runtime, &backend,
                                                              &record));
}

/* Pont direct : le TX d'un runtime pousse le paquet dans la file RX de l'autre
 * (suffisant pour les messages de rejointe, qui sont PLAIN broadcast). */
typedef struct {
    meshpay_app_runtime_t *dst;
} direct_bridge_t;

static esp_err_t direct_bridge_tx(const rns_packet_t *packet, void *ctx)
{
    direct_bridge_t *bridge = (direct_bridge_t *)ctx;
    meshpay_app_event_t ev = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = 1000,
        .packet = *packet,
    };
    return meshpay_app_runtime_post(bridge->dst, MESHPAY_APP_QUEUE_RETICULUM,
                                    &ev, 0);
}

TEST_CASE("member serves an offer for a matching descriptor request", "[app_main][b5]")
{
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    founder_runtime_init(&runtime, app, &mock, &signed_desc, 0x30);
    TEST_ASSERT_TRUE(app->currency.has_descriptor);

    packet_tx_probe_t probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime, packet_tx_probe_cb, &probe));

    /* Un membre demande le descripteur de CETTE monnaie. */
    uint8_t requester[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(requester, sizeof(requester), 0x77);
    rns_packet_t request;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_descriptor_sync_build_request(
                                  signed_desc.currency_id, requester, &request));
    inject_reticulum_packet(&runtime, &request, 1000);

    /* Exactement un OFFER 0x34 émis, décodable en le même descripteur signé. */
    TEST_ASSERT_EQUAL_UINT32(1, probe.count);
    TEST_ASSERT_EQUAL_HEX8(MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER, probe.last_packet.data[0]);
    meshpay_currency_descriptor_signed_t served;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_descriptor_sync_parse_offer(
                                  &probe.last_packet, &served));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&served));
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, served.currency_id);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("member ignores request for another currency or when non-member", "[app_main][b5]")
{
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    /* Cas 1 : membre, mais REQUEST pour un AUTRE currency_id -> pas d'OFFER. */
    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    founder_runtime_init(&runtime, app, &mock, &signed_desc, 0x30);
    packet_tx_probe_t probe = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime, packet_tx_probe_cb, &probe));

    uint8_t requester[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(requester, sizeof(requester), 0x77);
    rns_packet_t request;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_descriptor_sync_build_request(
                                  signed_desc.currency_id ^ 0xFFFFFFFFu, requester,
                                  &request));
    inject_reticulum_packet(&runtime, &request, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, probe.count); /* aucune monnaie correspondante */

    meshpay_app_runtime_destroy(&runtime);

    /* Cas 2 : NON-membre (config de repli, pas de descripteur) -> pas d'OFFER. */
    meshpay_app_t *app2 = test_pool_app(1);
    meshpay_storage_mock_t mock2;
    meshpay_app_runtime_t runtime2;
    member_runtime_init(&runtime2, app2, &mock2);
    packet_tx_probe_t probe2 = {0};
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime2, packet_tx_probe_cb, &probe2));
    rns_packet_t request2;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_descriptor_sync_build_request(
                                  signed_desc.currency_id, requester, &request2));
    inject_reticulum_packet(&runtime2, &request2, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, probe2.count); /* pas de descripteur à servir */

    meshpay_app_runtime_destroy(&runtime2);
}

TEST_CASE("member exposes its invitation code from the stored descriptor", "[app_main][b5]")
{
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    meshpay_app_t *app = test_pool_app(0);
    meshpay_storage_mock_t mock;
    meshpay_app_runtime_t runtime;
    founder_runtime_init(&runtime, app, &mock, &signed_desc, 0x30);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_invite_code(&runtime, code,
                                                              sizeof(code)));
    /* Le code décode vers l'ancre du descripteur détenu. */
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_invite_decode(code, anchor,
                                                             sizeof(anchor),
                                                             &anchor_len));
    TEST_ASSERT_EQUAL_size_t(MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, anchor_len);
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.genesis_hash, anchor,
                             MESHPAY_CURRENCY_INVITE_ANCHOR_LEN);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("bridged join: member requests, founder serves, member imports", "[app_main][b5]")
{
    rns_identity_t founder;
    meshpay_currency_descriptor_signed_t signed_desc;
    sign_min_descriptor(&founder, 0x30, &signed_desc);

    /* A = fondateur (membre, sert le descripteur). B = membre vierge, armé. */
    meshpay_app_t *app_a = test_pool_app(0);
    meshpay_app_t *app_b = test_pool_app(1);
    meshpay_storage_mock_t mock_a, mock_b;
    meshpay_app_runtime_t runtime_a, runtime_b;
    founder_runtime_init(&runtime_a, app_a, &mock_a, &signed_desc, 0x30);
    member_runtime_init(&runtime_b, app_b, &mock_b);

    /* Pont direct A<->B (chaque TX pousse dans la file RX de l'autre). */
    direct_bridge_t a_to_b = { .dst = &runtime_b };
    direct_bridge_t b_to_a = { .dst = &runtime_a };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_a, direct_bridge_tx, &a_to_b));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_set_packet_tx(
                                  &runtime_b, direct_bridge_tx, &b_to_a));

    /* B arme sur l'ancre de A puis émet une REQUEST -> file RX de A. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_arm_join_anchor(
                                  &runtime_b, signed_desc.genesis_hash,
                                  MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_emit_join_request(&runtime_b, 1000));

    /* A traite la REQUEST -> émet un OFFER -> file RX de B. */
    process_reticulum_until_idle(&runtime_a);
    /* B traite l'OFFER -> importe le descripteur. */
    process_reticulum_until_idle(&runtime_b);

    /* B est devenu membre de la monnaie de A. */
    TEST_ASSERT_EQUAL(MESHPAY_APP_JOIN_MEMBER,
                      meshpay_app_runtime_join_state(&runtime_b));
    TEST_ASSERT_TRUE(app_b->currency.has_descriptor);
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, app_b->currency.currency_id);

    meshpay_app_runtime_destroy(&runtime_a);
    meshpay_app_runtime_destroy(&runtime_b);
}
