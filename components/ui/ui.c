#include "meshpay/ui.h"

#include <stdio.h>
#include <string.h>

static void view_text(char out[MESHPAY_UI_TEXT_MAX], const char *text)
{
    if (out == NULL) {
        return;
    }
    if (text == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out, MESHPAY_UI_TEXT_MAX, "%s", text);
}

static void add_action(meshpay_ui_view_t *view,
                       meshpay_ui_action_t action,
                       const char *label)
{
    if (view == NULL || view->action_count >= MESHPAY_UI_ACTION_MAX) {
        return;
    }
    uint8_t index = view->action_count++;
    view->actions[index] = action;
    (void)snprintf(view->action_labels[index],
                   MESHPAY_UI_ACTION_LABEL_MAX,
                   "%s",
                   label == NULL ? "" : label);
}

static void add_detail(meshpay_ui_view_t *view, const char *text)
{
    if (view == NULL || view->detail_count >= MESHPAY_UI_DETAIL_LINE_MAX) {
        return;
    }
    view_text(view->detail_lines[view->detail_count], text);
    view->detail_count++;
}

static void add_monitor_nav_actions(meshpay_ui_view_t *view)
{
    add_action(view, MESHPAY_UI_ACTION_MONITOR_OVERVIEW, "Vue");
    add_action(view, MESHPAY_UI_ACTION_MONITOR_PEERS, "Pairs");
    add_action(view, MESHPAY_UI_ACTION_MONITOR_ALERTS, "Alertes");
    add_action(view, MESHPAY_UI_ACTION_MONITOR_RADIO, "Radio");
}

static const char *feedback_text(meshpay_ui_feedback_t feedback)
{
    switch (feedback) {
    case MESHPAY_UI_FEEDBACK_PAYMENT_LOCKED:
        return "Paiement verrouille";
    case MESHPAY_UI_FEEDBACK_PAYMENT_SENT:
        return "Paiement envoye";
    case MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED:
        return "Paiement recu";
    case MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED:
        return "Paiement confirme";
    case MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED:
        return "Paiement refuse";
    case MESHPAY_UI_FEEDBACK_PIN_ERROR:
        return "PIN invalide";
    case MESHPAY_UI_FEEDBACK_NONE:
    default:
        return "";
    }
}

