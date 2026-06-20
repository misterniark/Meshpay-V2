#include "meshpay/ui.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>

TEST_CASE("ui starts on setup until pin exists", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, false);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_SETUP_PIN, ui.screen);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_HOME));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_pin_result(&ui, true, false));
    TEST_ASSERT_TRUE(ui.has_pin);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HOME, ui.screen);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_PAY));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_PAY, ui.screen);
}

TEST_CASE("ui tracks pin failures and locked state", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_pin_result(&ui, false, false));
    TEST_ASSERT_EQUAL_UINT8(1, ui.pin_failures);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PIN_ERROR, ui.feedback);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_pin_result(&ui, false, true));
    TEST_ASSERT_TRUE(ui.pin_locked);
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_LOCKED, ui.screen);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_HOME));
}

TEST_CASE("ui maps payment feedback to expected screens", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_on_payment_feedback(
                          &ui, MESHPAY_PAYMENT_FEEDBACK_SENT, 125));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_PAY, ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_SENT, ui.feedback);
    TEST_ASSERT_EQUAL_UINT32(125, ui.last_amount);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_on_payment_feedback(
                          &ui, MESHPAY_PAYMENT_FEEDBACK_RECEIVED, 125));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_RECEIVE, ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED, ui.feedback);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_on_payment_feedback(
                          &ui, MESHPAY_PAYMENT_FEEDBACK_ACKED, 125));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HISTORY, ui.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED, ui.feedback);
}

TEST_CASE("ui updates balance and network state", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_balance(&ui, 1234));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_network_peers(&ui, 3));
    TEST_ASSERT_EQUAL_UINT32(1234, ui.balance);
    TEST_ASSERT_EQUAL_UINT8(3, ui.network_peers);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_NETWORK, ui.screen);
}

TEST_CASE("ui builds dag monitor view from read-only status", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init_monitor(&ui);

    meshpay_ui_dag_monitor_status_t status = {
        .lora_ready = true,
        .battery_available = true,
        .health_score = 93,
        .battery_percent = 87,
        .peer_count = 2,
        .alert_count = 1,
        .battery_mv = 4012,
        .lora_frames = 44,
        .rns_packets = 21,
        .dag_summaries = 7,
        .dag_requests = 3,
        .resource_frames = 5,
        .dag_batches = 2,
        .tx_advertised = 18,
        .tx_observed = 12,
        .malformed_lora_frames = 1,
        .malformed_rns_packets = 2,
        .malformed_dag_sync = 3,
        .malformed_frames = 1,
        .duplicate_packets = 4,
        .peer_regressions = 5,
        .peer_summary_without_tips = 6,
        .alert_line_count = 1,
    };
    (void)snprintf(status.radio_label,
                   sizeof(status.radio_label),
                   "LoRa OK");
    (void)snprintf(status.alert_lines[0],
                   sizeof(status.alert_lines[0]),
                   "WARN 919293 TX REGRESS TX5");

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_dag_monitor(&ui, &status));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_DAG_MONITOR, view.screen);
    TEST_ASSERT_EQUAL(MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW,
                      view.dag_monitor_page);
    TEST_ASSERT_EQUAL_STRING("Moniteur DAG", view.title);
    TEST_ASSERT_EQUAL_STRING("Sante 93", view.primary);
    TEST_ASSERT_EQUAL_STRING("LoRa OK / 2 pairs", view.secondary);
    TEST_ASSERT_EQUAL_UINT8(7, view.detail_count);
    TEST_ASSERT_EQUAL_STRING("LoRa frames 44", view.detail_lines[0]);
    TEST_ASSERT_EQUAL_STRING("RNS packets 21", view.detail_lines[1]);
    TEST_ASSERT_EQUAL_STRING("TX reseau 18", view.detail_lines[4]);
    TEST_ASSERT_EQUAL_STRING("TX batch 12", view.detail_lines[5]);
    TEST_ASSERT_EQUAL_STRING("", view.footer);
    TEST_ASSERT_EQUAL_UINT8(4, view.action_count);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_MONITOR_ALERTS, view.actions[2]);
    TEST_ASSERT_FALSE(view.confirm_enabled);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_monitor_page(
                          &ui, MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL(MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS,
                      view.dag_monitor_page);
    TEST_ASSERT_EQUAL_STRING("Alertes", view.title);
    TEST_ASSERT_EQUAL_STRING("1 alertes", view.primary);
    TEST_ASSERT_EQUAL_STRING("Err LoRa 1 RNS 2", view.detail_lines[0]);
    TEST_ASSERT_EQUAL_STRING("Err DAG 3 dup 4", view.detail_lines[1]);
    TEST_ASSERT_EQUAL_STRING("Regressions TX 5", view.detail_lines[2]);
    TEST_ASSERT_EQUAL_STRING("Summary sans tips 6", view.detail_lines[3]);
    TEST_ASSERT_EQUAL_UINT8(5, view.detail_count);
    TEST_ASSERT_EQUAL_STRING("WARN 919293 TX REGRESS TX5",
                             view.detail_lines[4]);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_ui_monitor_page(
                          &ui, MESHPAY_UI_DAG_MONITOR_PAGE_MAX));

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_monitor_page(
                          &ui, MESHPAY_UI_DAG_MONITOR_PAGE_RADIO));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Radio LoRa", view.title);
    TEST_ASSERT_EQUAL_STRING("BATT 87% / 4012 mV", view.detail_lines[1]);
}

