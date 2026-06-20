#include "meshpay/app_main_logic.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_node.h"
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

    meshpay_app_t app_a;
    meshpay_app_t app_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_b, bob, &identity_b,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HOME, app_a.ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_APP_UI_TASK_NAME[0], 'u');
    TEST_ASSERT_EQUAL(MESHPAY_APP_CORE_TASK_NAME[0], 'c');

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_b, &mint));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_announce(&app_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_announce(&app_b));
    TEST_ASSERT_TRUE(app_a.announced);
    TEST_ASSERT_TRUE(app_b.announced);
    TEST_ASSERT_EQUAL_UINT8(0, app_a.ui.network_peers);
    TEST_ASSERT_EQUAL_UINT8(0, app_b.ui.network_peers);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_pay(&app_a, &app_b, 100, 1000));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED,
                      app_a.ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HISTORY, app_a.ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b.ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_RECEIVE, app_b.ui.screen);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&app_a.currency,
                                                           &app_a.dag,
                                                           alice,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(895, balance);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&app_b.currency,
                                                           &app_b.dag,
                                                           bob,
                                                           &balance));
    TEST_ASSERT_EQUAL_UINT32(100, balance);
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&app_a.dag));
    TEST_ASSERT_EQUAL_UINT32(2, meshpay_dag_count(&app_b.dag));
}

TEST_CASE("app runtime creates queues and non recursive mutex", "[app_main]")
{
    uint8_t owner[MESHPAY_TX_DESTINATION_HASH_SIZE];
    fill_sequence(owner, sizeof(owner), 0x31);

    meshpay_currency_config_t config;
    meshpay_currency_config_init(&config, 7);

    rns_identity_t identity;
    load_identity(&identity, 0x71);

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, owner, &identity,
                                               &config, 1, true));

    meshpay_app_runtime_config_t runtime_config =
        meshpay_app_runtime_default_config();
    runtime_config.ui_queue_length = 2;
    runtime_config.reticulum_queue_length = 2;
    runtime_config.core_queue_length = 2;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app,
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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL_UINT8(0, app.ui.network_peers);

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));

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
    TEST_ASSERT_EQUAL_UINT8(1, app.ui.network_peers);
    TEST_ASSERT_EQUAL_UINT8(1, app.ui.payment_peer_count);
    TEST_ASSERT_EQUAL_STRING("test-peer", app.ui.payment_peer_label);
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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));
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
    TEST_ASSERT_TRUE(app.announced);
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
    TEST_ASSERT_EQUAL_UINT32(1000, app.ui.balance);
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
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(895, app.ui.balance);
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
    memcpy(ack_packet.data + 1, app.payments.pending_tx.id,
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
                      app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(100, app.ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(895, app.ui.balance);
    TEST_ASSERT_EQUAL_STRING("pair 7273", app.ui.last_peer_label);
    TEST_ASSERT_FALSE(app.payments.has_pending);
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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app, &mint));

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
    record.next_seq = app.wallet.next_seq;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));
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
    TEST_ASSERT_TRUE(app.payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(2, app.wallet.next_seq);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);

    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_EQUAL_UINT32(2, loaded.next_seq);

    meshpay_app_runtime_destroy(&runtime);
}