static esp_err_t append_pin_digit(meshpay_ui_state_t *ui, uint8_t digit)
{
    if (ui->pin_entry_len >= MESHPAY_UI_PIN_ENTRY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ui->pin_entry[ui->pin_entry_len++] = (char)('0' + digit);
    ui->pin_entry[ui->pin_entry_len] = '\0';
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

static esp_err_t append_amount_digit(meshpay_ui_state_t *ui, uint8_t digit)
{
    if (ui->draft_amount > (UINT32_MAX - digit) / 10U) {
        return ESP_ERR_INVALID_SIZE;
    }
    ui->draft_amount = ui->draft_amount * 10U + digit;
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

/* Palier D : ajoute un caractère imprimable à la saisie texte (code/nom). Ignore
 * les caractères de contrôle ; borne à la capacité du buffer. */
static esp_err_t append_text_char(meshpay_ui_state_t *ui, char c)
{
    if (c < 0x20 || c == 0x7f) {
        return ESP_OK; /* touche de contrôle -> ignorée sans erreur */
    }
    if (ui->text_entry_len >= MESHPAY_UI_TEXT_MAX - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    ui->text_entry[ui->text_entry_len++] = c;
    ui->text_entry[ui->text_entry_len] = '\0';
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

void meshpay_ui_init(meshpay_ui_state_t *ui, bool has_pin)
{
    if (ui == NULL) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->has_pin = has_pin;
    ui->screen = has_pin ? MESHPAY_UI_SCREEN_HOME
                         : MESHPAY_UI_SCREEN_SETUP_PIN;
}

void meshpay_ui_init_monitor(meshpay_ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->has_pin = true;
    ui->screen = MESHPAY_UI_SCREEN_DAG_MONITOR;
    ui->dag_monitor_page = MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW;
    ui->dag_monitor.health_score = 100;
    (void)snprintf(ui->dag_monitor.radio_label,
                   sizeof(ui->dag_monitor.radio_label),
                   "LoRa attente");
}

esp_err_t meshpay_ui_set_balance(meshpay_ui_state_t *ui, uint32_t balance)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->balance = balance;
    return ESP_OK;
}

esp_err_t meshpay_ui_set_network_peers(meshpay_ui_state_t *ui, uint8_t peers)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->network_peers = peers;
    return ESP_OK;
}

esp_err_t meshpay_ui_set_dag_monitor(
    meshpay_ui_state_t *ui,
    const meshpay_ui_dag_monitor_status_t *status)
{
    if (ui == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->dag_monitor = *status;
    if (ui->dag_monitor.health_score > 100U) {
        ui->dag_monitor.health_score = 100U;
    }
    if (ui->dag_monitor.peer_line_count >
        MESHPAY_UI_DAG_MONITOR_PEER_LINE_MAX) {
        ui->dag_monitor.peer_line_count =
            MESHPAY_UI_DAG_MONITOR_PEER_LINE_MAX;
    }
    if (ui->dag_monitor.alert_line_count >
        MESHPAY_UI_DAG_MONITOR_ALERT_LINE_MAX) {
        ui->dag_monitor.alert_line_count =
            MESHPAY_UI_DAG_MONITOR_ALERT_LINE_MAX;
    }
    ui->network_peers = ui->dag_monitor.peer_count;
    return ESP_OK;
}

esp_err_t meshpay_ui_monitor_page(meshpay_ui_state_t *ui,
                                  meshpay_ui_dag_monitor_page_t page)
{
    if (ui == NULL || page >= MESHPAY_UI_DAG_MONITOR_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->screen != MESHPAY_UI_SCREEN_DAG_MONITOR) {
        return ESP_ERR_INVALID_STATE;
    }
    ui->dag_monitor_page = page;
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

esp_err_t meshpay_ui_set_local_identity(meshpay_ui_state_t *ui,
                                        const char *alias,
                                        const char *short_id)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(ui->local_alias,
                   sizeof(ui->local_alias),
                   "%s",
                   alias == NULL ? "" : alias);
    (void)snprintf(ui->local_id,
                   sizeof(ui->local_id),
                   "%s",
                   short_id == NULL ? "" : short_id);
    return ESP_OK;
}

esp_err_t meshpay_ui_set_payment_peer(meshpay_ui_state_t *ui,
                                      const char *label,
                                      uint8_t selected_index,
                                      uint8_t peer_count)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->payment_peer_count = peer_count;
    ui->selected_payment_peer =
        peer_count == 0 ? 0 : (uint8_t)(selected_index % peer_count);
    (void)snprintf(ui->payment_peer_label,
                   sizeof(ui->payment_peer_label),
                   "%s",
                   label == NULL ? "" : label);
    return ESP_OK;
}

esp_err_t meshpay_ui_next_payment_peer(meshpay_ui_state_t *ui)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->payment_peer_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ui->selected_payment_peer =
        (uint8_t)((ui->selected_payment_peer + 1U) % ui->payment_peer_count);
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

esp_err_t meshpay_ui_set_history_peer(meshpay_ui_state_t *ui,
                                      const char *label)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(ui->last_peer_label,
                   sizeof(ui->last_peer_label),
                   "%s",
                   label == NULL ? "" : label);
    return ESP_OK;
}

esp_err_t meshpay_ui_nav(meshpay_ui_state_t *ui, meshpay_ui_screen_t screen)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->pin_locked && screen != MESHPAY_UI_SCREEN_LOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ui->has_pin && screen != MESHPAY_UI_SCREEN_SETUP_PIN) {
        return ESP_ERR_INVALID_STATE;
    }
    if (screen > MESHPAY_UI_SCREEN_FOUNDER_CODE) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->screen = screen;
    ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
    return ESP_OK;
}

