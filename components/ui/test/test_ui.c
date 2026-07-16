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

/* --- Palier D2 : écrans portables création/rejointe/code --- */

static bool view_has_action(const meshpay_ui_view_t *view,
                            meshpay_ui_action_t action)
{
    for (uint8_t i = 0; i < view->action_count; ++i) {
        if (view->actions[i] == action) {
            return true;
        }
    }
    return false;
}

/* L'écran JOIN accumule un code texte (input_char), gère backspace/clear et
 * n'active la confirmation que si la saisie est non vide. */
TEST_CASE("ui join code screen accumulates invite code text", "[ui][d2]")
{
    /* E3 : la saisie manuelle vit sur l'ecran de repli JOIN_CODE (JOIN est
     * devenu la liste des monnaies decouvertes). */
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_JOIN_CODE));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, 'A'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, 'B'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, '7'));

    char out[MESHPAY_UI_TEXT_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_text_entry(&ui, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("AB7", out);
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_backspace(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_text_entry(&ui, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("AB", out);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_clear_entry(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_text_entry(&ui, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Rejoindre", view.title);
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_CONFIRM));
}

/* input_char route les chiffres vers la saisie numérique (PIN) et ignore le
 * reste : un seul chemin clavier pour tous les écrans. */
TEST_CASE("ui input_char routes digits on numeric screens", "[ui][d2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, false); /* SETUP_PIN */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, '1'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, '2'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, 'x')); /* ignoré */

    char pin[MESHPAY_UI_PIN_ENTRY_MAX + 1];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_pin_entry(&ui, pin, sizeof(pin), &len));
    TEST_ASSERT_EQUAL_STRING("12", pin);
    TEST_ASSERT_EQUAL_UINT(2, len);
}

/* Le sous-menu monnaie propose Créer/Rejoindre quand on n'est pas membre. */
TEST_CASE("ui currency menu offers create or join when idle", "[ui][d2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_join_state(&ui, MESHPAY_UI_JOIN_IDLE));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_CURRENCY_MENU));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Monnaie", view.title);
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_CREATE));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_JOIN));
    TEST_ASSERT_FALSE(view_has_action(&view, MESHPAY_UI_ACTION_SHOW_CODE));
}

/* Une fois membre, le sous-menu montre le nom de la monnaie, le solde et l'accès
 * au code (plus de Créer/Rejoindre). */
TEST_CASE("ui currency menu shows code and balance when member", "[ui][d2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_join_state(&ui, MESHPAY_UI_JOIN_MEMBER));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_currency(&ui, "Minimistan"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_balance(&ui, 250));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_CURRENCY_MENU));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Minimistan", view.primary);
    TEST_ASSERT_EQUAL_STRING("Solde 250", view.secondary);
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_SHOW_CODE));
    TEST_ASSERT_FALSE(view_has_action(&view, MESHPAY_UI_ACTION_CREATE));
    TEST_ASSERT_FALSE(view_has_action(&view, MESHPAY_UI_ACTION_JOIN));
}

/* L'écran code fondateur affiche le code d'invitation détenu. */
TEST_CASE("ui founder code screen displays the invite code", "[ui][d2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_invite_code(&ui, "ABCD-EFGH-JKMN-PQRS-TV"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_FOUNDER_CODE));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Code monnaie", view.title);
    TEST_ASSERT_EQUAL_STRING("ABCD-EFGH-JKMN-PQRS-TV", view.primary);
}

/* L'écran Réseau expose l'entrée vers le sous-menu monnaie. */
TEST_CASE("ui network screen exposes the currency entry", "[ui][d2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_CURRENCY));
}

/* --- Palier D3 : wizard de création (écran unique + curseur de champ) --- */

/* Le wizard démarre avec les défauts sur le premier champ (Nom). */
TEST_CASE("ui wizard begins with defaults on the first field", "[ui][d3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_begin(&ui));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_CREATE, ui.screen);
    TEST_ASSERT_EQUAL_UINT8(0, ui.wizard.field);
    TEST_ASSERT_EQUAL_UINT32(100, ui.wizard.initial_credit); /* défaut */
    TEST_ASSERT_EQUAL_UINT64(0, ui.wizard.max_supply);
    TEST_ASSERT_EQUAL_STRING("", ui.wizard.name);

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Creer monnaie", view.title);
    TEST_ASSERT_EQUAL_STRING("Nom: (vide)", view.primary);
    TEST_ASSERT_EQUAL_STRING("Champ 1/6", view.secondary);
}

