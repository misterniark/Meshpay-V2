#pragma once

#include "esp_err.h"
#include "meshpay/payment_engine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MESHPAY_UI_SCREEN_SETUP_PIN = 0,
    MESHPAY_UI_SCREEN_HOME,
    MESHPAY_UI_SCREEN_PAYEE,
    MESHPAY_UI_SCREEN_PAY,
    MESHPAY_UI_SCREEN_RECEIVE,
    MESHPAY_UI_SCREEN_HISTORY,
    MESHPAY_UI_SCREEN_NETWORK,
    MESHPAY_UI_SCREEN_DAG_MONITOR,
    MESHPAY_UI_SCREEN_LOCKED,
} meshpay_ui_screen_t;

typedef enum {
    MESHPAY_UI_FEEDBACK_NONE = 0,
    MESHPAY_UI_FEEDBACK_PAYMENT_LOCKED,
    MESHPAY_UI_FEEDBACK_PAYMENT_SENT,
    MESHPAY_UI_FEEDBACK_PAYMENT_RECEIVED,
    MESHPAY_UI_FEEDBACK_PAYMENT_CONFIRMED,
    MESHPAY_UI_FEEDBACK_PAYMENT_REJECTED,
    MESHPAY_UI_FEEDBACK_PIN_ERROR,
} meshpay_ui_feedback_t;

#define MESHPAY_UI_PIN_ENTRY_MAX 16
#define MESHPAY_UI_TEXT_MAX 48
#define MESHPAY_UI_ACTION_MAX 4
#define MESHPAY_UI_ACTION_LABEL_MAX 12
#define MESHPAY_UI_PEER_LABEL_MAX 32
#define MESHPAY_UI_ID_LABEL_MAX 16
#define MESHPAY_UI_DETAIL_LINE_MAX 8
#define MESHPAY_UI_DAG_MONITOR_PEER_LINE_MAX 4
#define MESHPAY_UI_DAG_MONITOR_ALERT_LINE_MAX 4

typedef enum {
    MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW = 0,
    MESHPAY_UI_DAG_MONITOR_PAGE_PEERS,
    MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS,
    MESHPAY_UI_DAG_MONITOR_PAGE_RADIO,
    MESHPAY_UI_DAG_MONITOR_PAGE_MAX,
} meshpay_ui_dag_monitor_page_t;

typedef struct {
    bool lora_ready;
    bool battery_available;
    uint8_t health_score;
    uint8_t battery_percent;
    uint8_t peer_count;
    uint8_t alert_count;
    uint16_t battery_mv;
    uint32_t lora_frames;
    uint32_t rns_packets;
    uint32_t dag_summaries;
    uint32_t dag_requests;
    uint32_t resource_frames;
    uint32_t dag_batches;
    uint32_t tx_advertised;
    uint32_t tx_observed;
    uint32_t announces;
    uint32_t unknown_packets;
    uint32_t malformed_lora_frames;
    uint32_t malformed_rns_packets;
    uint32_t malformed_dag_sync;
    uint32_t malformed_frames;
    uint32_t duplicate_packets;
    uint32_t peer_regressions;
    uint32_t peer_summary_without_tips;
    uint8_t peer_line_count;
    uint8_t alert_line_count;
    char radio_label[MESHPAY_UI_ID_LABEL_MAX];
    char peer_lines[MESHPAY_UI_DAG_MONITOR_PEER_LINE_MAX][MESHPAY_UI_TEXT_MAX];
    char alert_lines[MESHPAY_UI_DAG_MONITOR_ALERT_LINE_MAX][MESHPAY_UI_TEXT_MAX];
} meshpay_ui_dag_monitor_status_t;

typedef enum {
    MESHPAY_UI_ACTION_NONE = 0,
    MESHPAY_UI_ACTION_HOME,
    MESHPAY_UI_ACTION_PAY,
    MESHPAY_UI_ACTION_RECEIVE,
    MESHPAY_UI_ACTION_HISTORY,
    MESHPAY_UI_ACTION_NETWORK,
    MESHPAY_UI_ACTION_CONFIRM,
    MESHPAY_UI_ACTION_BACKSPACE,
    MESHPAY_UI_ACTION_CLEAR,
    MESHPAY_UI_ACTION_NEXT_PEER,
    MESHPAY_UI_ACTION_MONITOR_OVERVIEW,
    MESHPAY_UI_ACTION_MONITOR_PEERS,
    MESHPAY_UI_ACTION_MONITOR_ALERTS,
    MESHPAY_UI_ACTION_MONITOR_RADIO,
} meshpay_ui_action_t;