esp_err_t meshpay_ui_input_digit(meshpay_ui_state_t *ui, uint8_t digit)
{
    if (ui == NULL || digit > 9) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->pin_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (ui->screen) {
    case MESHPAY_UI_SCREEN_SETUP_PIN:
        return append_pin_digit(ui, digit);
    case MESHPAY_UI_SCREEN_PAY:
        return append_amount_digit(ui, digit);
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t meshpay_ui_input_char(meshpay_ui_state_t *ui, char c)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->pin_locked) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (ui->screen) {
    case MESHPAY_UI_SCREEN_JOIN:
        return append_text_char(ui, c); /* saisie texte du code */
    case MESHPAY_UI_SCREEN_SETUP_PIN:
    case MESHPAY_UI_SCREEN_PAY:
        /* Écrans numériques : seuls les chiffres comptent, le reste est ignoré. */
        if (c >= '0' && c <= '9') {
            return meshpay_ui_input_digit(ui, (uint8_t)(c - '0'));
        }
        return ESP_OK;
    default:
        return ESP_OK; /* aucun champ de saisie sur cet écran */
    }
}

esp_err_t meshpay_ui_set_join_state(meshpay_ui_state_t *ui,
                                    meshpay_ui_join_state_t state)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->join_state = state;
    return ESP_OK;
}

esp_err_t meshpay_ui_set_invite_code(meshpay_ui_state_t *ui, const char *code)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(ui->invite_code, sizeof(ui->invite_code), "%s",
                   code == NULL ? "" : code);
    return ESP_OK;
}

esp_err_t meshpay_ui_set_currency(meshpay_ui_state_t *ui, const char *name)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(ui->currency_name, sizeof(ui->currency_name), "%s",
                   name == NULL ? "" : name);
    return ESP_OK;
}

esp_err_t meshpay_ui_text_entry(const meshpay_ui_state_t *ui,
                                char *out,
                                size_t out_len)
{
    if (ui == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len <= ui->text_entry_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, ui->text_entry, ui->text_entry_len + 1U);
    return ESP_OK;
}

esp_err_t meshpay_ui_backspace(meshpay_ui_state_t *ui)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui->pin_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (ui->screen) {
    case MESHPAY_UI_SCREEN_SETUP_PIN:
        if (ui->pin_entry_len > 0) {
            ui->pin_entry[--ui->pin_entry_len] = '\0';
        }
        return ESP_OK;
    case MESHPAY_UI_SCREEN_PAY:
        ui->draft_amount /= 10U;
        return ESP_OK;
    case MESHPAY_UI_SCREEN_JOIN:
        if (ui->text_entry_len > 0) {
            ui->text_entry[--ui->text_entry_len] = '\0';
        }
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t meshpay_ui_clear_entry(meshpay_ui_state_t *ui)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui->draft_amount = 0;
    memset(ui->pin_entry, 0, sizeof(ui->pin_entry));
    ui->pin_entry_len = 0;
    memset(ui->text_entry, 0, sizeof(ui->text_entry));
    ui->text_entry_len = 0;
    return ESP_OK;
}

bool meshpay_ui_confirm_enabled(const meshpay_ui_state_t *ui)
{
    if (ui == NULL || ui->pin_locked) {
        return false;
    }
    if (ui->screen == MESHPAY_UI_SCREEN_SETUP_PIN) {
        return ui->pin_entry_len >= 4 &&
               ui->pin_entry_len <= MESHPAY_UI_PIN_ENTRY_MAX;
    }
    if (ui->screen == MESHPAY_UI_SCREEN_PAYEE) {
        return ui->payment_peer_count > 0;
    }
    if (ui->screen == MESHPAY_UI_SCREEN_PAY) {
        return ui->draft_amount > 0 && ui->payment_peer_count > 0;
    }
    if (ui->screen == MESHPAY_UI_SCREEN_JOIN) {
        return ui->text_entry_len > 0;
    }
    return false;
}