/* Édition par curseur : texte sur Nom, et sur un champ numérique la 1re frappe
 * remplace le défaut (pas de concaténation), backspace retire un chiffre. */
TEST_CASE("ui wizard edits fields by cursor with pristine number replace", "[ui][d3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_begin(&ui));

    meshpay_ui_input_char(&ui, 'M');
    meshpay_ui_input_char(&ui, 'i');
    meshpay_ui_input_char(&ui, 'n');
    TEST_ASSERT_EQUAL_STRING("Min", ui.wizard.name);

    /* -> Champ 2 = Credit (défaut 100). */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_next_field(&ui)); /* Symbole */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_next_field(&ui)); /* Credit */
    TEST_ASSERT_EQUAL_UINT8(2, ui.wizard.field);
    TEST_ASSERT_EQUAL_UINT32(100, ui.wizard.initial_credit);

    meshpay_ui_input_digit(&ui, 2);
    TEST_ASSERT_EQUAL_UINT32(2, ui.wizard.initial_credit); /* remplacé, pas 1002 */
    meshpay_ui_input_digit(&ui, 5);
    meshpay_ui_input_digit(&ui, 0);
    TEST_ASSERT_EQUAL_UINT32(250, ui.wizard.initial_credit);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_backspace(&ui));
    TEST_ASSERT_EQUAL_UINT32(25, ui.wizard.initial_credit);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_prev_field(&ui));
    TEST_ASSERT_EQUAL_UINT8(1, ui.wizard.field);
}

/* Le curseur de champ est clampé aux deux extrémités. */
TEST_CASE("ui wizard clamps the field cursor at both ends", "[ui][d3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_begin(&ui));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_prev_field(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, ui.wizard.field); /* reste au premier */

    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_next_field(&ui));
    }
    TEST_ASSERT_EQUAL_UINT8(MESHPAY_UI_WIZARD_FIELD_COUNT - 1, ui.wizard.field);
}

/* La confirmation exige un nom ; le wizard porte bien les valeurs des 6 champs. */
TEST_CASE("ui wizard confirm requires a name and holds all values", "[ui][d3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_begin(&ui));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_FALSE(view.confirm_enabled); /* pas de nom -> pas de création */

    const char *name = "Minimistan";
    for (const char *p = name; *p != '\0'; ++p) {
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, *p));
    }
    TEST_ASSERT_EQUAL_STRING("Minimistan", ui.wizard.name);

    for (int i = 0; i < 5; ++i) { /* -> Champ 5 = Fonte bps */
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_wizard_next_field(&ui));
    }
    TEST_ASSERT_EQUAL_UINT8(5, ui.wizard.field);
    meshpay_ui_input_digit(&ui, 1);
    meshpay_ui_input_digit(&ui, 5);
    meshpay_ui_input_digit(&ui, 0);
    TEST_ASSERT_EQUAL_UINT16(150, ui.wizard.demurrage_bps);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_TRUE(view.confirm_enabled);
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_CONFIRM));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_NEXT_FIELD));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Palier E3 — écran Rejoindre = liste des monnaies découvertes
 * ══════════════════════════════════════════════════════════════════════════ */

/* Petit constructeur local d'entrée découverte. */
static void make_discovered(meshpay_ui_discovered_entry_t *e,
                            const char *name, const char *fingerprint)
{
    memset(e, 0, sizeof(*e));
    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    (void)snprintf(e->fingerprint, sizeof(e->fingerprint), "%s", fingerprint);
}