TEST_CASE("ui builds home view with primary actions", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_balance(&ui, 1234));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_network_peers(&ui, 2));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HOME, view.screen);
    TEST_ASSERT_EQUAL_STRING("MeshPay", view.title);
    TEST_ASSERT_EQUAL_STRING("Solde 1234", view.primary);
    TEST_ASSERT_EQUAL_STRING("2 peers", view.secondary);
    TEST_ASSERT_EQUAL_UINT8(4, view.action_count);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_PAY, view.actions[0]);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_RECEIVE, view.actions[1]);
    TEST_ASSERT_EQUAL_STRING("Moi", view.action_labels[1]);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_HISTORY, view.actions[2]);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_NETWORK, view.actions[3]);
}

TEST_CASE("ui builds me view with local alias and id", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_balance(&ui, 42));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_network_peers(&ui, 2));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_local_identity(&ui,
                                                    "castor precis",
                                                    "8024a936"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_RECEIVE));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Moi", view.title);
    TEST_ASSERT_EQUAL_STRING("castor precis", view.primary);
    TEST_ASSERT_EQUAL_STRING("ID 8024a936 / 2 peers", view.secondary);
    TEST_ASSERT_EQUAL_UINT8(4, view.action_count);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_HOME, view.actions[0]);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_PAY, view.actions[1]);
}

TEST_CASE("ui captures setup pin entry and shows it in view", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, false);

    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 3));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 4));
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));

    char pin[MESHPAY_UI_PIN_ENTRY_MAX + 1];
    size_t pin_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_pin_entry(&ui, pin, sizeof(pin),
                                                   &pin_len));
    TEST_ASSERT_EQUAL_UINT32(4, pin_len);
    TEST_ASSERT_EQUAL_STRING("1234", pin);

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Creer PIN", view.title);
    TEST_ASSERT_EQUAL_STRING("PIN 1234", view.primary);
    TEST_ASSERT_TRUE(view.confirm_enabled);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_backspace(&ui));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL_UINT8(3, ui.pin_entry_len);
}

TEST_CASE("ui captures payment amount edits", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_payment_peer(&ui, "renard malin", 0, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_PAY));

    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_digit(&ui, 5));
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL_UINT32(125, ui.draft_amount);

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Montant", view.title);
    TEST_ASSERT_EQUAL_STRING("Montant 125", view.primary);
    TEST_ASSERT_EQUAL_STRING("Pour renard malin", view.secondary);
    TEST_ASSERT_EQUAL(MESHPAY_UI_ACTION_CONFIRM, view.actions[0]);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_backspace(&ui));
    TEST_ASSERT_EQUAL_UINT32(12, ui.draft_amount);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_clear_entry(&ui));
    TEST_ASSERT_EQUAL_UINT32(0, ui.draft_amount);
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));
}

TEST_CASE("ui chooses payee before amount", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_PAYEE));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Payer a", view.title);
    TEST_ASSERT_EQUAL_STRING("Aucun pair", view.primary);
    TEST_ASSERT_FALSE(view.confirm_enabled);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_payment_peer(&ui, "loup brave", 0, 2));
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("loup brave", view.primary);
    TEST_ASSERT_EQUAL_STRING("1/2 destinataires", view.secondary);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_payment_peer(&ui));
    TEST_ASSERT_EQUAL_UINT8(1, ui.selected_payment_peer);
}

TEST_CASE("ui adds peer alias to history", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_history_peer(&ui, "loup brave"));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_on_payment_feedback(
                          &ui, MESHPAY_PAYMENT_FEEDBACK_ACKED, 7));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_HISTORY, view.screen);
    TEST_ASSERT_EQUAL_STRING("Avec loup brave", view.secondary);
}

TEST_CASE("ui locked state rejects user entry", "[ui]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_pin_result(&ui, false, true));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, meshpay_ui_input_digit(&ui, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, meshpay_ui_backspace(&ui));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_LOCKED, view.screen);
    TEST_ASSERT_EQUAL_STRING("PIN verrouille", view.primary);
    TEST_ASSERT_EQUAL_UINT8(0, view.action_count);
}