esp_err_t meshpay_ui_pin_entry(const meshpay_ui_state_t *ui,
                               char *out,
                               size_t out_len,
                               size_t *pin_len)
{
    if (ui == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len <= ui->pin_entry_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, ui->pin_entry, ui->pin_entry_len + 1U);
    if (pin_len != NULL) {
        *pin_len = ui->pin_entry_len;
    }
    return ESP_OK;
}

static void build_pin_mask(const meshpay_ui_state_t *ui,
                           char out[MESHPAY_UI_TEXT_MAX])
{
    (void)snprintf(out,
                   MESHPAY_UI_TEXT_MAX,
                   "PIN %s",
                   ui->pin_entry);
}

esp_err_t meshpay_ui_build_view(const meshpay_ui_state_t *ui,
                                meshpay_ui_view_t *view)
{
    if (ui == NULL || view == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(view, 0, sizeof(*view));
    view->screen = ui->screen;
    view->dag_monitor_page = ui->dag_monitor_page;
    view->feedback = ui->feedback;
    view->confirm_enabled = meshpay_ui_confirm_enabled(ui);

    switch (ui->screen) {
    case MESHPAY_UI_SCREEN_SETUP_PIN:
        view_text(view->title, "Creer PIN");
        build_pin_mask(ui, view->primary);
        view_text(view->secondary, ui->pin_entry_len < 4
                                       ? "4 chiffres minimum"
                                       : "PIN pret");
        add_action(view, MESHPAY_UI_ACTION_CONFIRM, "OK");
        add_action(view, MESHPAY_UI_ACTION_BACKSPACE, "Effacer");
        break;
    case MESHPAY_UI_SCREEN_HOME:
        view_text(view->title, "MeshPay");
        (void)snprintf(view->primary, sizeof(view->primary),
                       "Solde %lu", (unsigned long)ui->balance);
        (void)snprintf(view->secondary, sizeof(view->secondary),
                       "%u peers", (unsigned)ui->network_peers);
        add_action(view, MESHPAY_UI_ACTION_PAY, "Payer");
        add_action(view, MESHPAY_UI_ACTION_RECEIVE, "Moi");
        add_action(view, MESHPAY_UI_ACTION_HISTORY, "Hist.");
        add_action(view, MESHPAY_UI_ACTION_NETWORK, "Reseau");
        break;
    case MESHPAY_UI_SCREEN_PAYEE:
        view_text(view->title, "Payer a");
        if (ui->payment_peer_count == 0) {
            view_text(view->primary, "Aucun pair");
            view_text(view->secondary, "Attente annonce");
        } else {
            view_text(view->primary, ui->payment_peer_label);
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "%u/%u destinataires",
                           (unsigned)(ui->selected_payment_peer + 1U),
                           (unsigned)ui->payment_peer_count);
        }
        add_action(view, MESHPAY_UI_ACTION_CONFIRM, "OK");
        add_action(view, MESHPAY_UI_ACTION_NEXT_PEER, "Suiv.");
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        break;
    case MESHPAY_UI_SCREEN_PAY:
        view_text(view->title, "Montant");
        if (ui->draft_amount == 0) {
            view_text(view->primary, "Montant --");
        } else {
            (void)snprintf(view->primary, sizeof(view->primary),
                           "Montant %lu", (unsigned long)ui->draft_amount);
        }
        if (ui->payment_peer_count == 0) {
            view_text(view->secondary, "Cible: aucun pair");
        } else {
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "Pour %s",
                           ui->payment_peer_label);
        }
        add_action(view, MESHPAY_UI_ACTION_CONFIRM, "Envoyer");
        add_action(view, MESHPAY_UI_ACTION_BACKSPACE, "Effacer");
        add_action(view, MESHPAY_UI_ACTION_PAY, "Cible");
        add_action(view, MESHPAY_UI_ACTION_CLEAR, "Vider");
        break;
    case MESHPAY_UI_SCREEN_RECEIVE:
        view_text(view->title, "Moi");
        if (ui->feedback == MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED &&
            ui->last_amount > 0) {
            (void)snprintf(view->primary,
                           sizeof(view->primary),
                           "Recu +%lu",
                           (unsigned long)ui->last_amount);
            if (ui->last_peer_label[0] != '\0') {
                (void)snprintf(view->secondary,
                               sizeof(view->secondary),
                               "De %s",
                               ui->last_peer_label);
            } else {
                view_text(view->secondary, feedback_text(ui->feedback));
            }
        } else {
            view_text(view->primary,
                      ui->local_alias[0] != '\0' ? ui->local_alias : "Alias local");
            if (ui->local_id[0] != '\0') {
                (void)snprintf(view->secondary,
                               sizeof(view->secondary),
                               "ID %s / %u peers",
                               ui->local_id,
                               (unsigned)ui->network_peers);
            } else {
                (void)snprintf(view->secondary,
                               sizeof(view->secondary),
                               "Solde %lu / %u peers",
                               (unsigned long)ui->balance,
                               (unsigned)ui->network_peers);
            }
        }
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        add_action(view, MESHPAY_UI_ACTION_PAY, "Payer");
        add_action(view, MESHPAY_UI_ACTION_HISTORY, "Hist.");
        add_action(view, MESHPAY_UI_ACTION_NETWORK, "Reseau");
        break;
    case MESHPAY_UI_SCREEN_HISTORY:
        view_text(view->title, "Historique");
        if (ui->last_amount == 0) {
            view_text(view->primary, "Aucune operation");
        } else {
            (void)snprintf(view->primary, sizeof(view->primary),
                           "Dernier %lu", (unsigned long)ui->last_amount);
        }
        if (ui->last_peer_label[0] != '\0') {
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "Avec %s",
                           ui->last_peer_label);
        } else {
            view_text(view->secondary, feedback_text(ui->feedback));
        }
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        add_action(view, MESHPAY_UI_ACTION_PAY, "Payer");
        break;
    case MESHPAY_UI_SCREEN_NETWORK:
        view_text(view->title, "Reseau");
        (void)snprintf(view->primary, sizeof(view->primary),
                       "%u peers connus", (unsigned)ui->network_peers);
        view_text(view->secondary, ui->network_peers == 0
                                       ? "En attente"
                                       : "Reticulum actif");
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        add_action(view, MESHPAY_UI_ACTION_CURRENCY, "Monnaie");
        break;
    case MESHPAY_UI_SCREEN_DAG_MONITOR: {
        const meshpay_ui_dag_monitor_status_t *m = &ui->dag_monitor;
        char line[MESHPAY_UI_TEXT_MAX];

        switch (ui->dag_monitor_page) {
        case MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW:
            view_text(view->title, "Moniteur DAG");
            (void)snprintf(view->primary,
                           sizeof(view->primary),
                           "Sante %u",
                           (unsigned)m->health_score);
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "%s / %u pairs",
                           m->radio_label[0] != '\0' ? m->radio_label : "LoRa",
                           (unsigned)m->peer_count);
            (void)snprintf(line,
                           sizeof(line),
                           "LoRa frames %lu",
                           (unsigned long)m->lora_frames);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "RNS packets %lu",
                           (unsigned long)m->rns_packets);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "DAG sum %lu req %lu",
                           (unsigned long)m->dag_summaries,
                           (unsigned long)m->dag_requests);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Resources %lu batches %lu",
                           (unsigned long)m->resource_frames,
                           (unsigned long)m->dag_batches);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "TX reseau %lu",
                           (unsigned long)m->tx_advertised);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "TX batch %lu",
                           (unsigned long)m->tx_observed);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Err %lu dup %lu alert %u",
                           (unsigned long)m->malformed_frames,
                           (unsigned long)m->duplicate_packets,
                           (unsigned)m->alert_count);
            add_detail(view, line);
            break;

        case MESHPAY_UI_DAG_MONITOR_PAGE_PEERS:
            view_text(view->title, "Pairs DAG");
            (void)snprintf(view->primary,
                           sizeof(view->primary),
                           "%u pairs",
                           (unsigned)m->peer_count);
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "%lu TX reseau",
                           (unsigned long)m->tx_advertised);
            if (m->peer_line_count == 0U) {
                add_detail(view, "Aucun pair observe");
            } else {
                for (uint8_t i = 0; i < m->peer_line_count; ++i) {
                    add_detail(view, m->peer_lines[i]);
                }
            }
            (void)snprintf(line,
                           sizeof(line),
                           "Announces %lu",
                           (unsigned long)m->announces);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Summaries %lu requests %lu",
                           (unsigned long)m->dag_summaries,
                           (unsigned long)m->dag_requests);
            add_detail(view, line);
            break;

        case MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS:
            view_text(view->title, "Alertes");
            (void)snprintf(view->primary,
                           sizeof(view->primary),
                           "%u alertes",
                           (unsigned)m->alert_count);
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "Sante %u",
                           (unsigned)m->health_score);
            (void)snprintf(line,
                           sizeof(line),
                           "Err LoRa %lu RNS %lu",
                           (unsigned long)m->malformed_lora_frames,
                           (unsigned long)m->malformed_rns_packets);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Err DAG %lu dup %lu",
                           (unsigned long)m->malformed_dag_sync,
                           (unsigned long)m->duplicate_packets);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Regressions TX %lu",
                           (unsigned long)m->peer_regressions);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Summary sans tips %lu",
                           (unsigned long)m->peer_summary_without_tips);
            add_detail(view, line);
            if (m->alert_line_count == 0U) {
                add_detail(view, "Aucune alerte");
            } else {
                for (uint8_t i = 0; i < m->alert_line_count; ++i) {
                    add_detail(view, m->alert_lines[i]);
                }
            }
            break;

        case MESHPAY_UI_DAG_MONITOR_PAGE_RADIO:
            view_text(view->title, "Radio LoRa");
            view_text(view->primary, m->lora_ready ? "LoRa OK" : "LoRa OFF");
            (void)snprintf(view->secondary,
                           sizeof(view->secondary),
                           "%lu frames",
                           (unsigned long)m->lora_frames);
            add_detail(view, m->lora_ready ? "Bearer LoRa actif"
                                           : "Bearer LoRa off");
            if (m->battery_available) {
                (void)snprintf(line,
                               sizeof(line),
                               "BATT %u%% / %u mV",
                               (unsigned)m->battery_percent,
                               (unsigned)m->battery_mv);
                add_detail(view, line);
            } else {
                add_detail(view, "BATT -- / ---- mV");
            }
            (void)snprintf(line,
                           sizeof(line),
                           "Frames LoRa %lu",
                           (unsigned long)m->lora_frames);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Paquets RNS %lu",
                           (unsigned long)m->rns_packets);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Unknown packets %lu",
                           (unsigned long)m->unknown_packets);
            add_detail(view, line);
            (void)snprintf(line,
                           sizeof(line),
                           "Malformed total %lu",
                           (unsigned long)m->malformed_frames);
            add_detail(view, line);
            add_detail(view, "Aucune emission DAG");
            break;

        default:
            return ESP_ERR_INVALID_ARG;
        }
        add_monitor_nav_actions(view);
        break;
    }
    case MESHPAY_UI_SCREEN_LOCKED:
        view_text(view->title, "Verrouille");
        view_text(view->primary, "PIN verrouille");
        view_text(view->secondary, "Reinitialisation requise");
        break;
    case MESHPAY_UI_SCREEN_CURRENCY_MENU:
        view_text(view->title, "Monnaie");
        if (ui->join_state == MESHPAY_UI_JOIN_MEMBER) {
            view_text(view->primary,
                      ui->currency_name[0] != '\0' ? ui->currency_name
                                                   : "Monnaie active");
            (void)snprintf(view->secondary, sizeof(view->secondary),
                           "Solde %lu", (unsigned long)ui->balance);
            add_action(view, MESHPAY_UI_ACTION_SHOW_CODE, "Code");
            add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        } else if (ui->join_state == MESHPAY_UI_JOIN_ARMED) {
            view_text(view->primary, "Rejointe en cours");
            view_text(view->secondary, "Approcher un pair");
            add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        } else {
            view_text(view->primary, "Creer ou rejoindre");
            view_text(view->secondary, "Choisir une action");
            add_action(view, MESHPAY_UI_ACTION_CREATE, "Creer");
            add_action(view, MESHPAY_UI_ACTION_JOIN, "Rejoindre");
            add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        }
        break;
    case MESHPAY_UI_SCREEN_JOIN:
        view_text(view->title, "Rejoindre");
        view_text(view->primary, "Code d'invitation");
        view_text(view->secondary,
                  ui->text_entry_len > 0 ? ui->text_entry : "Saisir le code");
        add_action(view, MESHPAY_UI_ACTION_CONFIRM, "OK");
        add_action(view, MESHPAY_UI_ACTION_BACKSPACE, "Effacer");
        add_action(view, MESHPAY_UI_ACTION_CLEAR, "Vider");
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        break;
    case MESHPAY_UI_SCREEN_FOUNDER_CODE:
        view_text(view->title, "Code monnaie");
        view_text(view->primary,
                  ui->invite_code[0] != '\0' ? ui->invite_code : "Indisponible");
        view_text(view->secondary, "A dicter au nouveau membre");
        add_action(view, MESHPAY_UI_ACTION_HOME, "Accueil");
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (view->footer[0] == '\0') {
        view_text(view->footer, feedback_text(ui->feedback));
    }
    return ESP_OK;
}