TEST_CASE("ui join screen lists discovered currencies", "[ui][e3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    meshpay_ui_discovered_entry_t list[2];
    make_discovered(&list[0], "Alpha", "A1B2C3D4");
    make_discovered(&list[1], "Beta", "0BADF00D");
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_discovered(&ui, list, 2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_JOIN));
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Rejoindre", view.title);
    /* Sélection courante en primary : nom + empreinte anti-usurpation. */
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "Alpha"));
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "A1B2C3D4"));
    TEST_ASSERT_NOT_NULL(strstr(view.secondary, "1/2"));
    /* Les deux monnaies listées en détail, marqueur sur la sélection. */
    TEST_ASSERT_NOT_NULL(strstr(view.detail_lines[0], "Alpha"));
    TEST_ASSERT_EQUAL('>', view.detail_lines[0][0]);
    TEST_ASSERT_NOT_NULL(strstr(view.detail_lines[1], "Beta"));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_CONFIRM));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_NEXT_DISCOVERED));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_JOIN_CODE));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_HOME));
}

TEST_CASE("ui join screen without discovery shows search state", "[ui][e3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_JOIN));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Recherche...", view.primary);
    TEST_ASSERT_FALSE(view_has_action(&view, MESHPAY_UI_ACTION_CONFIRM));
    /* Le repli saisie manuelle reste proposé. */
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_JOIN_CODE));
}

TEST_CASE("ui next discovered cycles selection and clamps on shrink",
          "[ui][e3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    meshpay_ui_discovered_entry_t list[2];
    make_discovered(&list[0], "Alpha", "A1B2C3D4");
    make_discovered(&list[1], "Beta", "0BADF00D");
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_discovered(&ui, list, 2));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_JOIN));

    /* Cycle : Alpha -> Beta -> Alpha. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_discovered(&ui));
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "Beta"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_discovered(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "Alpha"));

    /* La liste rétrécit sous la sélection : clamp sans débordement. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_discovered(&ui)); /* sel = 1 */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_discovered(&ui, list, 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "Alpha"));

    /* Liste vide : next refuse. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_discovered(&ui, NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, meshpay_ui_next_discovered(&ui));
}

TEST_CASE("ui set discovered truncates above capacity", "[ui][e3]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    meshpay_ui_discovered_entry_t list[MESHPAY_UI_DISCOVERED_MAX + 1];
    for (uint8_t i = 0; i < MESHPAY_UI_DISCOVERED_MAX + 1; ++i) {
        char name[MESHPAY_UI_CURRENCY_NAME_MAX];
        (void)snprintf(name, sizeof(name), "M%u", (unsigned)i);
        make_discovered(&list[i], name, "00000000");
    }
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_discovered(
                                  &ui, list, MESHPAY_UI_DISCOVERED_MAX + 1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_JOIN));
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.secondary, "1/4"));
}

/* --- Chantier migration NVS (M4) : stockage HS visible + reset 2 temps --- */

TEST_CASE("ui storage alert shows in footer and network screen", "[ui][m4]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_storage_status(&ui,
                                                    MESHPAY_UI_STORAGE_CORRUPT));

    /* HOME : l'alerte persiste dans le footer de tous les écrans. */
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.footer, "Stockage HS"));

    /* Réseau : motif détaillé + bouton de réinitialisation. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.detail_lines[0], "illisibles"));
    TEST_ASSERT_TRUE(view_has_action(&view, MESHPAY_UI_ACTION_STORAGE_RESET));

    /* Retour à un stockage sain : alerte et bouton disparaissent. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_storage_status(&ui,
                                                    MESHPAY_UI_STORAGE_OK));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("", view.footer);
    TEST_ASSERT_FALSE(view_has_action(&view, MESHPAY_UI_ACTION_STORAGE_RESET));
}

TEST_CASE("ui storage reset needs two taps and nav disarms", "[ui][m4]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);

    /* Stockage sain : le reset est refusé (pas de bouton à l'écran). */
    bool confirmed = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_ui_storage_reset_request(&ui, &confirmed));
    TEST_ASSERT_FALSE(confirmed);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_storage_status(&ui,
                                                    MESHPAY_UI_STORAGE_LEGACY));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));

    /* 1er tap : armé, pas confirmé — le bouton passe à « Confirmer? ». */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_storage_reset_request(&ui, &confirmed));
    TEST_ASSERT_FALSE(confirmed);
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    bool armed_label = false;
    for (uint8_t i = 0; i < view.action_count; ++i) {
        if (view.actions[i] == MESHPAY_UI_ACTION_STORAGE_RESET &&
            strcmp(view.action_labels[i], "Confirmer?") == 0) {
            armed_label = true;
        }
    }
    TEST_ASSERT_TRUE(armed_label);

    /* Naviguer désarme : le tap suivant re-arme au lieu de confirmer. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_HOME));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_storage_reset_request(&ui, &confirmed));
    TEST_ASSERT_FALSE(confirmed);

    /* 2e tap consécutif : confirmé et désarmé. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_storage_reset_request(&ui, &confirmed));
    TEST_ASSERT_TRUE(confirmed);
    TEST_ASSERT_FALSE(ui.storage_reset_armed);
}

TEST_CASE("ui storage write failure feedback overrides footer", "[ui][m4]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_storage_status(&ui,
                                                    MESHPAY_UI_STORAGE_LEGACY));
    /* Une écriture vient d'échouer (rejointe refusée) : le feedback transient
     * prend le footer, l'alerte persistante reviendra à la prochaine nav. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_storage_write_failed(&ui));
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_EQUAL_STRING("Echec: stockage HS", view.footer);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.footer, "Stockage HS"));
}

/* --- Chantier erreurs UI invisibles (U1) : causes typées d'échec --- */

