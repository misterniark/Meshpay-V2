#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "meshpay/app_main_logic.h"
#include "meshpay/dag_monitor.h"
#include "meshpay/dag_sync.h"
#include "meshpay/dag_store.h"
#include "meshpay/device_hal.h"
#include "meshpay/project_skeleton.h"
#include "meshpay/rns/rns_announce.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_node.h"
#include "meshpay/rns/rns_radio.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
#include "esp_heap_caps.h"
#endif

#if CONFIG_MESHPAY_RADIO_ESPNOW || CONFIG_MESHPAY_RADIO_ESPNOW_LORA_CORE1262
#define MESHPAY_RADIO_HAS_ESPNOW 1
#else
#define MESHPAY_RADIO_HAS_ESPNOW 0
#endif

#if CONFIG_MESHPAY_RADIO_LORA_UART || CONFIG_MESHPAY_RADIO_LORA_CORE1262 || \
    CONFIG_MESHPAY_RADIO_ESPNOW_LORA_CORE1262
#define MESHPAY_RADIO_HAS_LORA 1
#else
#define MESHPAY_RADIO_HAS_LORA 0
#endif

#if CONFIG_MESHPAY_RADIO_LORA_CORE1262 || CONFIG_MESHPAY_RADIO_ESPNOW_LORA_CORE1262
#define MESHPAY_RADIO_HAS_LORA_CORE1262 1
#else
#define MESHPAY_RADIO_HAS_LORA_CORE1262 0
#endif

#if CONFIG_MESHPAY_LORA_C1262_CALIBRATE_IMAGE
#define MESHPAY_LORA_C1262_CALIBRATE_IMAGE_ENABLED true
#else
#define MESHPAY_LORA_C1262_CALIBRATE_IMAGE_ENABLED false
#endif

#if MESHPAY_RADIO_HAS_ESPNOW || MESHPAY_RADIO_HAS_LORA
#define MESHPAY_RADIO_ENABLED 1
#else
#define MESHPAY_RADIO_ENABLED 0
#endif

static const char *TAG = "meshpayv2";
static const char *RADIO_TASK_NAME = "radio_task";
#if CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
static const char *TOUCH_TASK_NAME = "touch_task";
#endif
static const char *s_radio_backend = "disabled";

#define MESHPAY_RADIO_TASK_STACK_BYTES 8192
#define MESHPAY_BOOT_CREDIT_AMOUNT 10U
#define MESHPAY_BOOT_CREDIT_SEQ 0U
#define MESHPAY_BOOT_ANNOUNCE_COUNT 3U
#define MESHPAY_BOOT_ANNOUNCE_INTERVAL_MS 900U
#define MESHPAY_ANNOUNCE_REPLY_CACHE_MAX 16U
#define MESHPAY_ANNOUNCE_REPLY_COOLDOWN_MS 30000U
#define MESHPAY_UI_REFRESH_INTERVAL_MS 1000U
#define MESHPAY_DAG_SUMMARY_INTERVAL_MS 15000U
#define MESHPAY_DAG_MONITOR_RENDER_POLL_MS 1000U
#define MESHPAY_DAG_MONITOR_FULL_REFRESH_MS 300000U
#define MESHPAY_DAG_MONITOR_RADIO_TASK_STACK_BYTES 8192
#define MESHPAY_DAG_MONITOR_UI_TASK_STACK_BYTES 8192

typedef struct {
    uint8_t destination[RNS_DESTINATION_HASH_SIZE];
    uint64_t last_reply_ms;
} meshpay_announce_reply_entry_t;

static rns_node_t s_node;
static meshpay_app_t s_app;
static meshpay_app_runtime_t s_runtime;
static char s_device_alias[MESHPAY_STORAGE_ALIAS_MAX];
static meshpay_announce_reply_entry_t
    s_announce_replied[MESHPAY_ANNOUNCE_REPLY_CACHE_MAX];
static size_t s_announce_replied_count;
static TaskHandle_t s_boot_announce_task;
static TaskHandle_t s_dag_summary_task;
#if CONFIG_MESHPAY_DAG_MONITOR_ONLY
static meshpay_dag_monitor_t s_dag_monitor;
static meshpay_ui_state_t s_dag_monitor_ui;
static SemaphoreHandle_t s_dag_monitor_lock;
#if MESHPAY_RADIO_HAS_LORA
static TaskHandle_t s_dag_monitor_radio_task;
#endif
static TaskHandle_t s_dag_monitor_render_task;
static TaskHandle_t s_dag_monitor_touch_task;
static bool s_dag_monitor_lora_ready;
static void dag_monitor_refresh_ui_locked(void);
#endif

static void meshpay_short_destination(
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE],
    char out[MESHPAY_UI_ID_LABEL_MAX])
{
    if (out == NULL || destination == NULL) {
        return;
    }
    (void)snprintf(out,
                   MESHPAY_UI_ID_LABEL_MAX,
                   "%02x%02x%02x%02x",
                   destination[0],
                   destination[1],
                   destination[2],
                   destination[3]);
}
#if MESHPAY_RADIO_ENABLED
static meshpay_hal_t s_hal;
#endif
#if MESHPAY_RADIO_HAS_ESPNOW
static meshpay_hal_t s_espnow_hal;
#endif
#if MESHPAY_RADIO_HAS_LORA
static meshpay_hal_t s_lora_hal;
#endif
#if CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
static meshpay_hal_t s_display_hal;
static meshpay_hal_waveshare_s3_touch_driver_t s_waveshare_display_driver;
static TaskHandle_t s_waveshare_touch_task;
#endif
#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
static meshpay_hal_t s_display_hal;
static meshpay_hal_lilygo_t5s3_h752_driver_t s_lilygo_h752_display_driver;
#endif
#if MESHPAY_RADIO_HAS_ESPNOW
static meshpay_hal_espnow_driver_t s_espnow_driver;
#endif
#if CONFIG_MESHPAY_RADIO_LORA_UART
static meshpay_hal_lora_uart_driver_t s_lora_uart_driver;
#endif
#if MESHPAY_RADIO_HAS_LORA_CORE1262
static meshpay_hal_lora_core1262_driver_t s_lora_core1262_driver;
#endif
#if MESHPAY_RADIO_ENABLED
typedef struct {
    meshpay_hal_t *espnow_hal;
    meshpay_hal_t *lora_hal;
} meshpay_radio_combo_t;

static meshpay_radio_combo_t s_radio_combo;
static rns_radio_t s_radio;
static rns_radio_node_adapter_t s_radio_adapter;
static TaskHandle_t s_radio_task;
#endif

static bool meshpay_destination_equal(
    const uint8_t a[MESHPAY_TX_DESTINATION_HASH_SIZE],
    const uint8_t b[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    return rns_crypto_constant_equal(a, b, MESHPAY_TX_DESTINATION_HASH_SIZE);
}

static esp_err_t meshpay_send_announce(rns_node_t *node,
                                       const char *reason)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *app_data = (const uint8_t *)s_device_alias;
    esp_err_t err = rns_node_announce(node,
                                      app_data,
                                      strlen(s_device_alias));
    if (err == ESP_OK) {
        const meshpay_app_event_t announce_event = {
            .type = MESHPAY_APP_EVENT_CORE_ANNOUNCE,
            .now_ms = (uint64_t)(esp_timer_get_time() / 1000),
        };
        (void)meshpay_app_runtime_post(&s_runtime,
                                       MESHPAY_APP_QUEUE_CORE,
                                       &announce_event,
                                       0);
        ESP_LOGI(TAG,
                 "announce sent reason=%s alias=%s",
                 reason == NULL ? "unknown" : reason,
                 s_device_alias);
    }
    return err;
}

static bool meshpay_announce_reply_seen(
    const uint8_t destination[RNS_DESTINATION_HASH_SIZE],
    uint64_t now_ms)
{
    for (size_t i = 0; i < s_announce_replied_count; ++i) {
        if (rns_destination_hash_equal(s_announce_replied[i].destination,
                                       destination)) {
            uint64_t last_ms = s_announce_replied[i].last_reply_ms;
            if (now_ms >= last_ms &&
                now_ms - last_ms >= MESHPAY_ANNOUNCE_REPLY_COOLDOWN_MS) {
                return false;
            }
            return true;
        }
    }
    return false;
}

static void meshpay_announce_reply_remember(
    const uint8_t destination[RNS_DESTINATION_HASH_SIZE],
    uint64_t now_ms)
{
    for (size_t i = 0; i < s_announce_replied_count; ++i) {
        if (rns_destination_hash_equal(s_announce_replied[i].destination,
                                       destination)) {
            s_announce_replied[i].last_reply_ms = now_ms;
            return;
        }
    }
    if (s_announce_replied_count < MESHPAY_ANNOUNCE_REPLY_CACHE_MAX) {
        memcpy(s_announce_replied[s_announce_replied_count].destination,
               destination,
               RNS_DESTINATION_HASH_SIZE);
        s_announce_replied[s_announce_replied_count].last_reply_ms = now_ms;
        s_announce_replied_count++;
        return;
    }
    memmove(s_announce_replied,
            s_announce_replied + 1,
            (MESHPAY_ANNOUNCE_REPLY_CACHE_MAX - 1U) *
                sizeof(s_announce_replied[0]));
    memcpy(s_announce_replied[MESHPAY_ANNOUNCE_REPLY_CACHE_MAX - 1U]
               .destination,
           destination,
           RNS_DESTINATION_HASH_SIZE);
    s_announce_replied[MESHPAY_ANNOUNCE_REPLY_CACHE_MAX - 1U].last_reply_ms =
        now_ms;
}

static esp_err_t refresh_app_balance(meshpay_app_t *app, uint64_t now_ms)
{
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t balance = 0;
    ESP_RETURN_ON_ERROR(meshpay_wallet_get_available_balance(&app->wallet,
                                                             &app->currency,
                                                             &app->dag,
                                                             now_ms,
                                                             &balance),
                        TAG,
                        "");
    return meshpay_ui_set_balance(&app->ui, balance);
}