esp_err_t meshpay_ui_on_pin_result(meshpay_ui_state_t *ui,
                                   bool success,
                                   bool locked)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (locked) {
        ui->pin_locked = true;
        ui->screen = MESHPAY_UI_SCREEN_LOCKED;
        ui->feedback = MESHPAY_UI_FEEDBACK_PIN_ERROR;
        return ESP_OK;
    }
    if (success) {
        ui->has_pin = true;
        ui->pin_failures = 0;
        ui->pin_locked = false;
        (void)meshpay_ui_clear_entry(ui);
        ui->screen = MESHPAY_UI_SCREEN_HOME;
        ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
        return ESP_OK;
    }

    ui->pin_failures++;
    ui->feedback = MESHPAY_UI_FEEDBACK_PIN_ERROR;
    return ESP_OK;
}

esp_err_t meshpay_ui_on_payment_feedback(meshpay_ui_state_t *ui,
                                         meshpay_payment_feedback_t feedback,
                                         uint32_t amount)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ui->last_amount = amount;
    switch (feedback) {
    case MESHPAY_PAYMENT_FEEDBACK_LOCKED:
        ui->feedback = MESHPAY_UI_FEEDBACK_PAYMENT_LOCKED;
        ui->screen = MESHPAY_UI_SCREEN_PAY;
        break;
    case MESHPAY_PAYMENT_FEEDBACK_SENT:
        ui->feedback = MESHPAY_UI_FEEDBACK_PAYMENT_SENT;
        ui->screen = MESHPAY_UI_SCREEN_PAY;
        break;
    case MESHPAY_PAYMENT_FEEDBACK_RECEIVED:
        ui->feedback = MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED;
        ui->screen = MESHPAY_UI_SCREEN_RECEIVE;
        break;
    case MESHPAY_PAYMENT_FEEDBACK_ACKED:
        ui->feedback = MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED;
        ui->screen = MESHPAY_UI_SCREEN_HISTORY;
        break;
    case MESHPAY_PAYMENT_FEEDBACK_REJECTED:
        ui->feedback = MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED;
        break;
    case MESHPAY_PAYMENT_FEEDBACK_IDLE:
    default:
        ui->feedback = MESHPAY_UI_FEEDBACK_NONE;
        break;
    }
    return ESP_OK;
}