TEST_CASE("app runtime rejects second payment without canceling pending ack",
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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 100, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));
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
    TEST_ASSERT_TRUE(app.payments.has_pending);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(60, app.ui.balance);
    uint8_t pending_id[MESHPAY_TX_ID_SIZE];
    memcpy(pending_id, app.payments.pending_tx.id, sizeof(pending_id));

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
    TEST_ASSERT_TRUE(app.payments.has_pending);
    TEST_ASSERT_EQUAL_MEMORY(pending_id, app.payments.pending_tx.id,
                             sizeof(pending_id));
    TEST_ASSERT_TRUE(meshpay_wallet_lock_active(&app.wallet, 1002));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED, app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(60, app.ui.balance);
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_app_runtime_queue_depth(
                                    &runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_RETICULUM, 0));
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);

    rns_packet_t ack_packet;
    rns_packet_clear(&ack_packet);
    ack_packet.header_type = RNS_PACKET_HEADER_TYPE_1;
    ack_packet.destination_type = RNS_DESTINATION_TYPE_SINGLE;
    ack_packet.packet_type = RNS_PACKET_TYPE_DATA;
    memcpy(ack_packet.destination_hash, alice,
           sizeof(ack_packet.destination_hash));
    ack_packet.data[0] = MESHPAY_PAYMENT_MSG_ACK;
    memcpy(ack_packet.data + 1, pending_id, MESHPAY_TX_ID_SIZE);
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
    TEST_ASSERT_FALSE(app.payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app.wallet, 1004));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED, app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(40, app.ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(60, app.ui.balance);

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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 500, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app, &mint));

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
    record.next_seq = app.wallet.next_seq;

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));
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
    TEST_ASSERT_FALSE(app.payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app.wallet, 2201));
    TEST_ASSERT_EQUAL_UINT32(1, app.wallet.next_seq);
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

    meshpay_app_t app_a;
    meshpay_app_t app_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_b, &mint));

    rns_packet_t payment_packet;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_payment_engine_create_payment(
                                  &app_a.payments, bob, 100, 1000,
                                  &payment_packet));

    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       &app_b, NULL));
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
                      app_b.ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_RECEIVE, app_b.ui.screen);
    TEST_ASSERT_EQUAL_UINT32(100, app_b.ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(100, app_b.ui.balance);
    TEST_ASSERT_EQUAL_STRING("pair 4849", app_b.ui.last_peer_label);
    TEST_ASSERT_EQUAL_UINT32(1, runtime_b.processed_reticulum);
    TEST_ASSERT_EQUAL_UINT32(1, tx_probe.count);
    TEST_ASSERT_EQUAL(RNS_PACKET_TYPE_DATA, tx_probe.last_packet.packet_type);
    TEST_ASSERT_EQUAL(MESHPAY_PAYMENT_MSG_ACK, tx_probe.last_packet.data[0]);
    TEST_ASSERT_EQUAL_UINT32(1U + MESHPAY_TX_ID_SIZE,
                             tx_probe.last_packet.data_len);
    TEST_ASSERT_EQUAL_MEMORY(app_a.payments.pending_tx.id,
                             tx_probe.last_packet.data + 1,
                             MESHPAY_TX_ID_SIZE);

    meshpay_app_runtime_destroy(&runtime_b);
}

TEST_CASE("app runtime reject packet restores payer after receiver cannot validate",
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

    meshpay_app_t app_a;
    meshpay_app_t app_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_a, &mint));

    meshpay_app_runtime_t runtime_a;
    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       &app_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       &app_b, NULL));

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
    TEST_ASSERT_EQUAL_UINT32(900, app_a.ui.balance);
    TEST_ASSERT_TRUE(app_a.payments.has_pending);

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
    TEST_ASSERT_EQUAL_UINT32(0, app_b.ui.balance);
    TEST_ASSERT_EQUAL_UINT32(0, meshpay_dag_count(&app_b.dag));

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
    TEST_ASSERT_FALSE(app_a.payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app_a.wallet, 5003));
    TEST_ASSERT_EQUAL_UINT32(1000, app_a.ui.balance);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED, app_a.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(1, app_a.wallet.next_seq);

    meshpay_app_runtime_destroy(&runtime_b);
    meshpay_app_runtime_destroy(&runtime_a);
}