TEST_CASE("ui action failure causes show their label in footer", "[ui][u1]")
{
    struct {
        meshpay_ui_feedback_t cause;
        const char *label;
    } cases[] = {
        { MESHPAY_UI_FEEDBACK_BAD_INVITE_CODE, "Code invalide" },
        { MESHPAY_UI_FEEDBACK_ALREADY_MEMBER, "Deja membre" },
        { MESHPAY_UI_FEEDBACK_CREATE_REFUSED, "Creation refusee" },
        { MESHPAY_UI_FEEDBACK_DISCOVERY_REFUSED, "Rejointe refusee" },
        { MESHPAY_UI_FEEDBACK_JOIN_EXPIRED, "Rejointe expiree" },
        { MESHPAY_UI_FEEDBACK_ACTION_FAILED, "Echec de l'action" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        meshpay_ui_state_t ui;
        meshpay_ui_init(&ui, true);
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_ui_on_action_failed(&ui, cases[i].cause));
        meshpay_ui_view_t view;
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
        TEST_ASSERT_NOT_NULL(strstr(view.footer, cases[i].label));
        /* Transient : la navigation efface le feedback. */
        TEST_ASSERT_EQUAL(ESP_OK,
                          meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_NETWORK));
        TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
        TEST_ASSERT_NULL(strstr(view.footer, cases[i].label));
    }
}

TEST_CASE("ui action failure rejects success feedback values", "[ui][u1]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    /* Les feedbacks de succès ne passent pas par ce canal. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_ui_on_action_failed(
                          &ui, MESHPAY_UI_FEEDBACK_PAYMENT_SENT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_ui_on_action_failed(&ui,
                                                  MESHPAY_UI_FEEDBACK_NONE));
    TEST_ASSERT_EQUAL(MESHPAY_UI_FEEDBACK_NONE, ui.feedback);
}

TEST_CASE("ui action failure has priority over storage alert", "[ui][u1]")
{
    /* Même règle que M4 : le transient (motif de l'échec du geste) prend le
     * footer, l'alerte storage persistante revient après la nav. */
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_storage_status(&ui,
                                                    MESHPAY_UI_STORAGE_LEGACY));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_action_failed(
                                  &ui, MESHPAY_UI_FEEDBACK_ALREADY_MEMBER));
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.footer, "Deja membre"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_HOME));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.footer, "Stockage HS"));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Chantier crédit fondateur (K2) — écran Créditer + gating fondateur
 * ══════════════════════════════════════════════════════════════════════════ */

/* L'entrée « Crediter » du menu Monnaie n'existe QUE chez le fondateur
 * (autorité MINT poussée par l'app) : un membre ne la voit jamais. */