static esp_err_t restore_boot_credit_from_checkpoint(
    meshpay_app_t *app,
    const meshpay_storage_record_t *record)
{
    if (app == NULL || record == NULL || !record->has_checkpoint) {
        return ESP_ERR_NOT_FOUND;
    }

    meshpay_tx_t tx;
    ESP_RETURN_ON_ERROR(meshpay_tx_decode(record->checkpoint,
                                          record->checkpoint_len,
                                          &tx),
                        TAG,
                        "");
    if (tx.type != MESHPAY_TX_TYPE_MINT ||
        tx.amount != MESHPAY_BOOT_CREDIT_AMOUNT ||
        !meshpay_destination_equal(tx.from, app->local_destination) ||
        !meshpay_destination_equal(tx.to, app->local_destination)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (meshpay_currency_validate_tx(&app->currency, &app->dag, &tx) !=
        MESHPAY_CURRENCY_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    meshpay_dag_merge_result_t merge = meshpay_dag_merge_tx(&app->dag, &tx);
    if (merge != MESHPAY_DAG_MERGE_OK &&
        merge != MESHPAY_DAG_MERGE_DUPLICATE) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "boot credit restored amount=%u",
             (unsigned)MESHPAY_BOOT_CREDIT_AMOUNT);
    return refresh_app_balance(app, 0);
}

static esp_err_t create_boot_credit_once(meshpay_app_t *app,
                                         const meshpay_storage_backend_t *backend,
                                         meshpay_storage_record_t *record)
{
    if (app == NULL || backend == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->has_checkpoint) {
        return restore_boot_credit_from_checkpoint(app, record);
    }

    meshpay_tx_t tx;
    ESP_RETURN_ON_ERROR(meshpay_tx_create_mint(&tx,
                                               &app->identity,
                                               app->local_destination,
                                               app->local_destination,
                                               MESHPAY_BOOT_CREDIT_AMOUNT,
                                               MESHPAY_BOOT_CREDIT_SEQ,
                                               app->currency.currency_id,
                                               NULL,
                                               0,
                                               0),
                        TAG,
                        "");
    if (meshpay_currency_validate_tx(&app->currency, &app->dag, &tx) !=
        MESHPAY_CURRENCY_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    meshpay_dag_merge_result_t merge = meshpay_dag_merge_tx(&app->dag, &tx);
    if (merge != MESHPAY_DAG_MERGE_OK &&
        merge != MESHPAY_DAG_MERGE_DUPLICATE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t encoded[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t encoded_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_tx_encode(&tx,
                                          encoded,
                                          sizeof(encoded),
                                          &encoded_len),
                        TAG,
                        "");
    ESP_RETURN_ON_ERROR(meshpay_storage_record_set_checkpoint(record,
                                                              1,
                                                              encoded,
                                                              encoded_len),
                        TAG,
                        "");
    ESP_RETURN_ON_ERROR(meshpay_storage_save(backend, record), TAG, "");
    ESP_LOGI(TAG, "boot credit minted once amount=%u",
             (unsigned)MESHPAY_BOOT_CREDIT_AMOUNT);
    return refresh_app_balance(app, 0);
}

/* Recalcule next_seq depuis la DAG restauree : max(seq des tx emises par soi) + 1,
 * jamais en dessous du next_seq deja charge du NVS. Empeche toute reutilisation
 * de seq si la DAG persistee est plus avancee que le compteur NVS. */
static void restore_next_seq_from_dag(meshpay_app_t *app)
{
    if (app == NULL) {
        return;
    }
    uint32_t max_seq = 0;
    bool found = false;
    size_t n = meshpay_dag_count(&app->dag);
    for (size_t i = 0; i < n; ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(&app->dag, i);
        if (tx == NULL || tx->type != MESHPAY_TX_TYPE_TRANSFER) {
            continue;
        }
        if (!meshpay_destination_equal(tx->from, app->wallet.owner)) {
            continue;
        }
        if (!found || tx->seq >= max_seq) {
            max_seq = tx->seq;
            found = true;
        }
    }
    if (found && max_seq + 1U > app->wallet.next_seq) {
        app->wallet.next_seq = max_seq + 1U;
    }
}

#if CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
#define WS147_UI_BG 0x0841
#define WS147_UI_PANEL 0xFFFF
#define WS147_UI_TEXT 0xFFFF
#define WS147_UI_TEXT_DARK 0x0841
#define WS147_UI_ACCENT 0x07E0
#define WS147_UI_MUTED 0xC638
#define WS147_UI_WARN 0xFFE0
#define WS147_UI_DANGER 0xF800
#define WS147_SOFT_KEY_Y 128
#define WS147_SOFT_KEY_H 30
#define WS147_SOFT_KEY_W 72
#define WS147_SOFT_KEY_GAP 6
#define WS147_KEYPAD_X 194
#define WS147_KEYPAD_Y 50
#define WS147_KEY_W 36
#define WS147_KEY_H 24
#define WS147_KEY_GAP 6
#define WS147_TOUCH_POLL_MS 35
#define WS147_TOUCH_RENDER_MS 150

static bool waveshare_input_screen(meshpay_ui_screen_t screen)
{
    return screen == MESHPAY_UI_SCREEN_SETUP_PIN ||
           screen == MESHPAY_UI_SCREEN_PAY;
}

static void fb_rect(uint16_t *fb,
                    uint16_t x,
                    uint16_t y,
                    uint16_t w,
                    uint16_t h,
                    uint16_t color)
{
    if (fb == NULL || x >= MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH ||
        y >= MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT) {
        return;
    }
    if ((uint32_t)x + w > MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH) {
        w = MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH - x;
    }
    if ((uint32_t)y + h > MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT) {
        h = MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT - y;
    }
    for (uint16_t row = 0; row < h; ++row) {
        uint16_t *line =
            fb + ((size_t)y + row) * MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH + x;
        for (uint16_t col = 0; col < w; ++col) {
            line[col] = color;
        }
    }
}

static const uint8_t *font5x7(char ch)
{
    static const uint8_t space[5] = {0, 0, 0, 0, 0};
    static const uint8_t glyphs[][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
        {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
        {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
        {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
        {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
        {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
        {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
        {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
        {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
        {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
        {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
        {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
        {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
        {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
        {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
        {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
        {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
        {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
        {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
        {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
        {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
        {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
        {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
        {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
        {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
        {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
        {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    };
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
    static const uint8_t star[5] = {0x14, 0x08, 0x3E, 0x08, 0x14};

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch >= '0' && ch <= '9') {
        return glyphs[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return glyphs[10 + ch - 'A'];
    }
    if (ch == '-') {
        return dash;
    }
    if (ch == '.') {
        return dot;
    }
    if (ch == ':') {
        return colon;
    }
    if (ch == '%') {
        return percent;
    }
    if (ch == '*') {
        return star;
    }
    return space;
}

static void fb_text(uint16_t *fb,
                    uint16_t x,
                    uint16_t y,
                    const char *text,
                    uint16_t color,
                    uint8_t scale)
{
    if (fb == NULL || text == NULL || scale == 0) {
        return;
    }
    uint16_t cursor = x;
    for (const char *p = text; *p != '\0' && cursor < 316; ++p) {
        const uint8_t *glyph = font5x7(*p);
        for (uint8_t col = 0; col < 5; ++col) {
            for (uint8_t row = 0; row < 7; ++row) {
                if ((glyph[col] & (1U << row)) != 0) {
                    fb_rect(fb,
                            (uint16_t)(cursor + col * scale),
                            (uint16_t)(y + row * scale),
                            scale,
                            scale,
                            color);
                }
            }
        }
        cursor = (uint16_t)(cursor + 6U * scale);
    }
}

static uint16_t fb_text_width(const char *text, uint8_t scale)
{
    if (text == NULL || scale == 0) {
        return 0;
    }
    size_t len = strlen(text);
    size_t width = len * 6U * scale;
    if (width > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)width;
}

static void fb_text_center(uint16_t *fb,
                           uint16_t x,
                           uint16_t y,
                           uint16_t w,
                           uint16_t h,
                           const char *text,
                           uint16_t color,
                           uint8_t scale)
{
    uint16_t text_w = fb_text_width(text, scale);
    uint16_t text_h = (uint16_t)(7U * scale);
    uint16_t text_x = x;
    uint16_t text_y = y;
    if (w > text_w) {
        text_x = (uint16_t)(x + (w - text_w) / 2U);
    }
    if (h > text_h) {
        text_y = (uint16_t)(y + (h - text_h) / 2U);
    }
    fb_text(fb, text_x, text_y, text, color, scale);
}

static void fb_button(uint16_t *fb,
                      uint16_t x,
                      uint16_t y,
                      uint16_t w,
                      uint16_t h,
                      const char *label,
                      uint16_t fill,
                      uint16_t text_color)
{
    fb_rect(fb, x, y, w, h, fill);
    fb_rect(fb, x, y, w, 2, WS147_UI_ACCENT);
    fb_text_center(fb, x, y, w, h, label, text_color, 1);
}

static void render_numeric_keypad(uint16_t *fb, const meshpay_ui_view_t *view)
{
    static const char *labels[4][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {"DEL", "0", "OK"},
    };

    for (uint8_t row = 0; row < 4; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
            uint16_t fill = WS147_UI_PANEL;
            if (row == 3 && col == 2 && !view->confirm_enabled) {
                fill = WS147_UI_MUTED;
            }
            const uint16_t x =
                (uint16_t)(WS147_KEYPAD_X + col * (WS147_KEY_W + WS147_KEY_GAP));
            const uint16_t y =
                (uint16_t)(WS147_KEYPAD_Y + row * (WS147_KEY_H + WS147_KEY_GAP));
            fb_button(fb,
                      x,
                      y,
                      WS147_KEY_W,
                      WS147_KEY_H,
                      labels[row][col],
                      fill,
                      WS147_UI_TEXT_DARK);
        }
    }
}

static void render_soft_actions(uint16_t *fb, const meshpay_ui_view_t *view)
{
    if (waveshare_input_screen(view->screen)) {
        if (view->screen == MESHPAY_UI_SCREEN_PAY) {
            fb_button(fb,
                      14,
                      WS147_SOFT_KEY_Y,
                      WS147_SOFT_KEY_W,
                      WS147_SOFT_KEY_H,
                      "Accueil",
                      WS147_UI_PANEL,
                      WS147_UI_TEXT_DARK);
            fb_button(fb,
                      94,
                      WS147_SOFT_KEY_Y,
                      WS147_SOFT_KEY_W,
                      WS147_SOFT_KEY_H,
                      "Cible",
                      WS147_UI_PANEL,
                      WS147_UI_TEXT_DARK);
        }
        return;
    }

    for (uint8_t i = 0; i < view->action_count && i < 4; ++i) {
        const uint16_t x =
            (uint16_t)(8U + i * (WS147_SOFT_KEY_W + WS147_SOFT_KEY_GAP));
        uint16_t fill = WS147_UI_PANEL;
        if (view->actions[i] == MESHPAY_UI_ACTION_CONFIRM &&
            !view->confirm_enabled) {
            fill = WS147_UI_MUTED;
        }
        fb_button(fb,
                  x,
                  WS147_SOFT_KEY_Y,
                  WS147_SOFT_KEY_W,
                  WS147_SOFT_KEY_H,
                  view->action_labels[i],
                  fill,
                  WS147_UI_TEXT_DARK);
    }
}

static void render_waveshare_view(const meshpay_ui_view_t *view)
{
    if (view == NULL) {
        return;
    }

    const size_t pixels = (size_t)MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH *
                          MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT;
    uint16_t *fb = (uint16_t *)malloc(pixels * sizeof(uint16_t));
    if (fb == NULL) {
        ESP_LOGW(TAG, "display framebuffer allocation failed");
        return;
    }

    for (size_t i = 0; i < pixels; ++i) {
        fb[i] = WS147_UI_BG;
    }

    fb_rect(fb, 0, 0, 320, 4, WS147_UI_ACCENT);
    fb_text(fb, 14, 16, view->title, WS147_UI_TEXT, 3);
    fb_text(fb, 14, 58, view->primary, WS147_UI_ACCENT, 2);
    fb_text(fb,
            14,
            waveshare_input_screen(view->screen) ? 86 : 84,
            view->secondary,
            WS147_UI_MUTED,
            waveshare_input_screen(view->screen) ? 1 : 2);
    fb_text(fb,
            184,
            18,
            s_device_alias[0] != '\0' ? s_device_alias : "MESH PAY",
            WS147_UI_MUTED,
            1);
    fb_text(fb, 204, 34, "RNS OK", WS147_UI_ACCENT, 1);

    if (waveshare_input_screen(view->screen)) {
        render_numeric_keypad(fb, view);
    }
    render_soft_actions(fb, view);
    if (view->footer[0] != '\0') {
        fb_text(fb, 14, 110, view->footer, WS147_UI_WARN, 1);
    }

    esp_err_t err = meshpay_hal_display_flush(
        &s_display_hal,
        fb,
        MESHPAY_HAL_WAVESHARE_S3_TOUCH_WIDTH,
        MESHPAY_HAL_WAVESHARE_S3_TOUCH_HEIGHT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display UI flush failed: %s", esp_err_to_name(err));
    }
    free(fb);
}

static bool point_in_rect(int16_t x,
                          int16_t y,
                          uint16_t rect_x,
                          uint16_t rect_y,
                          uint16_t rect_w,
                          uint16_t rect_h)
{
    return x >= (int16_t)rect_x && y >= (int16_t)rect_y &&
           x < (int16_t)(rect_x + rect_w) &&
           y < (int16_t)(rect_y + rect_h);
}

typedef struct {
    bool handled;
    bool has_digit;
    uint8_t digit;
    meshpay_ui_action_t action;
} waveshare_touch_intent_t;

static waveshare_touch_intent_t waveshare_map_keypad(
    const meshpay_ui_view_t *view,
    const meshpay_touch_state_t *touch)
{
    waveshare_touch_intent_t intent = {0};
    if (view == NULL || touch == NULL ||
        !waveshare_input_screen(view->screen)) {
        return intent;
    }

    for (uint8_t row = 0; row < 4; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
            const uint16_t x =
                (uint16_t)(WS147_KEYPAD_X +
                           col * (WS147_KEY_W + WS147_KEY_GAP));
            const uint16_t y =
                (uint16_t)(WS147_KEYPAD_Y +
                           row * (WS147_KEY_H + WS147_KEY_GAP));
            if (!point_in_rect(touch->x,
                               touch->y,
                               x,
                               y,
                               WS147_KEY_W,
                               WS147_KEY_H)) {
                continue;
            }

            intent.handled = true;
            if (row < 3) {
                intent.has_digit = true;
                intent.digit = (uint8_t)(1U + row * 3U + col);
            } else if (col == 0) {
                intent.action = MESHPAY_UI_ACTION_BACKSPACE;
            } else if (col == 1) {
                intent.has_digit = true;
                intent.digit = 0;
            } else if (view->confirm_enabled) {
                intent.action = MESHPAY_UI_ACTION_CONFIRM;
            }
            return intent;
        }
    }
    return intent;
}

static waveshare_touch_intent_t waveshare_map_soft_action(
    const meshpay_ui_view_t *view,
    const meshpay_touch_state_t *touch)
{
    waveshare_touch_intent_t intent = {0};
    if (view == NULL || touch == NULL) {
        return intent;
    }

    if (view->screen == MESHPAY_UI_SCREEN_PAY) {
        if (point_in_rect(touch->x,
                          touch->y,
                          14,
                          WS147_SOFT_KEY_Y,
                          WS147_SOFT_KEY_W,
                          WS147_SOFT_KEY_H)) {
            intent.handled = true;
            intent.action = MESHPAY_UI_ACTION_HOME;
            return intent;
        }
        if (point_in_rect(touch->x,
                          touch->y,
                          94,
                          WS147_SOFT_KEY_Y,
                          WS147_SOFT_KEY_W,
                          WS147_SOFT_KEY_H)) {
            intent.handled = true;
            intent.action = MESHPAY_UI_ACTION_PAY;
            return intent;
        }
    }

    if (waveshare_input_screen(view->screen)) {
        return intent;
    }

    for (uint8_t i = 0; i < view->action_count && i < 4; ++i) {
        const uint16_t x =
            (uint16_t)(8U + i * (WS147_SOFT_KEY_W + WS147_SOFT_KEY_GAP));
        if (point_in_rect(touch->x,
                          touch->y,
                          x,
                          WS147_SOFT_KEY_Y,
                          WS147_SOFT_KEY_W,
                          WS147_SOFT_KEY_H)) {
            intent.handled = true;
            intent.action = view->actions[i];
            return intent;
        }
    }
    return intent;
}

static waveshare_touch_intent_t waveshare_map_touch(
    const meshpay_ui_view_t *view,
    const meshpay_touch_state_t *touch)
{
    waveshare_touch_intent_t intent = waveshare_map_keypad(view, touch);
    if (intent.handled) {
        return intent;
    }
    return waveshare_map_soft_action(view, touch);
}

static esp_err_t waveshare_persist_pin_locked(void)
{
    if (!s_runtime.has_storage) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(meshpay_storage_record_set_pin_hash(
                            &s_runtime.storage_record,
                            s_app.wallet.pin_hash),
                        TAG,
                        "");
    return meshpay_storage_save(&s_runtime.storage_backend,
                                &s_runtime.storage_record);
}

static esp_err_t waveshare_confirm_pin_locked(void)
{
    char pin[MESHPAY_UI_PIN_ENTRY_MAX + 1] = {0};
    size_t pin_len = 0;
    esp_err_t err = meshpay_ui_pin_entry(&s_app.ui,
                                         pin,
                                         sizeof(pin),
                                         &pin_len);
    if (err == ESP_OK) {
        err = meshpay_wallet_set_pin(&s_app.wallet, pin, pin_len);
    }
    if (err == ESP_OK) {
        err = waveshare_persist_pin_locked();
    }
    rns_crypto_secure_zero(pin, sizeof(pin));
    (void)meshpay_ui_on_pin_result(&s_app.ui, err == ESP_OK, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PIN setup failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool waveshare_known_is_local(
    const rns_announce_known_destination_t *known)
{
    return known == NULL ||
           rns_destination_hash_equal(known->destination_hash,
                                      s_app.local_destination);
}

static void waveshare_peer_label_from_known(
    const rns_announce_known_destination_t *known,
    char out[MESHPAY_UI_PEER_LABEL_MAX])
{
    if (out == NULL) {
        return;
    }
    out[0] = '\0';
    if (known != NULL && known->app_data_len > 0) {
        size_t len = known->app_data_len;
        if (len >= MESHPAY_UI_PEER_LABEL_MAX) {
            len = MESHPAY_UI_PEER_LABEL_MAX - 1U;
        }
        for (size_t i = 0; i < len; ++i) {
            uint8_t ch = known->app_data[i];
            out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        }
        out[len] = '\0';
    }
    if (out[0] == '\0') {
        if (known == NULL) {
            (void)snprintf(out, MESHPAY_UI_PEER_LABEL_MAX, "pair inconnu");
        } else {
            (void)snprintf(out,
                           MESHPAY_UI_PEER_LABEL_MAX,
                           "pair %02x%02x",
                           known->destination_hash[0],
                           known->destination_hash[1]);
        }
    }
}

static const rns_announce_known_destination_t *waveshare_peer_at_index(
    uint8_t selected_index,
    uint8_t *peer_count)
{
    uint8_t count = 0;
    const rns_announce_known_destination_t *selected = NULL;
    size_t known_count = rns_announce_known_count();
    for (size_t i = 0; i < known_count; ++i) {
        const rns_announce_known_destination_t *known =
            rns_announce_known_get(i);
        if (waveshare_known_is_local(known)) {
            continue;
        }
        if (count == selected_index) {
            selected = known;
        }
        count++;
    }
    if (peer_count != NULL) {
        *peer_count = count;
    }
    return selected;
}

static esp_err_t waveshare_refresh_payment_peer_locked(void)
{
    uint8_t peer_count = 0;
    uint8_t selected_index = s_app.ui.selected_payment_peer;
    const rns_announce_known_destination_t *known =
        waveshare_peer_at_index(selected_index, &peer_count);
    if (peer_count == 0) {
        return meshpay_ui_set_payment_peer(&s_app.ui, "", 0, 0);
    }
    if (known == NULL) {
        selected_index = 0;
        known = waveshare_peer_at_index(selected_index, NULL);
    }

    char label[MESHPAY_UI_PEER_LABEL_MAX];
    waveshare_peer_label_from_known(known, label);
    return meshpay_ui_set_payment_peer(&s_app.ui,
                                       label,
                                       selected_index,
                                       peer_count);
}

static const rns_announce_known_destination_t *waveshare_selected_peer_locked(void)
{
    if (waveshare_refresh_payment_peer_locked() != ESP_OK ||
        s_app.ui.payment_peer_count == 0) {
        return NULL;
    }
    return waveshare_peer_at_index(s_app.ui.selected_payment_peer, NULL);
}

static esp_err_t waveshare_confirm_payment_locked(void)
{
    uint32_t amount = s_app.ui.draft_amount;
    if (amount == 0) {
        return ESP_OK;
    }

    const rns_announce_known_destination_t *peer =
        waveshare_selected_peer_locked();
    if (peer == NULL) {
        (void)meshpay_ui_on_payment_feedback(&s_app.ui,
                                             MESHPAY_PAYMENT_FEEDBACK_REJECTED,
                                             amount);
        return ESP_OK;
    }

    meshpay_app_event_t event = {
        .type = MESHPAY_APP_EVENT_CORE_PAYMENT,
        .now_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .amount = amount,
    };
    memcpy(event.destination,
           peer->destination_hash,
           sizeof(event.destination));
    esp_err_t err = meshpay_app_runtime_post(&s_runtime,
                                             MESHPAY_APP_QUEUE_CORE,
                                             &event,
                                             0);
    if (err == ESP_OK) {
        (void)meshpay_ui_set_history_peer(&s_app.ui,
                                          s_app.ui.payment_peer_label);
        (void)meshpay_ui_clear_entry(&s_app.ui);
    } else {
        (void)meshpay_ui_on_payment_feedback(&s_app.ui,
                                             MESHPAY_PAYMENT_FEEDBACK_REJECTED,
                                             amount);
    }
    return err;
}

static esp_err_t waveshare_apply_action_locked(meshpay_ui_action_t action)
{
    switch (action) {
    case MESHPAY_UI_ACTION_HOME:
        (void)meshpay_ui_clear_entry(&s_app.ui);
        return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_HOME);
    case MESHPAY_UI_ACTION_PAY:
        if (s_app.ui.screen != MESHPAY_UI_SCREEN_PAY) {
            (void)meshpay_ui_clear_entry(&s_app.ui);
        }
        (void)waveshare_refresh_payment_peer_locked();
        return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_PAYEE);
    case MESHPAY_UI_ACTION_RECEIVE:
        return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_RECEIVE);
    case MESHPAY_UI_ACTION_HISTORY:
        return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_HISTORY);
    case MESHPAY_UI_ACTION_NETWORK:
        return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_NETWORK);
    case MESHPAY_UI_ACTION_CONFIRM:
        if (s_app.ui.screen == MESHPAY_UI_SCREEN_SETUP_PIN) {
            return waveshare_confirm_pin_locked();
        }
        if (s_app.ui.screen == MESHPAY_UI_SCREEN_PAYEE) {
            if (s_app.ui.payment_peer_count == 0) {
                return ESP_OK;
            }
            return meshpay_ui_nav(&s_app.ui, MESHPAY_UI_SCREEN_PAY);
        }
        if (s_app.ui.screen == MESHPAY_UI_SCREEN_PAY) {
            return waveshare_confirm_payment_locked();
        }
        return ESP_OK;
    case MESHPAY_UI_ACTION_BACKSPACE:
        return meshpay_ui_backspace(&s_app.ui);
    case MESHPAY_UI_ACTION_CLEAR:
        return meshpay_ui_clear_entry(&s_app.ui);
    case MESHPAY_UI_ACTION_NEXT_PEER: {
        esp_err_t err = meshpay_ui_next_payment_peer(&s_app.ui);
        if (err == ESP_ERR_NOT_FOUND) {
            return ESP_OK;
        }
        if (err != ESP_OK) {
            return err;
        }
        return waveshare_refresh_payment_peer_locked();
    }
    case MESHPAY_UI_ACTION_NONE:
    default:
        return ESP_OK;
    }
}

static bool waveshare_handle_tap(const meshpay_touch_state_t *touch)
{
    if (touch == NULL || s_runtime.lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_runtime.lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    meshpay_ui_view_t view;
    (void)waveshare_refresh_payment_peer_locked();
    esp_err_t err = meshpay_ui_build_view(&s_app.ui, &view);
    waveshare_touch_intent_t intent = {0};
    if (err == ESP_OK) {
        intent = waveshare_map_touch(&view, touch);
        if (intent.has_digit) {
            err = meshpay_ui_input_digit(&s_app.ui, intent.digit);
        } else if (intent.action != MESHPAY_UI_ACTION_NONE) {
            err = waveshare_apply_action_locked(intent.action);
        }
    }
    xSemaphoreGive(s_runtime.lock);

    if (intent.handled) {
        ESP_LOGI(TAG,
                 "touch x=%d y=%d digit=%d action=%u err=%s",
                 (int)touch->x,
                 (int)touch->y,
                 intent.has_digit ? (int)intent.digit : -1,
                 (unsigned)intent.action,
                 esp_err_to_name(err));
    }
    return intent.handled;
}

static void waveshare_render_current(bool force)
{
    static bool last_valid = false;
    static meshpay_ui_view_t last_view;

    if (s_runtime.lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_runtime.lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    meshpay_ui_view_t view;
    (void)waveshare_refresh_payment_peer_locked();
    esp_err_t err = meshpay_ui_build_view(&s_app.ui, &view);
    xSemaphoreGive(s_runtime.lock);
    if (err != ESP_OK) {
        return;
    }
    if (!force && last_valid &&
        memcmp(&view, &last_view, sizeof(view)) == 0) {
        return;
    }

    render_waveshare_view(&view);
    memcpy(&last_view, &view, sizeof(last_view));
    last_valid = true;
}

static void waveshare_touch_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    int64_t last_render_us = 0;
    while (true) {
        meshpay_touch_state_t touch = {0};
        esp_err_t err = meshpay_hal_touch_read(&s_display_hal, &touch);
        if (err == ESP_OK && touch.pressed && !was_pressed) {
            if (waveshare_handle_tap(&touch)) {
                waveshare_render_current(true);
                last_render_us = esp_timer_get_time();
            }
        }
        if (err == ESP_OK) {
            was_pressed = touch.pressed;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_render_us >
            (int64_t)WS147_TOUCH_RENDER_MS * 1000) {
            waveshare_render_current(false);
            last_render_us = now_us;
        }
        vTaskDelay(pdMS_TO_TICKS(WS147_TOUCH_POLL_MS));
    }
}
#endif

#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
#define H752_UI_BLACK 0x0000
#define H752_UI_WHITE 0xFFFF
#define H752_UI_MARGIN 36
#define H752_UI_NAV_X H752_UI_MARGIN
#define H752_UI_NAV_Y 464
#define H752_UI_NAV_W 884
#define H752_UI_NAV_H 48
#define H752_UI_NAV_ITEMS 4
#define H752_UI_TOUCH_POLL_MS 80

static void h752_fb_rect(uint16_t *fb,
                         uint16_t x,
                         uint16_t y,
                         uint16_t w,
                         uint16_t h,
                         uint16_t color)
{
    if (fb == NULL || x >= MESHPAY_HAL_LILYGO_H752_WIDTH ||
        y >= MESHPAY_HAL_LILYGO_H752_HEIGHT) {
        return;
    }
    if ((uint32_t)x + w > MESHPAY_HAL_LILYGO_H752_WIDTH) {
        w = MESHPAY_HAL_LILYGO_H752_WIDTH - x;
    }
    if ((uint32_t)y + h > MESHPAY_HAL_LILYGO_H752_HEIGHT) {
        h = MESHPAY_HAL_LILYGO_H752_HEIGHT - y;
    }
    for (uint16_t row = 0; row < h; ++row) {
        uint16_t *line =
            fb + ((size_t)y + row) * MESHPAY_HAL_LILYGO_H752_WIDTH + x;
        for (uint16_t col = 0; col < w; ++col) {
            line[col] = color;
        }
    }
}

static void h752_fb_frame(uint16_t *fb,
                          uint16_t x,
                          uint16_t y,
                          uint16_t w,
                          uint16_t h)
{
    h752_fb_rect(fb, x, y, w, 3, H752_UI_BLACK);
    h752_fb_rect(fb, x, (uint16_t)(y + h - 3U), w, 3, H752_UI_BLACK);
    h752_fb_rect(fb, x, y, 3, h, H752_UI_BLACK);
    h752_fb_rect(fb, (uint16_t)(x + w - 3U), y, 3, h, H752_UI_BLACK);
}

static const uint8_t *h752_font5x7(char ch)
{
    static const uint8_t space[5] = {0, 0, 0, 0, 0};
    static const uint8_t glyphs[][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
        {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
        {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
        {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
        {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
        {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
        {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
        {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
        {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
        {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
        {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
        {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
        {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
        {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
        {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
        {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
        {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
        {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
        {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
        {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
        {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
        {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
        {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
        {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
        {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
        {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
        {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    };
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t slash[5] = {0x40, 0x30, 0x0C, 0x03, 0x00};
    static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch >= '0' && ch <= '9') {
        return glyphs[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return glyphs[10 + ch - 'A'];
    }
    if (ch == '-') {
        return dash;
    }
    if (ch == '.') {
        return dot;
    }
    if (ch == ':') {
        return colon;
    }
    if (ch == '/') {
        return slash;
    }
    if (ch == '%') {
        return percent;
    }
    return space;
}

static void h752_fb_text(uint16_t *fb,
                         uint16_t x,
                         uint16_t y,
                         const char *text,
                         uint16_t color,
                         uint8_t scale)
{
    if (fb == NULL || text == NULL || scale == 0) {
        return;
    }
    uint16_t cursor = x;
    const uint16_t char_w = (uint16_t)(6U * scale);
    for (const char *p = text; *p != '\0'; ++p) {
        if ((uint32_t)cursor + char_w >= MESHPAY_HAL_LILYGO_H752_WIDTH) {
            break;
        }
        const uint8_t *glyph = h752_font5x7(*p);
        for (uint8_t col = 0; col < 5; ++col) {
            for (uint8_t row = 0; row < 7; ++row) {
                if ((glyph[col] & (1U << row)) != 0) {
                    h752_fb_rect(fb,
                                 (uint16_t)(cursor + col * scale),
                                 (uint16_t)(y + row * scale),
                                 scale,
                                 scale,
                                 color);
                }
            }
        }
        cursor = (uint16_t)(cursor + char_w);
    }
}

static uint16_t h752_text_width(const char *text, uint8_t scale)
{
    if (text == NULL || scale == 0) {
        return 0;
    }
    size_t len = strlen(text);
    size_t width = len * 6U * scale;
    if (width > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)width;
}

static meshpay_ui_action_t h752_monitor_action_for_page(
    meshpay_ui_dag_monitor_page_t page)
{
    switch (page) {
    case MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW:
        return MESHPAY_UI_ACTION_MONITOR_OVERVIEW;
    case MESHPAY_UI_DAG_MONITOR_PAGE_PEERS:
        return MESHPAY_UI_ACTION_MONITOR_PEERS;
    case MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS:
        return MESHPAY_UI_ACTION_MONITOR_ALERTS;
    case MESHPAY_UI_DAG_MONITOR_PAGE_RADIO:
        return MESHPAY_UI_ACTION_MONITOR_RADIO;
    default:
        return MESHPAY_UI_ACTION_NONE;
    }
}

static meshpay_ui_dag_monitor_page_t h752_monitor_page_for_nav_index(
    uint8_t index)
{
    switch (index) {
    case 0:
        return MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW;
    case 1:
        return MESHPAY_UI_DAG_MONITOR_PAGE_PEERS;
    case 2:
        return MESHPAY_UI_DAG_MONITOR_PAGE_ALERTS;
    case 3:
    default:
        return MESHPAY_UI_DAG_MONITOR_PAGE_RADIO;
    }
}

static void h752_render_nav(uint16_t *fb, const meshpay_ui_view_t *view)
{
    if (fb == NULL || view == NULL || view->action_count == 0) {
        return;
    }

    const uint16_t item_w = H752_UI_NAV_W / H752_UI_NAV_ITEMS;
    const meshpay_ui_action_t active =
        h752_monitor_action_for_page(view->dag_monitor_page);
    for (uint8_t i = 0; i < view->action_count && i < H752_UI_NAV_ITEMS; ++i) {
        const uint16_t x = (uint16_t)(H752_UI_NAV_X + item_w * i);
        const uint16_t w = (i == H752_UI_NAV_ITEMS - 1U)
                               ? (uint16_t)(H752_UI_NAV_W - item_w * i)
                               : item_w;
        const bool selected = view->actions[i] == active;
        if (selected) {
            h752_fb_rect(fb, x, H752_UI_NAV_Y, w, H752_UI_NAV_H, H752_UI_BLACK);
        } else {
            h752_fb_frame(fb, x, H752_UI_NAV_Y, w, H752_UI_NAV_H);
        }

        const char *label = view->action_labels[i];
        const uint16_t text_w = h752_text_width(label, 3);
        uint16_t text_x = (uint16_t)(x + 14U);
        if (text_w < w) {
            text_x = (uint16_t)(x + (w - text_w) / 2U);
        }
        h752_fb_text(fb,
                     text_x,
                     (uint16_t)(H752_UI_NAV_Y + 14U),
                     label,
                     selected ? H752_UI_WHITE : H752_UI_BLACK,
                     3);
    }
}

static void render_h752_view(const meshpay_ui_view_t *view)
{
    if (view == NULL) {
        return;
    }

    const size_t pixels = (size_t)MESHPAY_HAL_LILYGO_H752_WIDTH *
                          MESHPAY_HAL_LILYGO_H752_HEIGHT;
    uint16_t *fb = (uint16_t *)heap_caps_malloc(
        pixels * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        fb = (uint16_t *)malloc(pixels * sizeof(uint16_t));
    }
    if (fb == NULL) {
        ESP_LOGW(TAG, "H752 monitor framebuffer allocation failed");
        return;
    }

    for (size_t i = 0; i < pixels; ++i) {
        fb[i] = H752_UI_WHITE;
    }

    h752_fb_rect(fb, 0, 0, MESHPAY_HAL_LILYGO_H752_WIDTH, 64, H752_UI_BLACK);
    h752_fb_text(fb, 32, 15, view->title, H752_UI_WHITE, 5);
    h752_fb_text(fb, 650, 24, view->secondary, H752_UI_WHITE, 2);

    h752_fb_frame(fb, H752_UI_MARGIN, 96, 292, 176);
    h752_fb_text(fb, 46, 132, view->primary, H752_UI_BLACK, 4);
    if (view->screen == MESHPAY_UI_SCREEN_DAG_MONITOR &&
        view->dag_monitor_page == MESHPAY_UI_DAG_MONITOR_PAGE_OVERVIEW) {
        uint16_t health_width = 0;
        unsigned score = 0;
        if (sscanf(view->primary, "Sante %u", &score) == 1 && score > 100U) {
            score = 100U;
        }
        health_width = (uint16_t)((240U * score) / 100U);
        h752_fb_frame(fb, 60, 214, 244, 34);
        h752_fb_rect(fb, 62, 216, health_width, 30, H752_UI_BLACK);
    } else {
        h752_fb_text(fb, 58, 218, view->secondary, H752_UI_BLACK, 2);
    }

    h752_fb_frame(fb, 360, 96, 560, 344);
    uint16_t y = 128;
    for (uint8_t i = 0; i < view->detail_count; ++i) {
        h752_fb_text(fb,
                     392,
                     y,
                     view->detail_lines[i],
                     H752_UI_BLACK,
                     3);
        y = (uint16_t)(y + 48U);
    }

    h752_render_nav(fb, view);

    esp_err_t err = meshpay_hal_display_flush(&s_display_hal,
                                              fb,
                                              MESHPAY_HAL_LILYGO_H752_WIDTH,
                                              MESHPAY_HAL_LILYGO_H752_HEIGHT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "H752 monitor flush failed: %s", esp_err_to_name(err));
    }
    free(fb);
}

#if CONFIG_MESHPAY_DAG_MONITOR_ONLY
static void h752_render_dag_monitor_current(bool force)
{
    static bool last_valid = false;
    static meshpay_ui_view_t last_view;
    static int64_t last_full_refresh_us = 0;

    if (!s_lilygo_h752_display_driver.initialized ||
        s_dag_monitor_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_dag_monitor_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    meshpay_ui_view_t view;
    esp_err_t err = meshpay_ui_build_view(&s_dag_monitor_ui, &view);
    xSemaphoreGive(s_dag_monitor_lock);
    if (err != ESP_OK) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool periodic_full_refresh =
        last_valid &&
        now_us - last_full_refresh_us >=
            (int64_t)MESHPAY_DAG_MONITOR_FULL_REFRESH_MS * 1000;
    const bool unchanged =
        last_valid && memcmp(&view, &last_view, sizeof(view)) == 0;
    if (!force && unchanged && !periodic_full_refresh) {
        return;
    }

    render_h752_view(&view);
    memcpy(&last_view, &view, sizeof(last_view));
    last_valid = true;
    last_full_refresh_us = esp_timer_get_time();
}

static bool h752_dag_monitor_handle_tap(const meshpay_touch_state_t *touch)
{
    if (touch == NULL || s_dag_monitor_lock == NULL || !touch->pressed) {
        return false;
    }
    if (touch->x < H752_UI_NAV_X ||
        touch->x >= (int16_t)(H752_UI_NAV_X + H752_UI_NAV_W) ||
        touch->y < H752_UI_NAV_Y ||
        touch->y >= (int16_t)(H752_UI_NAV_Y + H752_UI_NAV_H)) {
        return false;
    }

    const uint16_t item_w = H752_UI_NAV_W / H752_UI_NAV_ITEMS;
    uint8_t index = (uint8_t)((touch->x - H752_UI_NAV_X) / item_w);
    if (index >= H752_UI_NAV_ITEMS) {
        index = H752_UI_NAV_ITEMS - 1U;
    }

    bool changed = false;
    if (xSemaphoreTake(s_dag_monitor_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        meshpay_ui_dag_monitor_page_t page =
            h752_monitor_page_for_nav_index(index);
        changed = meshpay_ui_monitor_page(&s_dag_monitor_ui, page) == ESP_OK;
        if (changed) {
            dag_monitor_refresh_ui_locked();
        }
        xSemaphoreGive(s_dag_monitor_lock);
    }
    return changed;
}

static void h752_dag_monitor_touch_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    while (true) {
        meshpay_touch_state_t touch = {0};
        esp_err_t err = meshpay_hal_touch_read(&s_display_hal, &touch);
        if (err == ESP_OK && touch.pressed && !was_pressed) {
            if (h752_dag_monitor_handle_tap(&touch)) {
                h752_render_dag_monitor_current(true);
            }
        }
        if (err == ESP_OK) {
            was_pressed = touch.pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(H752_UI_TOUCH_POLL_MS));
    }
}
#endif
#endif

static esp_err_t log_tx_packet(rns_node_t *node,
                               const rns_packet_t *packet,
                               void *ctx)
{
    (void)node;
    (void)ctx;

    uint8_t packet_hash[RNS_DESTINATION_HASH_SIZE];
    esp_err_t err = rns_packet_truncated_hash(packet, packet_hash);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "reticulum tx type=%u context=0x%02x len=%u hash=%02x%02x%02x%02x",
             (unsigned)packet->packet_type,
             (unsigned)packet->context,
             (unsigned)packet->data_len,
             packet_hash[0],
             packet_hash[1],
             packet_hash[2],
             packet_hash[3]);
    return ESP_OK;
}

static esp_err_t runtime_tx_packet(const rns_packet_t *packet, void *ctx)
{
    rns_node_t *node = (rns_node_t *)ctx;
    if (node == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return rns_node_send_packet(node, packet);
}

static esp_err_t runtime_rx_packet(rns_node_t *node,
                                   const rns_packet_t *packet,
                                   void *ctx)
{
    meshpay_app_runtime_t *runtime = (meshpay_app_runtime_t *)ctx;
    if (runtime == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE &&
        !rns_destination_hash_equal(packet->destination_hash,
                                    s_app.local_destination) &&
        !meshpay_announce_reply_seen(packet->destination_hash, now_ms)) {
        meshpay_announce_reply_remember(packet->destination_hash, now_ms);
        esp_err_t announce_err = meshpay_send_announce(node, "peer-hello");
        if (announce_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "announce reply failed: %s",
                     esp_err_to_name(announce_err));
        }
    }

    const meshpay_app_event_t event = {
        .type = MESHPAY_APP_EVENT_RETICULUM_RX,
        .now_ms = now_ms,
        .packet = *packet,
    };
    return meshpay_app_runtime_post(runtime,
                                    MESHPAY_APP_QUEUE_RETICULUM,
                                    &event,
                                    0);
}

#if MESHPAY_RADIO_ENABLED
static void radio_poll_task(void *arg)
{
    (void)arg;
    while (true) {
        rns_transport_rx_result_t result = RNS_TRANSPORT_RX_ACCEPTED;
        esp_err_t err = rns_radio_poll_hal(&s_radio, &s_node, &result);
        if (err == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "radio poll failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGD(TAG, "radio rx result=%u", (unsigned)result);
    }
}
#endif

static void boot_announce_task(void *arg)
{
    rns_node_t *node = (rns_node_t *)arg;
    for (uint32_t i = 0; i < MESHPAY_BOOT_ANNOUNCE_COUNT; ++i) {
        esp_err_t err = meshpay_send_announce(
            node,
            i == 0 ? "boot" : "boot-retry");
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "boot announce failed: %s",
                     esp_err_to_name(err));
        }
        if (i + 1U < MESHPAY_BOOT_ANNOUNCE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(MESHPAY_BOOT_ANNOUNCE_INTERVAL_MS));
        }
    }
    s_boot_announce_task = NULL;
    vTaskDelete(NULL);
}

static uint32_t dag_summary_jitter_ms(void)
{
    uint32_t seed = ((uint32_t)s_app.local_destination[0] << 8) |
                    (uint32_t)s_app.local_destination[1];
    return 1000U + (seed % 7000U);
}

static void dag_summary_task(void *arg)
{
    (void)arg;
    const uint32_t initial_delay_ms = 3000U + dag_summary_jitter_ms();
    vTaskDelay(pdMS_TO_TICKS(initial_delay_ms));

    while (true) {
        const meshpay_app_event_t summary_event = {
            .type = MESHPAY_APP_EVENT_CORE_DAG_SUMMARY,
            .now_ms = (uint64_t)(esp_timer_get_time() / 1000),
        };
        esp_err_t err = meshpay_app_runtime_post(&s_runtime,
                                                 MESHPAY_APP_QUEUE_CORE,
                                                 &summary_event,
                                                 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "periodic DAG summary queue failed: %s",
                     esp_err_to_name(err));
        }
        if (xSemaphoreTake(s_runtime.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            uint8_t dag_digest[RNS_CRYPTO_SHA256_SIZE];
            size_t dag_n = meshpay_dag_count(&s_app.dag);
            esp_err_t digest_err = meshpay_dag_digest(&s_app.dag, dag_digest);
            xSemaphoreGive(s_runtime.lock);
            if (digest_err == ESP_OK) {
                ESP_LOGI(TAG, "dag_digest=%02x%02x%02x%02x count=%u",
                         dag_digest[0], dag_digest[1], dag_digest[2], dag_digest[3],
                         (unsigned)dag_n);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(MESHPAY_DAG_SUMMARY_INTERVAL_MS));
    }
}

#if MESHPAY_RADIO_ENABLED
static meshpay_board_t configured_board(void)
{
#if CONFIG_MESHPAY_BOARD_CYD
    return MESHPAY_BOARD_CYD;
#elif CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
    return MESHPAY_BOARD_WAVESHARE_S3_TOUCH;
#elif CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
    return MESHPAY_BOARD_LILYGO_T5S3_H752;
#elif CONFIG_MESHPAY_BOARD_LILYGO_TDECK
    /* T-Deck / T-Deck Plus : carte fondateur — drivers viendront dans les tâches suivantes */
    return MESHPAY_BOARD_LILYGO_TDECK;
#else
    return MESHPAY_BOARD_UNKNOWN;
#endif
}

static esp_err_t radio_combo_espnow_send(void *ctx,
                                         const uint8_t *data,
                                         size_t len)
{
    meshpay_radio_combo_t *combo = (meshpay_radio_combo_t *)ctx;
    if (combo == NULL || combo->espnow_hal == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return meshpay_hal_espnow_send(combo->espnow_hal, data, len);
}

static esp_err_t radio_combo_espnow_recv(void *ctx,
                                         uint8_t *data,
                                         size_t size,
                                         size_t *len)
{
    meshpay_radio_combo_t *combo = (meshpay_radio_combo_t *)ctx;
    if (combo == NULL || combo->espnow_hal == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    return meshpay_hal_espnow_recv(combo->espnow_hal, data, size, len);
}

static esp_err_t radio_combo_lora_send(void *ctx,
                                       const uint8_t *data,
                                       size_t len)
{
    meshpay_radio_combo_t *combo = (meshpay_radio_combo_t *)ctx;
    if (combo == NULL || combo->lora_hal == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return meshpay_hal_lora_send(combo->lora_hal, data, len);
}

static esp_err_t radio_combo_lora_recv(void *ctx,
                                       uint8_t *data,
                                       size_t size,
                                       size_t *len)
{
    meshpay_radio_combo_t *combo = (meshpay_radio_combo_t *)ctx;
    if (combo == NULL || combo->lora_hal == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    return meshpay_hal_lora_recv(combo->lora_hal, data, size, len);
}

static const meshpay_hal_ops_t RADIO_COMBO_OPS = {
    .espnow_send = radio_combo_espnow_send,
    .espnow_recv = radio_combo_espnow_recv,
    .lora_send = radio_combo_lora_send,
    .lora_recv = radio_combo_lora_recv,
};

static uint8_t meshpay_select_radio_bearer(const rns_packet_t *packet,
                                           void *ctx)
{
    (void)ctx;
    if (packet == NULL) {
        return 0;
    }
#if CONFIG_MESHPAY_FORCE_ESPNOW_ONLY
    /* Bascule provisoire : tout le trafic Reticulum (announce + DAG) passe par
     * ESP-NOW. Le bearer LoRa reste initialisé mais ne reçoit aucun paquet.
     * Réversible via Kconfig (MESHPAY_FORCE_ESPNOW_ONLY=n). */
    (void)packet;
    return RNS_RADIO_BEARER_ESPNOW;
#endif
    if (packet->packet_type == RNS_PACKET_TYPE_ANNOUNCE) {
        return RNS_RADIO_BEARER_ESPNOW;
    }
    if (packet->packet_type == RNS_PACKET_TYPE_DATA) {
        if (packet->context == RNS_PACKET_CONTEXT_RESOURCE ||
            packet->context == RNS_PACKET_CONTEXT_RESOURCE_ADV ||
            packet->context == RNS_PACKET_CONTEXT_RESOURCE_REQ ||
            packet->context == RNS_PACKET_CONTEXT_RESOURCE_HMU ||
            packet->context == RNS_PACKET_CONTEXT_RESOURCE_PRF ||
            packet->context == RNS_PACKET_CONTEXT_REQUEST ||
            packet->context == RNS_PACKET_CONTEXT_RESPONSE) {
            return RNS_RADIO_BEARER_LORA;
        }
        if (packet->destination_type == RNS_DESTINATION_TYPE_PLAIN &&
            packet->data_len > 0 &&
            packet->data[0] == MESHPAY_DAG_SYNC_MSG_SUMMARY) {
            return RNS_RADIO_BEARER_LORA;
        }
    }
    return RNS_RADIO_BEARER_ESPNOW;
}

static esp_err_t bind_radio_to_node(uint8_t bearer,
                                    const rns_node_callbacks_t *callbacks)
{
    esp_err_t err = rns_radio_init(&s_radio, &s_hal, bearer);
    if (err == ESP_OK) {
        err = rns_radio_set_bearer_selector(&s_radio,
                                            meshpay_select_radio_bearer,
                                            NULL);
    }
    if (err == ESP_OK) {
        err = rns_radio_bind_node(&s_radio_adapter,
                                  &s_radio,
                                  &s_node,
                                  callbacks);
    }
    return err;
}
#endif

static bool init_radio_if_available(const rns_node_callbacks_t *callbacks)
{
#if MESHPAY_RADIO_ENABLED
    uint8_t bearers = 0;
    memset(&s_radio_combo, 0, sizeof(s_radio_combo));

#if MESHPAY_RADIO_HAS_ESPNOW
    meshpay_hal_espnow_config_t espnow_config;
    meshpay_hal_espnow_default_config(&espnow_config);
    espnow_config.channel = CONFIG_MESHPAY_ESPNOW_CHANNEL;

    esp_err_t err = meshpay_hal_espnow_driver_init(&s_espnow_driver,
                                                   &s_espnow_hal,
                                                   configured_board(),
                                                   &espnow_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW radio unavailable: %s", esp_err_to_name(err));
    } else {
        s_radio_combo.espnow_hal = &s_espnow_hal;
        bearers |= RNS_RADIO_BEARER_ESPNOW;
        ESP_LOGI(TAG,
                 "ESP-NOW Reticulum bearer ready channel=%u",
                 (unsigned)espnow_config.channel);
    }
#endif

#if CONFIG_MESHPAY_RADIO_LORA_UART
    meshpay_hal_lora_uart_config_t lora_config;
    meshpay_hal_lora_uart_default_config(&lora_config);
    lora_config.uart_port = CONFIG_MESHPAY_LORA_UART_PORT;
    lora_config.tx_io = CONFIG_MESHPAY_LORA_UART_TX_IO;
    lora_config.rx_io = CONFIG_MESHPAY_LORA_UART_RX_IO;
    lora_config.baud_rate = CONFIG_MESHPAY_LORA_UART_BAUD;

    esp_err_t err = meshpay_hal_lora_uart_driver_init(&s_lora_uart_driver,
                                                      &s_lora_hal,
                                                      configured_board(),
                                                      &lora_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LoRa UART radio unavailable: %s", esp_err_to_name(err));
    } else {
        s_radio_combo.lora_hal = &s_lora_hal;
        bearers |= RNS_RADIO_BEARER_LORA;
        ESP_LOGI(TAG,
                 "LoRa UART Reticulum bearer ready uart=%d tx=%d rx=%d baud=%lu",
                 lora_config.uart_port,
                 lora_config.tx_io,
                 lora_config.rx_io,
                 (unsigned long)lora_config.baud_rate);
    }
#endif

#if MESHPAY_RADIO_HAS_LORA_CORE1262
    meshpay_hal_lora_core1262_config_t c1262_config;
    meshpay_hal_lora_core1262_default_config(&c1262_config);
    c1262_config.spi_host = CONFIG_MESHPAY_LORA_C1262_SPI_HOST;
    c1262_config.pin_sck = CONFIG_MESHPAY_LORA_C1262_PIN_SCK;
    c1262_config.pin_mosi = CONFIG_MESHPAY_LORA_C1262_PIN_MOSI;
    c1262_config.pin_miso = CONFIG_MESHPAY_LORA_C1262_PIN_MISO;
    c1262_config.pin_nss = CONFIG_MESHPAY_LORA_C1262_PIN_NSS;
    c1262_config.pin_reset = CONFIG_MESHPAY_LORA_C1262_PIN_RESET;
    c1262_config.pin_busy = CONFIG_MESHPAY_LORA_C1262_PIN_BUSY;
    c1262_config.pin_dio1 = CONFIG_MESHPAY_LORA_C1262_PIN_DIO1;
    c1262_config.pin_rxen = CONFIG_MESHPAY_LORA_C1262_PIN_RXEN;
    c1262_config.pin_txen = CONFIG_MESHPAY_LORA_C1262_PIN_TXEN;
    c1262_config.pin_aux_cs = CONFIG_MESHPAY_LORA_C1262_PIN_AUX_CS;
    c1262_config.frequency_hz = CONFIG_MESHPAY_LORA_C1262_FREQUENCY_HZ;
    c1262_config.tcxo_ctrl_voltage =
        CONFIG_MESHPAY_LORA_C1262_TCXO_CTRL_VOLTAGE;
    c1262_config.calibrate_image =
        MESHPAY_LORA_C1262_CALIBRATE_IMAGE_ENABLED;
    c1262_config.tx_power_dbm = CONFIG_MESHPAY_LORA_C1262_TX_POWER_DBM;

    esp_err_t c1262_err = meshpay_hal_lora_core1262_driver_init(
        &s_lora_core1262_driver,
        &s_lora_hal,
        configured_board(),
        &c1262_config);
    if (c1262_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Core1262 LoRa radio unavailable: %s",
                 esp_err_to_name(c1262_err));
    } else {
        s_radio_combo.lora_hal = &s_lora_hal;
        bearers |= RNS_RADIO_BEARER_LORA;
        ESP_LOGI(TAG,
                 "Core1262 LoRa Reticulum bearer ready spi=%d freq=%lu",
                 c1262_config.spi_host,
                 (unsigned long)c1262_config.frequency_hz);
    }
#endif

    if (bearers == 0) {
        ESP_LOGW(TAG, "No Reticulum radio bearer available");
        return false;
    }

    esp_err_t combo_err = meshpay_hal_init(&s_hal,
                                           configured_board(),
                                           &RADIO_COMBO_OPS,
                                           &s_radio_combo);
    if (combo_err != ESP_OK) {
        ESP_LOGW(TAG, "Reticulum radio HAL bind failed: %s",
                 esp_err_to_name(combo_err));
        return false;
    }

    combo_err = bind_radio_to_node(bearers, callbacks);
    if (combo_err != ESP_OK) {
        ESP_LOGW(TAG, "Reticulum radio bind failed: %s",
                 esp_err_to_name(combo_err));
#if MESHPAY_RADIO_HAS_ESPNOW
        (void)meshpay_hal_espnow_driver_deinit(&s_espnow_driver);
#endif
#if CONFIG_MESHPAY_RADIO_LORA_UART
        (void)meshpay_hal_lora_uart_driver_deinit(&s_lora_uart_driver);
#endif
#if MESHPAY_RADIO_HAS_LORA_CORE1262
        (void)meshpay_hal_lora_core1262_driver_deinit(&s_lora_core1262_driver);
#endif
        return false;
    }

    if (bearers == RNS_RADIO_BEARER_ALL) {
        s_radio_backend = "espnow+lora";
    } else if ((bearers & RNS_RADIO_BEARER_ESPNOW) != 0) {
        s_radio_backend = "espnow";
    } else {
        s_radio_backend = "lora";
    }
#if CONFIG_MESHPAY_FORCE_ESPNOW_ONLY
    ESP_LOGI(TAG,
             "Reticulum radio ready backend=%s policy=all:espnow (lora off)",
             s_radio_backend);
#else
    ESP_LOGI(TAG,
             "Reticulum radio ready backend=%s policy=discovery:espnow dag:lora",
             s_radio_backend);
#endif
    return true;
#else
    (void)callbacks;
    ESP_LOGI(TAG, "Reticulum radio disabled by configuration");
    return false;
#endif
}

#if CONFIG_MESHPAY_DAG_MONITOR_ONLY
static const char *dag_monitor_alert_level_label(
    meshpay_dag_monitor_alert_level_t level)
{
    switch (level) {
    case MESHPAY_DAG_MONITOR_ALERT_CRIT:
        return "CRIT";
    case MESHPAY_DAG_MONITOR_ALERT_WARN:
        return "WARN";
    case MESHPAY_DAG_MONITOR_ALERT_INFO:
    default:
        return "INFO";
    }
}

static const char *dag_monitor_alert_type_label(
    meshpay_dag_monitor_alert_type_t type)
{
    switch (type) {
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_LORA:
        return "LORA BAD";
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_PACKET:
        return "RNS BAD";
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC:
        return "DAG BAD";
    case MESHPAY_DAG_MONITOR_ALERT_PEER_TX_COUNT_REGRESSED:
        return "TX REGRESS";
    case MESHPAY_DAG_MONITOR_ALERT_PEER_SUMMARY_WITHOUT_TIPS:
        return "NO TIPS";
    default:
        return "UNKNOWN";
    }
}

static const char *dag_monitor_alert_value_label(
    meshpay_dag_monitor_alert_type_t type)
{
    switch (type) {
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_LORA:
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_PACKET:
    case MESHPAY_DAG_MONITOR_ALERT_MALFORMED_DAG_SYNC:
        return "LEN";
    case MESHPAY_DAG_MONITOR_ALERT_PEER_TX_COUNT_REGRESSED:
    case MESHPAY_DAG_MONITOR_ALERT_PEER_SUMMARY_WITHOUT_TIPS:
        return "TX";
    default:
        return "V";
    }
}

static const char *dag_monitor_freshness_label(uint64_t now_ms,
                                               uint64_t seen_ms)
{
    if (seen_ms == 0 || now_ms < seen_ms) {
        return "FROID";
    }

    uint64_t age_ms = now_ms - seen_ms;
    if (age_ms <= 30000ULL) {
        return "RECENT";
    }
    if (age_ms <= 300000ULL) {
        return "STALE";
    }
    return "FROID";
}

static bool dag_monitor_destination_zero(
    const uint8_t destination[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (destination == NULL) {
        return true;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < MESHPAY_TX_DESTINATION_HASH_SIZE; ++i) {
        acc |= destination[i];
    }
    return acc == 0;
}

static void dag_monitor_format_peer_line(
    const meshpay_dag_monitor_peer_t *peer,
    uint64_t now_ms,
    char out[MESHPAY_UI_TEXT_MAX])
{
    if (peer == NULL || out == NULL) {
        return;
    }
    (void)snprintf(out,
                   MESHPAY_UI_TEXT_MAX,
                   "%02X%02X%02X %s TX%u T%u",
                   peer->destination[0],
                   peer->destination[1],
                   peer->destination[2],
                   dag_monitor_freshness_label(now_ms, peer->last_seen_ms),
                   (unsigned)peer->tx_count,
                   (unsigned)peer->tip_count);
}

static void dag_monitor_format_alert_line(
    const meshpay_dag_monitor_alert_t *alert,
    char out[MESHPAY_UI_TEXT_MAX])
{
    if (alert == NULL || out == NULL) {
        return;
    }
    if (dag_monitor_destination_zero(alert->destination)) {
        (void)snprintf(out,
                       MESHPAY_UI_TEXT_MAX,
                       "%s -- %s %s%lu",
                       dag_monitor_alert_level_label(alert->level),
                       dag_monitor_alert_type_label(alert->type),
                       dag_monitor_alert_value_label(alert->type),
                       (unsigned long)alert->value);
    } else {
        (void)snprintf(out,
                       MESHPAY_UI_TEXT_MAX,
                       "%s %02X%02X%02X %s %s%lu",
                       dag_monitor_alert_level_label(alert->level),
                       alert->destination[0],
                       alert->destination[1],
                       alert->destination[2],
                       dag_monitor_alert_type_label(alert->type),
                       dag_monitor_alert_value_label(alert->type),
                       (unsigned long)alert->value);
    }
}

static void dag_monitor_refresh_ui_locked(void)
{
    meshpay_dag_monitor_snapshot_t snapshot;
    if (meshpay_dag_monitor_snapshot(&s_dag_monitor, &snapshot) != ESP_OK) {
        return;
    }

    meshpay_ui_dag_monitor_status_t status = {
        .lora_ready = s_dag_monitor_lora_ready,
        .health_score = snapshot.health_score,
        .peer_count = snapshot.peer_count > UINT8_MAX
                          ? UINT8_MAX
                          : (uint8_t)snapshot.peer_count,
        .alert_count = snapshot.alert_count > UINT8_MAX
                           ? UINT8_MAX
                           : (uint8_t)snapshot.alert_count,
        .lora_frames = snapshot.lora_frames,
        .rns_packets = snapshot.rns_packets,
        .dag_summaries = snapshot.dag_summaries,
        .dag_requests = snapshot.dag_requests,
        .resource_frames = snapshot.resource_frames,
        .dag_batches = snapshot.dag_batches,
        .tx_advertised = snapshot.tx_advertised,
        .tx_observed = snapshot.tx_observed,
        .announces = snapshot.announces,
        .unknown_packets = snapshot.unknown_packets,
        .malformed_lora_frames = snapshot.malformed_lora_frames,
        .malformed_rns_packets = snapshot.malformed_rns_packets,
        .malformed_dag_sync = snapshot.malformed_dag_sync,
        .malformed_frames = snapshot.malformed_lora_frames +
                            snapshot.malformed_rns_packets +
                            snapshot.malformed_dag_sync,
        .duplicate_packets = snapshot.duplicate_packets,
        .peer_regressions = snapshot.peer_regressions,
        .peer_summary_without_tips = snapshot.peer_summary_without_tips,
    };
#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
    uint16_t battery_mv = 0;
    uint8_t battery_percent = 0;
    if (meshpay_hal_battery_status(&s_display_hal,
                                   &battery_mv,
                                   &battery_percent) == ESP_OK) {
        status.battery_available = true;
        status.battery_mv = battery_mv;
        status.battery_percent = battery_percent > 100U ? 100U
                                                        : battery_percent;
    }
#endif
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    bool peer_used[MESHPAY_DAG_MONITOR_MAX_PEERS] = {0};
    while (status.peer_line_count <
           MESHPAY_UI_DAG_MONITOR_PEER_LINE_MAX) {
        size_t best_index = MESHPAY_DAG_MONITOR_MAX_PEERS;
        uint64_t best_seen_ms = 0;
        for (size_t i = 0; i < MESHPAY_DAG_MONITOR_MAX_PEERS; ++i) {
            if (!snapshot.peers[i].in_use || peer_used[i]) {
                continue;
            }
            if (best_index == MESHPAY_DAG_MONITOR_MAX_PEERS ||
                snapshot.peers[i].last_seen_ms >= best_seen_ms) {
                best_index = i;
                best_seen_ms = snapshot.peers[i].last_seen_ms;
            }
        }
        if (best_index == MESHPAY_DAG_MONITOR_MAX_PEERS) {
            break;
        }
        dag_monitor_format_peer_line(
            &snapshot.peers[best_index],
            now_ms,
            status.peer_lines[status.peer_line_count]);
        peer_used[best_index] = true;
        status.peer_line_count++;
    }
    const size_t alert_count =
        snapshot.alert_count < MESHPAY_DAG_MONITOR_ALERT_MAX
            ? snapshot.alert_count
            : MESHPAY_DAG_MONITOR_ALERT_MAX;
    for (size_t shown = 0; shown < alert_count &&
                            status.alert_line_count <
                                MESHPAY_UI_DAG_MONITOR_ALERT_LINE_MAX;
         ++shown) {
        size_t index =
            (snapshot.alert_next + MESHPAY_DAG_MONITOR_ALERT_MAX - 1U -
             shown) %
            MESHPAY_DAG_MONITOR_ALERT_MAX;
        dag_monitor_format_alert_line(
            &snapshot.alerts[index],
            status.alert_lines[status.alert_line_count]);
        status.alert_line_count++;
    }
    (void)snprintf(status.radio_label,
                   sizeof(status.radio_label),
                   "%s",
                   s_dag_monitor_lora_ready ? "LoRa OK" : "LoRa OFF");
    (void)meshpay_ui_set_dag_monitor(&s_dag_monitor_ui, &status);
}

static bool init_dag_monitor_lora_if_available(void)
{
#if MESHPAY_RADIO_HAS_LORA
#if CONFIG_MESHPAY_RADIO_LORA_UART
    meshpay_hal_lora_uart_config_t lora_config;
    meshpay_hal_lora_uart_default_config(&lora_config);
    lora_config.uart_port = CONFIG_MESHPAY_LORA_UART_PORT;
    lora_config.tx_io = CONFIG_MESHPAY_LORA_UART_TX_IO;
    lora_config.rx_io = CONFIG_MESHPAY_LORA_UART_RX_IO;
    lora_config.baud_rate = CONFIG_MESHPAY_LORA_UART_BAUD;

    esp_err_t err = meshpay_hal_lora_uart_driver_init(&s_lora_uart_driver,
                                                      &s_lora_hal,
                                                      configured_board(),
                                                      &lora_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "DAG monitor LoRa UART unavailable: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_radio_backend = "lora";
    ESP_LOGI(TAG, "DAG monitor LoRa UART RX ready");
    return true;
#endif

#if MESHPAY_RADIO_HAS_LORA_CORE1262
    meshpay_hal_lora_core1262_config_t c1262_config;
    meshpay_hal_lora_core1262_default_config(&c1262_config);
    c1262_config.spi_host = CONFIG_MESHPAY_LORA_C1262_SPI_HOST;
    c1262_config.pin_sck = CONFIG_MESHPAY_LORA_C1262_PIN_SCK;
    c1262_config.pin_mosi = CONFIG_MESHPAY_LORA_C1262_PIN_MOSI;
    c1262_config.pin_miso = CONFIG_MESHPAY_LORA_C1262_PIN_MISO;
    c1262_config.pin_nss = CONFIG_MESHPAY_LORA_C1262_PIN_NSS;
    c1262_config.pin_reset = CONFIG_MESHPAY_LORA_C1262_PIN_RESET;
    c1262_config.pin_busy = CONFIG_MESHPAY_LORA_C1262_PIN_BUSY;
    c1262_config.pin_dio1 = CONFIG_MESHPAY_LORA_C1262_PIN_DIO1;
    c1262_config.pin_rxen = CONFIG_MESHPAY_LORA_C1262_PIN_RXEN;
    c1262_config.pin_txen = CONFIG_MESHPAY_LORA_C1262_PIN_TXEN;
    c1262_config.pin_aux_cs = CONFIG_MESHPAY_LORA_C1262_PIN_AUX_CS;
    c1262_config.frequency_hz = CONFIG_MESHPAY_LORA_C1262_FREQUENCY_HZ;
    c1262_config.tcxo_ctrl_voltage =
        CONFIG_MESHPAY_LORA_C1262_TCXO_CTRL_VOLTAGE;
    c1262_config.calibrate_image =
        MESHPAY_LORA_C1262_CALIBRATE_IMAGE_ENABLED;
    c1262_config.tx_power_dbm = CONFIG_MESHPAY_LORA_C1262_TX_POWER_DBM;

    esp_err_t err = meshpay_hal_lora_core1262_driver_init(
        &s_lora_core1262_driver,
        &s_lora_hal,
        configured_board(),
        &c1262_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "DAG monitor Core1262 LoRa unavailable: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_radio_backend = "lora";
    ESP_LOGI(TAG,
             "DAG monitor Core1262 LoRa RX ready spi=%d freq=%lu",
             c1262_config.spi_host,
             (unsigned long)c1262_config.frequency_hz);
    return true;
#endif
#else
    ESP_LOGW(TAG, "DAG monitor LoRa disabled by configuration");
    return false;
#endif
}

#if MESHPAY_RADIO_HAS_LORA
static void dag_monitor_lora_task(void *arg)
{
    (void)arg;
    uint8_t frame[MESHPAY_HAL_PACKET_MAX];
    while (true) {
        size_t frame_len = 0;
        esp_err_t err = meshpay_hal_lora_recv(&s_lora_hal,
                                              frame,
                                              sizeof(frame),
                                              &frame_len);
        if (err == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "DAG monitor LoRa RX failed: %s",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xSemaphoreTake(s_dag_monitor_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            esp_err_t record_err = meshpay_dag_monitor_record_lora_frame(
                &s_dag_monitor,
                frame,
                frame_len,
                (uint64_t)(esp_timer_get_time() / 1000));
            dag_monitor_refresh_ui_locked();
            xSemaphoreGive(s_dag_monitor_lock);
            if (record_err != ESP_OK) {
                ESP_LOGD(TAG,
                         "DAG monitor rejected LoRa frame: %s",
                         esp_err_to_name(record_err));
            }
        }
    }
}
#endif

static void dag_monitor_render_task(void *arg)
{
    (void)arg;
    while (true) {
#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
        h752_render_dag_monitor_current(false);
#endif
        vTaskDelay(pdMS_TO_TICKS(MESHPAY_DAG_MONITOR_RENDER_POLL_MS));
    }
}

static void run_dag_monitor_app(void)
{
    (void)snprintf(s_device_alias, sizeof(s_device_alias), "dag monitor");
    s_dag_monitor_lock = xSemaphoreCreateMutex();
    if (s_dag_monitor_lock == NULL) {
        ESP_LOGE(TAG, "DAG monitor lock allocation failed");
        return;
    }

    meshpay_dag_monitor_init(&s_dag_monitor);
    meshpay_ui_init_monitor(&s_dag_monitor_ui);
    s_dag_monitor_lora_ready = init_dag_monitor_lora_if_available();

    if (xSemaphoreTake(s_dag_monitor_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        dag_monitor_refresh_ui_locked();
        xSemaphoreGive(s_dag_monitor_lock);
    }

#if CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
    h752_render_dag_monitor_current(true);
    if (s_lilygo_h752_display_driver.initialized &&
        xTaskCreate(h752_dag_monitor_touch_task,
                    "dag_mon_touch",
                    MESHPAY_DAG_MONITOR_UI_TASK_STACK_BYTES,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_dag_monitor_touch_task) != pdPASS) {
        ESP_LOGW(TAG, "DAG monitor touch task start failed");
    }
#endif

#if MESHPAY_RADIO_HAS_LORA
    if (s_dag_monitor_lora_ready &&
        xTaskCreate(dag_monitor_lora_task,
                    "dag_mon_lora",
                    MESHPAY_DAG_MONITOR_RADIO_TASK_STACK_BYTES,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_dag_monitor_radio_task) != pdPASS) {
        ESP_LOGW(TAG, "DAG monitor LoRa task start failed");
    }
#endif

    if (xTaskCreate(dag_monitor_render_task,
                    "dag_mon_ui",
                    MESHPAY_DAG_MONITOR_UI_TASK_STACK_BYTES,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_dag_monitor_render_task) != pdPASS) {
        ESP_LOGW(TAG, "DAG monitor render task start failed");
    }

    ESP_LOGI(TAG,
             "DAG monitor app ready mode=read-only bearer=%s",
             s_dag_monitor_lora_ready ? s_radio_backend : "none");
    uint64_t last_dag_summary_ms = (uint64_t)(esp_timer_get_time() / 1000);
    while (true) {
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        const meshpay_app_event_t refresh_event = {
            .type = MESHPAY_APP_EVENT_UI_REFRESH,
            .now_ms = now_ms,
        };
        (void)meshpay_app_runtime_post(&s_runtime,
                                       MESHPAY_APP_QUEUE_UI,
                                       &refresh_event,
                                       0);

        if (now_ms - last_dag_summary_ms >= MESHPAY_DAG_SUMMARY_INTERVAL_MS) {
            const meshpay_app_event_t summary_event = {
                .type = MESHPAY_APP_EVENT_CORE_DAG_SUMMARY,
                .now_ms = now_ms,
            };
            if (meshpay_app_runtime_post(&s_runtime,
                                         MESHPAY_APP_QUEUE_CORE,
                                         &summary_event,
                                         0) == ESP_OK) {
                last_dag_summary_ms = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MESHPAY_UI_REFRESH_INTERVAL_MS));
    }
}
#endif

static void init_display_if_available(void)
{
#if CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
    esp_err_t err = meshpay_hal_waveshare_s3_touch_driver_init(
        &s_waveshare_display_driver,
        &s_display_hal);
    if (err == ESP_OK) {
        err = meshpay_hal_display_init(&s_display_hal);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Waveshare display/touch HAL ready");
    } else {
        ESP_LOGW(TAG,
                 "Waveshare display/touch HAL unavailable: %s",
                 esp_err_to_name(err));
    }
#elif CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752
    esp_err_t err = meshpay_hal_lilygo_t5s3_h752_driver_init(
        &s_lilygo_h752_display_driver,
        &s_display_hal);
    if (err == ESP_OK) {
        err = meshpay_hal_display_init(&s_display_hal);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LilyGo H752 display/touch HAL ready");
    } else {
        ESP_LOGW(TAG,
                 "LilyGo H752 display/touch HAL unavailable: %s",
                 esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "display/touch HAL disabled for this board");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s firmware boot ready (schema v%u)",
             meshpay_project_skeleton_name(),
             (unsigned)meshpay_project_skeleton_schema_version());
    ESP_LOGI(TAG, "crypto profile: %s", RNS_CRYPTO_SIGNATURE_SCHEME);
    init_display_if_available();

#if CONFIG_MESHPAY_DAG_MONITOR_ONLY
    run_dag_monitor_app();
    return;
#endif

    rns_identity_t identity;
    meshpay_storage_record_t boot_record;
    meshpay_storage_record_init(&boot_record);
    meshpay_storage_backend_t storage_backend = {0};
    bool identity_created = false;
    bool storage_ready = false;
    uint32_t next_seq = 1;
    char default_alias[MESHPAY_STORAGE_ALIAS_MAX] = "renard malin";
    if (meshpay_app_generate_alias(default_alias, sizeof(default_alias)) !=
        ESP_OK) {
        (void)snprintf(default_alias, sizeof(default_alias), "renard malin");
    }
    (void)snprintf(s_device_alias, sizeof(s_device_alias), "%s",
                   default_alias);

    esp_err_t storage_err = meshpay_storage_nvs_init();
    esp_err_t err = storage_err;
    if (storage_err == ESP_OK) {
        storage_backend = meshpay_storage_nvs_backend();
        err = meshpay_app_bootstrap_identity(&storage_backend,
                                             default_alias,
                                             &identity,
                                             &boot_record,
                                             &identity_created);
        if (err == ESP_OK) {
            esp_err_t alias_err = meshpay_app_ensure_record_alias(
                &storage_backend,
                &boot_record);
            if (alias_err != ESP_OK) {
                ESP_LOGW(TAG, "alias ensure failed: %s",
                         esp_err_to_name(alias_err));
            }
            next_seq = boot_record.next_seq == 0 ? 1 : boot_record.next_seq;
            storage_ready = true;
            if (boot_record.alias[0] != '\0') {
                (void)snprintf(s_device_alias,
                               sizeof(s_device_alias),
                               "%s",
                               boot_record.alias);
            }
            ESP_LOGI(TAG, "identity %s from NVS next_seq=%u",
                     identity_created ? "created" : "loaded",
                     (unsigned)next_seq);
        }
    }
    if (storage_err != ESP_OK || err != ESP_OK) {
        ESP_LOGW(TAG, "persistent identity unavailable: nvs=%s boot=%s",
                 esp_err_to_name(storage_err),
                 esp_err_to_name(err));
        err = rns_identity_generate(&identity);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "identity generation failed: %s",
                     esp_err_to_name(err));
            return;
        }
    }
    ESP_LOGI(TAG, "device alias %s", s_device_alias);

    err = rns_node_init(&s_node, &identity);
    rns_identity_clear(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rns node init failed: %s", esp_err_to_name(err));
        return;
    }

    const rns_node_callbacks_t callbacks = {
        .tx = log_tx_packet,
    };
    err = rns_node_set_callbacks(&s_node, &callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rns callbacks init failed: %s", esp_err_to_name(err));
        return;
    }

    const rns_destination_t *destination = rns_node_destination(&s_node);
    ESP_LOGI(TAG,
             "local destination %s %02x%02x%02x%02x...",
             destination->full_name,
             destination->hash[0],
             destination->hash[1],
             destination->hash[2],
             destination->hash[3]);

    meshpay_currency_config_t currency;
    meshpay_currency_config_init(&currency, 1);
    currency.transfer_fee = 0;
    err = meshpay_currency_add_mint_authority(&currency, destination->hash);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "currency init failed: %s", esp_err_to_name(err));
        return;
    }

    err = meshpay_app_init(&s_app,
                           destination->hash,
                           &s_node.identity,
                           &currency,
                           next_seq,
                           storage_ready && boot_record.has_pin_hash);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "meshpay app init failed: %s", esp_err_to_name(err));
        return;
    }
    char local_id[MESHPAY_UI_ID_LABEL_MAX] = {0};
    meshpay_short_destination(destination->hash, local_id);
    (void)meshpay_ui_set_local_identity(&s_app.ui,
                                        s_device_alias,
                                        local_id);
    if (storage_ready && boot_record.has_pin_hash) {
        err = meshpay_wallet_load_pin_hash(&s_app.wallet,
                                           boot_record.pin_hash);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stored pin hash load failed: %s",
                     esp_err_to_name(err));
            return;
        }
    }
    if (storage_ready) {
        err = create_boot_credit_once(&s_app,
                                      &storage_backend,
                                      &boot_record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot credit failed: %s", esp_err_to_name(err));
            return;
        }
    } else {
        ESP_LOGW(TAG, "boot credit skipped: persistent storage unavailable");
    }

    err = meshpay_app_runtime_init(&s_runtime, &s_app, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime init failed: %s", esp_err_to_name(err));
        return;
    }

    err = meshpay_app_runtime_set_packet_tx(&s_runtime,
                                            runtime_tx_packet,
                                            &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime tx hook failed: %s", esp_err_to_name(err));
        return;
    }
    if (storage_ready) {
        err = meshpay_app_runtime_set_storage(&s_runtime,
                                              &storage_backend,
                                              &boot_record);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "runtime storage disabled: %s",
                     esp_err_to_name(err));
        }
    }

    /* Persistance durable de la DAG (Phase A) : restaurer la fenetre depuis la
     * flash AVANT le demarrage de la sync, puis brancher le backend pour les
     * sauvegardes ulterieures. Degradation gracieuse si la partition est absente. */
    {
        meshpay_dag_store_backend_t dag_store_be;
        esp_err_t derr =
            meshpay_dag_store_partition_backend("dagstore", &dag_store_be);
        if (derr == ESP_OK) {
            esp_err_t lerr = meshpay_dag_store_load(&dag_store_be, &s_app.dag);
            if (lerr == ESP_OK) {
                restore_next_seq_from_dag(&s_app);
                (void)refresh_app_balance(&s_app, 0);
                ESP_LOGI(TAG,
                         "dag restored from flash count=%u next_seq=%u",
                         (unsigned)meshpay_dag_count(&s_app.dag),
                         (unsigned)s_app.wallet.next_seq);
            } else if (lerr == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "dag store empty (first boot)");
            } else {
                ESP_LOGW(TAG, "dag store load err=%s", esp_err_to_name(lerr));
            }
            (void)meshpay_app_runtime_set_dag_store(&s_runtime, &dag_store_be);
        } else {
            ESP_LOGW(TAG, "dag store partition absent: %s",
                     esp_err_to_name(derr));
        }
    }

    const rns_node_callbacks_t runtime_callbacks = {
        .tx = log_tx_packet,
        .rx = runtime_rx_packet,
        .proof = runtime_rx_packet,
        .request = runtime_rx_packet,
        .ctx = &s_runtime,
    };
    err = rns_node_set_callbacks(&s_node, &runtime_callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime rx hook failed: %s", esp_err_to_name(err));
        return;
    }

    bool radio_ready = init_radio_if_available(&runtime_callbacks);

    err = meshpay_app_runtime_start_tasks(&s_runtime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime task start failed: %s", esp_err_to_name(err));
        return;
    }

#if MESHPAY_RADIO_ENABLED
    if (radio_ready &&
        xTaskCreate(radio_poll_task,
                    RADIO_TASK_NAME,
                    MESHPAY_RADIO_TASK_STACK_BYTES,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_radio_task) != pdPASS) {
        ESP_LOGW(TAG, "radio task start failed");
    }
#endif

    if (xTaskCreate(boot_announce_task,
                    "announce_task",
                    MESHPAY_APP_TASK_STACK_WORDS,
                    &s_node,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_boot_announce_task) != pdPASS) {
        ESP_LOGW(TAG, "announce task start failed, sending one announce");
        err = meshpay_send_announce(&s_node, "boot-fallback");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "initial announce failed: %s", esp_err_to_name(err));
            return;
        }
    }

    const meshpay_app_event_t ui_event = {
        .type = MESHPAY_APP_EVENT_UI_REFRESH,
    };
    (void)meshpay_app_runtime_post(&s_runtime,
                                   MESHPAY_APP_QUEUE_UI,
                                   &ui_event,
                                   0);
#if CONFIG_MESHPAY_BOARD_WAVESHARE_S3_TOUCH
    waveshare_render_current(true);
    if (s_waveshare_display_driver.initialized &&
        xTaskCreate(waveshare_touch_task,
                    TOUCH_TASK_NAME,
                    MESHPAY_APP_TASK_STACK_WORDS,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_waveshare_touch_task) != pdPASS) {
        ESP_LOGW(TAG, "touch task start failed");
    }
#endif

    const meshpay_app_event_t dag_summary_event = {
        .type = MESHPAY_APP_EVENT_CORE_DAG_SUMMARY,
        .now_ms = (uint64_t)(esp_timer_get_time() / 1000),
    };
    (void)meshpay_app_runtime_post(&s_runtime,
                                   MESHPAY_APP_QUEUE_CORE,
                                   &dag_summary_event,
                                   0);
    if (radio_ready &&
        xTaskCreate(dag_summary_task,
                    "dag_summary_task",
                    MESHPAY_APP_TASK_STACK_WORDS,
                    NULL,
                    MESHPAY_APP_TASK_PRIORITY,
                    &s_dag_summary_task) != pdPASS) {
        ESP_LOGW(TAG, "DAG summary task start failed");
    }

    const rns_node_stats_t *stats = rns_node_stats(&s_node);
    ESP_LOGI(TAG, "reticulum node ready tx=%u tasks=%s,%s,%s,%s radio=%s",
             (unsigned)stats->tx_packets,
             MESHPAY_APP_UI_TASK_NAME,
             MESHPAY_APP_RETICULUM_TASK_NAME,
             MESHPAY_APP_CORE_TASK_NAME,
             RADIO_TASK_NAME,
             radio_ready ? s_radio_backend : "disabled");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