TEST_CASE("app runtime ui refresh expires unanswered payment lock",
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

    meshpay_app_t app;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app, alice, &identity,
                                               &config, 1, true));
    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 1000, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app, &mint));

    meshpay_app_runtime_t runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime, &app, NULL));
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
    TEST_ASSERT_TRUE(app.payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(900, app.ui.balance);

    meshpay_app_event_t refresh = {
        .type = MESHPAY_APP_EVENT_UI_REFRESH,
        .now_ms = 6000 + MESHPAY_WALLET_LOCK_TIMEOUT_MS,
    };
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_post(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_UI,
                                  &refresh, 0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_process_one(
                                  &runtime,
                                  MESHPAY_APP_QUEUE_UI, 0));
    TEST_ASSERT_FALSE(app.payments.has_pending);
    TEST_ASSERT_FALSE(meshpay_wallet_lock_active(&app.wallet,
                                                 refresh.now_ms + 1));
    TEST_ASSERT_EQUAL_UINT32(1000, app.ui.balance);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED, app.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(2, app.wallet.next_seq);

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

    meshpay_app_t app_a;
    meshpay_app_t app_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_a, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_b, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t mint;
    make_mint(&mint, master, alice, 900, config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_b, &mint));

    meshpay_app_runtime_t runtime_a;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       &app_a, NULL));
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
                                                       &app_b, NULL));
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
                      app_b.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(120, app_b.ui.last_amount);
    TEST_ASSERT_EQUAL_UINT32(120, app_b.ui.balance);
    TEST_ASSERT_TRUE(app_b.payments.has_last_received);
    TEST_ASSERT_EQUAL_UINT32(120, app_b.payments.last_received_tx.amount);
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

    meshpay_app_t app_a;
    meshpay_app_t app_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_a,
                                               destination_a->hash,
                                               &identity_a,
                                               &config,
                                               1,
                                               true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&app_b,
                                               destination_b->hash,
                                               &identity_b,
                                               &config,
                                               1,
                                               true));

    meshpay_tx_t mint;
    make_mint(&mint, destination_a->hash, destination_a->hash, 700,
              config.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_a, &mint));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&app_b, &mint));

    meshpay_app_runtime_t runtime_a;
    meshpay_app_runtime_t runtime_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_a,
                                                       &app_a,
                                                       NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&runtime_b,
                                                       &app_b,
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
    TEST_ASSERT_EQUAL_UINT8(1, app_a.ui.network_peers);

    TEST_ASSERT_EQUAL(ESP_OK, rns_node_announce(&node_a,
                                                alias_a,
                                                sizeof(alias_a) - 1));
    process_reticulum_until_idle(&runtime_a);
    process_reticulum_until_idle(&runtime_b);
    TEST_ASSERT_EQUAL_UINT8(1, app_b.ui.network_peers);

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
                      app_a.ui.feedback);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
                      app_b.ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(606, app_a.ui.balance);
    TEST_ASSERT_EQUAL_UINT32(90, app_b.ui.balance);
    TEST_ASSERT_EQUAL_STRING("Bob", app_a.ui.last_peer_label);
    TEST_ASSERT_EQUAL_STRING("Alice", app_b.ui.last_peer_label);
    TEST_ASSERT_FALSE(app_a.payments.has_pending);
    TEST_ASSERT_EQUAL_UINT32(90, app_b.ui.last_amount);
    TEST_ASSERT_TRUE(app_b.payments.has_last_received);

    uint32_t balance = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_get_balance(&config,
                                                           &app_b.dag,
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

    meshpay_app_t full;
    meshpay_app_t slow;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&full, alice, &identity_a,
                                               &config, 1, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_init(&slow, bob, &identity_b,
                                               &config, 1, true));

    meshpay_tx_t tx0;
    meshpay_tx_t tx1;
    meshpay_tx_t tx2;
    make_mint(&tx0, master, alice, 1000, config.currency_id);
    make_transfer(&tx1, 0xa1, alice, bob, 100, 1, tx0.id);
    make_transfer(&tx2, 0xb1, bob, alice, 40, 1, tx1.id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&full, &tx0));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&full, &tx1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&full, &tx2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_seed_tx(&slow, &tx0));
    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(&full.dag));
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_dag_count(&slow.dag));

    meshpay_app_runtime_t slow_runtime;
    meshpay_app_runtime_t full_runtime;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&slow_runtime,
                                                       &slow, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_app_runtime_init(&full_runtime,
                                                       &full, NULL));

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
                      meshpay_dag_sync_build_summary(&full.dag,
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
    TEST_ASSERT_EQUAL_UINT16(1, request_known_count);
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

    TEST_ASSERT_EQUAL_UINT32(3, meshpay_dag_count(&slow.dag));
    TEST_ASSERT_TRUE(meshpay_dag_contains(&slow.dag, tx2.id));
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