TEST_CASE("ui currency menu shows credit action to founder only", "[ui][k2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_join_state(&ui, MESHPAY_UI_JOIN_MEMBER));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_CURRENCY_MENU));

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    for (uint8_t i = 0; i < view.action_count; ++i) {
        TEST_ASSERT_NOT_EQUAL(MESHPAY_UI_ACTION_CREDIT, view.actions[i]);
    }

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_set_founder(&ui, true));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    bool found = false;
    for (uint8_t i = 0; i < view.action_count; ++i) {
        if (view.actions[i] == MESHPAY_UI_ACTION_CREDIT) {
            found = true;
            TEST_ASSERT_EQUAL_STRING("Crediter", view.action_labels[i]);
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* L'écran Créditer : montant saisi aux chiffres (backspace compris), membre
 * cible cyclable, confirmation gated par (montant > 0 ET liste non vide). */
TEST_CASE("ui credit screen types amount and cycles members", "[ui][k2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_CREDIT));

    /* Liste vide : la confirmation reste interdite, l'écran l'affiche. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_credit_member(&ui, "", NULL, 0, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, meshpay_ui_next_credit_member(&ui));
    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.secondary, "Aucun membre"));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui));

    /* Deux membres poussés par l'app : saisie du montant au clavier. Le
     * COMPTE affiché est mémorisé (anti-TOCTOU : c'est LUI qui sera crédité,
     * jamais une re-résolution par index). Une liste non vide sans compte
     * est refusée. */
    uint8_t acc[MESHPAY_TX_DESTINATION_HASH_SIZE];
    memset(acc, 0x42, sizeof(acc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_ui_set_credit_member(&ui, "panda agile", NULL,
                                                   0, 2));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_credit_member(&ui, "panda agile", acc,
                                                   0, 2));
    TEST_ASSERT_EQUAL_MEMORY(acc, ui.credit_member_account, sizeof(acc));
    TEST_ASSERT_FALSE(meshpay_ui_confirm_enabled(&ui)); /* montant nul */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, '4'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, '2'));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_input_char(&ui, 'x')); /* ignoré */
    TEST_ASSERT_EQUAL_UINT32(42, ui.draft_amount);
    TEST_ASSERT_TRUE(meshpay_ui_confirm_enabled(&ui));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.primary, "Montant 42"));
    TEST_ASSERT_NOT_NULL(strstr(view.secondary, "panda agile"));
    TEST_ASSERT_NOT_NULL(strstr(view.secondary, "1/2"));

    /* Cycle des membres + backspace sur le montant. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_credit_member(&ui));
    TEST_ASSERT_EQUAL_UINT8(1, ui.selected_credit_member);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_next_credit_member(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, ui.selected_credit_member); /* cyclique */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_backspace(&ui));
    TEST_ASSERT_EQUAL_UINT32(4, ui.draft_amount);

    /* La liste qui se vide efface aussi le compte mémorisé (aucun MINT ne
     * doit pouvoir partir vers une cible fantôme). */
    uint8_t zero[MESHPAY_TX_DESTINATION_HASH_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_ui_set_credit_member(&ui, "", NULL, 0, 0));
    TEST_ASSERT_EQUAL_MEMORY(zero, ui.credit_member_account, sizeof(zero));
}

/* Le succès de l'émission remonte en footer (transient, effacé à la nav) ;
 * le canal des échecs refuse cette valeur de succès. */
TEST_CASE("ui credit sent feedback is transient", "[ui][k2]")
{
    meshpay_ui_state_t ui;
    meshpay_ui_init(&ui, true);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_CREDIT));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_on_credit_sent(&ui, 50));
    TEST_ASSERT_EQUAL(MESHPAY_UI_SCREEN_CREDIT, ui.screen);
    TEST_ASSERT_EQUAL_UINT32(50, ui.last_amount);

    meshpay_ui_view_t view;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NOT_NULL(strstr(view.footer, "Credit envoye"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_nav(&ui, MESHPAY_UI_SCREEN_HOME));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_ui_build_view(&ui, &view));
    TEST_ASSERT_NULL(strstr(view.footer, "Credit envoye"));

    /* Succès ≠ canal des échecs (même règle que PAYMENT_SENT). */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_ui_on_action_failed(
                          &ui, MESHPAY_UI_FEEDBACK_CREDIT_SENT));
}