typedef struct {
    meshpay_ui_screen_t screen;
    meshpay_ui_feedback_t feedback;
    uint32_t balance;
    uint32_t draft_amount;
    uint32_t last_amount;
    uint8_t network_peers;
    uint8_t payment_peer_count;
    uint8_t selected_payment_peer;
    uint8_t pin_failures;
    uint8_t pin_entry_len;
    char pin_entry[MESHPAY_UI_PIN_ENTRY_MAX + 1];
    char local_alias[MESHPAY_UI_PEER_LABEL_MAX];
    char local_id[MESHPAY_UI_ID_LABEL_MAX];
    char payment_peer_label[MESHPAY_UI_PEER_LABEL_MAX];
    char last_peer_label[MESHPAY_UI_PEER_LABEL_MAX];
    meshpay_ui_dag_monitor_status_t dag_monitor;
    meshpay_ui_dag_monitor_page_t dag_monitor_page;
    bool has_pin;
    bool pin_locked;
} meshpay_ui_state_t;

typedef struct {
    meshpay_ui_screen_t screen;
    meshpay_ui_dag_monitor_page_t dag_monitor_page;
    meshpay_ui_feedback_t feedback;
    char title[MESHPAY_UI_TEXT_MAX];
    char primary[MESHPAY_UI_TEXT_MAX];
    char secondary[MESHPAY_UI_TEXT_MAX];
    char footer[MESHPAY_UI_TEXT_MAX];
    char detail_lines[MESHPAY_UI_DETAIL_LINE_MAX][MESHPAY_UI_TEXT_MAX];
    meshpay_ui_action_t actions[MESHPAY_UI_ACTION_MAX];
    char action_labels[MESHPAY_UI_ACTION_MAX][MESHPAY_UI_ACTION_LABEL_MAX];
    uint8_t detail_count;
    uint8_t action_count;
    bool confirm_enabled;
} meshpay_ui_view_t;

void meshpay_ui_init(meshpay_ui_state_t *ui, bool has_pin);
void meshpay_ui_init_monitor(meshpay_ui_state_t *ui);
esp_err_t meshpay_ui_set_balance(meshpay_ui_state_t *ui, uint32_t balance);
esp_err_t meshpay_ui_set_network_peers(meshpay_ui_state_t *ui, uint8_t peers);
esp_err_t meshpay_ui_set_dag_monitor(
    meshpay_ui_state_t *ui,
    const meshpay_ui_dag_monitor_status_t *status);
esp_err_t meshpay_ui_monitor_page(meshpay_ui_state_t *ui,
                                  meshpay_ui_dag_monitor_page_t page);
esp_err_t meshpay_ui_set_local_identity(meshpay_ui_state_t *ui,
                                        const char *alias,
                                        const char *short_id);
esp_err_t meshpay_ui_set_payment_peer(meshpay_ui_state_t *ui,
                                      const char *label,
                                      uint8_t selected_index,
                                      uint8_t peer_count);
esp_err_t meshpay_ui_next_payment_peer(meshpay_ui_state_t *ui);
esp_err_t meshpay_ui_set_history_peer(meshpay_ui_state_t *ui,
                                      const char *label);
esp_err_t meshpay_ui_nav(meshpay_ui_state_t *ui, meshpay_ui_screen_t screen);
esp_err_t meshpay_ui_input_digit(meshpay_ui_state_t *ui, uint8_t digit);
esp_err_t meshpay_ui_backspace(meshpay_ui_state_t *ui);
esp_err_t meshpay_ui_clear_entry(meshpay_ui_state_t *ui);
bool meshpay_ui_confirm_enabled(const meshpay_ui_state_t *ui);
esp_err_t meshpay_ui_pin_entry(const meshpay_ui_state_t *ui,
                               char *out,
                               size_t out_len,
                               size_t *pin_len);
esp_err_t meshpay_ui_build_view(const meshpay_ui_state_t *ui,
                                meshpay_ui_view_t *view);
esp_err_t meshpay_ui_on_pin_result(meshpay_ui_state_t *ui,
                                   bool success,
                                   bool locked);
esp_err_t meshpay_ui_on_payment_feedback(meshpay_ui_state_t *ui,
                                         meshpay_payment_feedback_t feedback,
                                         uint32_t amount);

#ifdef __cplusplus
}
#endif
