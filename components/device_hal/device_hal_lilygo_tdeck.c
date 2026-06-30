#include "meshpay/device_hal.h"

#include "sdkconfig.h"

#include <string.h>

/* Driver de la carte LILYGO T-Deck / T-Deck Plus (carte fondateur).
 *
 * Phase 1 : décodage clavier pur (testable hors banc).
 * Phase 2 : alimentation + écran ST7789 SPI (init + remplissage bleu + rétroéclairage).
 *
 * Phases suivantes (incréments ultérieurs) : tactile GT911, clavier I2C, LoRa SX1262,
 * batterie — NON implémentés ici.
 *
 * Pinout écran ST7789 (paysage 320×240) :
 *   CS=12  DC=11  MOSI=41  SCK=40  MISO=38  RST=-1 (pas de reset GPIO)  BL=42
 * Alimentation :
 *   KB_POWERON=10  (HIGH avant tout autre accès matériel)
 *   SD_CS=39       (tenir HAUT pour éviter de perturber le bus SPI partagé) */

/* ──────────────────────────────────────────────────────────────────────────
 * Pins et constantes privées
 * ────────────────────────────────────────────────────────────────────────── */

/* Alimentation / arbitrage bus */
#define TDECK_PIN_KB_POWERON 10  /* GPIO à mettre HAUT avant toute init matérielle */
#define TDECK_PIN_SD_CS      39  /* CS de la carte SD : tenir HAUT (bus SPI partagé) */

/* Bus I2C partagé tactile GT911 + clavier ESP32-C3 */
#define TDECK_PIN_SDA  18   /* SDA du bus I2C partagé */
#define TDECK_PIN_SCL   8   /* SCL du bus I2C partagé */

/* GT911 : contrôleur tactile capacitif */
#define TDECK_TOUCH_ADDR        0x5D   /* Adresse I2C du GT911 */
#define TDECK_GT911_REG_STATUS  0x814E /* Registre statut GT911 : nombre de points + bit 0x80 */

/* Clavier T-Deck : microcontrôleur ESP32-C3 embarqué (@0x55)
 * Retourne 0 si aucune touche, sinon le code ASCII de la touche pressée. */
#define TDECK_KEYBOARD_ADDR  0x55

/* Écran ST7789 SPI */
#define TDECK_PIN_CS   12
#define TDECK_PIN_DC   11
#define TDECK_PIN_MOSI 41
#define TDECK_PIN_SCK  40
#define TDECK_PIN_MISO 38
/* RST = -1 : pas de broche reset GPIO sur le T-Deck, reset logiciel (SWRESET) uniquement */
#define TDECK_PIN_BL   42

/* Fréquence SPI : 40 MHz — ST7789 supporte jusqu'à 80 MHz mais on reste conservatif */
#define ST7789_SPI_CLOCK_HZ       (40 * 1000 * 1000)
/* Taille max d'un transfert DMA SPI (doit être multiple de 4 pour certains DMA) */
#define ST7789_SPI_MAX_TRANSFER   (32 * 1024)
/* Taille des chunks utilisés pour envoyer les pixels (en octets) */
#define ST7789_CHUNK_BYTES        512

/* Rétroéclairage LEDC — canal 0, timer 0, PWM 8 bits, 5 kHz */
#define TDECK_BL_LEDC_TIMER      LEDC_TIMER_0
#define TDECK_BL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define TDECK_BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define TDECK_BL_LEDC_FREQ_HZ    5000
#define TDECK_BL_LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define TDECK_BL_LEDC_MAX_DUTY   255

/* Commandes ST7789 */
#define ST7789_CMD_SWRESET 0x01
#define ST7789_CMD_SLPOUT  0x11
#define ST7789_CMD_NORON   0x13
#define ST7789_CMD_INVON   0x21
#define ST7789_CMD_CASET   0x2A
#define ST7789_CMD_RASET   0x2B
#define ST7789_CMD_RAMWR   0x2C
#define ST7789_CMD_COLMOD  0x3A
#define ST7789_CMD_MADCTL  0x36
#define ST7789_CMD_DISPON  0x29

/* COLMOD 0x55 = RGB565, 16 bits par pixel */
#define ST7789_COLMOD_RGB565 0x55
/* MADCTL 0x60 = MV (swap axes) + MX (miroir X) → paysage 320×240 */
#define ST7789_MADCTL_LANDSCAPE 0x60

/* Couleur de boot (bleu pur en RGB565 big-endian : R=0 G=0 B=31) */
#define TDECK_BOOT_FILL_RGB565 0x001F

/* ──────────────────────────────────────────────────────────────────────────
 * Section compilée uniquement sur ESP32-S3 (cible T-Deck)
 * ────────────────────────────────────────────────────────────────────────── */
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Forward decl : la lecture batterie est appelée depuis display_init (log de boot)
 * alors que sa définition est plus bas (près de la table d'ops). */
static esp_err_t tdeck_battery_mv(void *ctx, uint16_t *mv);

static const char *TAG = "hal_tdeck";

/* ── Helpers SPI ─────────────────────────────────────────────────────────── */

/* Envoie un octet de commande (DC=0) via polling SPI. */
static esp_err_t send_cmd(meshpay_hal_lilygo_tdeck_driver_t *driver, uint8_t cmd)
{
    gpio_set_level(TDECK_PIN_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .flags  = SPI_TRANS_USE_TXDATA,
    };
    t.tx_data[0] = cmd;
    return spi_device_polling_transmit((spi_device_handle_t)driver->spi_handle, &t);
}

/* Envoie un buffer de données (DC=1) par chunks de ST7789_CHUNK_BYTES octets.
 * Utilise tx_data inline si le chunk tient dans 4 octets, sinon tx_buffer. */
static esp_err_t send_data(meshpay_hal_lilygo_tdeck_driver_t *driver,
                           const uint8_t *data,
                           size_t len)
{
    if (driver == NULL || driver->spi_handle == NULL ||
        (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    gpio_set_level(TDECK_PIN_DC, 1);

    size_t offset = 0;
    while (offset < len) {
        const size_t remaining = len - offset;
        const size_t chunk = (remaining < ST7789_CHUNK_BYTES)
                                 ? remaining
                                 : ST7789_CHUNK_BYTES;
        spi_transaction_t t = {
            .length = chunk * 8U,
        };
        if (chunk <= sizeof(t.tx_data)) {
            /* Transfert inline : évite une allocation DMA pour les petits buffers */
            t.flags = SPI_TRANS_USE_TXDATA;
            memcpy(t.tx_data, data + offset, chunk);
        } else {
            t.tx_buffer = data + offset;
        }

        const esp_err_t err =
            spi_device_polling_transmit((spi_device_handle_t)driver->spi_handle, &t);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

/* Raccourci : commande + 1 octet de paramètre. */
static esp_err_t send_cmd_data(meshpay_hal_lilygo_tdeck_driver_t *driver,
                               uint8_t cmd,
                               uint8_t param)
{
    esp_err_t err = send_cmd(driver, cmd);
    if (err != ESP_OK) {
        return err;
    }
    return send_data(driver, &param, 1);
}

/* ── Init I2C (API legacy) ───────────────────────────────────────────────── */

/* Adresse alternative du GT911 (selon le niveau d'INT au reset). */
#define TDECK_TOUCH_ADDR_ALT 0x14

/* Adresse I2C du tactile GT911 détectée au runtime (0 = absent). */
static uint8_t s_tdeck_touch_addr = 0;

/* Teste si une adresse I2C 7 bits répond (ACK), via une lecture d'1 octet.
 * Lecture (et non écriture) pour éviter tout effet de bord sur les périphériques. */
static bool tdeck_i2c_probe(uint8_t addr)
{
    uint8_t b = 0;
    return i2c_master_read_from_device(I2C_NUM_0, addr, &b, 1,
                                       pdMS_TO_TICKS(20)) == ESP_OK;
}

/* Scanne le bus I2C et logue les adresses qui répondent (diagnostic banc). */
static void tdeck_i2c_scan(void)
{
    int count = 0;
    ESP_LOGI(TAG, "I2C scan (0x08-0x77)...");
    for (uint8_t a = 0x08; a <= 0x77; ++a) {
        if (tdeck_i2c_probe(a)) {
            ESP_LOGI(TAG, "  I2C présent à 0x%02x", a);
            count++;
        }
    }
    ESP_LOGI(TAG, "I2C scan terminé : %d périphérique(s)", count);
}

/* Détecte l'adresse du GT911 : essaie 0x5D puis 0x14, mémorise dans
 * s_tdeck_touch_addr (0 si absent). */
static void tdeck_detect_touch(void)
{
    if (tdeck_i2c_probe(TDECK_TOUCH_ADDR)) {
        s_tdeck_touch_addr = TDECK_TOUCH_ADDR;
    } else if (tdeck_i2c_probe(TDECK_TOUCH_ADDR_ALT)) {
        s_tdeck_touch_addr = TDECK_TOUCH_ADDR_ALT;
    } else {
        s_tdeck_touch_addr = 0;
    }
    ESP_LOGI(TAG, "GT911 tactile : %s (addr=0x%02x)",
             s_tdeck_touch_addr ? "détecté" : "ABSENT",
             (unsigned)s_tdeck_touch_addr);
}

/* Initialise le bus I2C_NUM_0 en maître à 400 kHz avec les pins SDA=18, SCL=8.
 * Tolère ESP_ERR_INVALID_STATE si le bus est déjà installé (idempotent). */
static esp_err_t tdeck_init_i2c(void)
{
    const i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TDECK_PIN_SDA,
        .scl_io_num       = TDECK_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags        = 0,
    };
    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config échec : %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Déjà installé — on continue sans erreur */
        ESP_LOGD(TAG, "I2C_NUM_0 déjà installé, partage du bus");
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C_NUM_0 init OK (SDA=%d SCL=%d 400kHz)",
                 TDECK_PIN_SDA, TDECK_PIN_SCL);
        tdeck_i2c_scan();       /* diagnostic : liste ce qui répond sur le bus */
        tdeck_detect_touch();   /* fixe s_tdeck_touch_addr (0x5D / 0x14 / absent) */
    } else {
        ESP_LOGE(TAG, "I2C driver install échec : %s", esp_err_to_name(err));
    }
    return err;
}

/* ── Helpers I2C bas niveau ──────────────────────────────────────────────── */

/* Lecture depuis un registre 16 bits d'un périphérique I2C (adresse 7 bits).
 * Écrit d'abord les 2 octets du registre (big-endian), puis lit `len` octets. */
static esp_err_t i2c_read_reg16(uint8_t addr, uint16_t reg,
                                 uint8_t *data, size_t len)
{
    const uint8_t rb[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_write_read_device(I2C_NUM_0,
                                        addr,
                                        rb, sizeof(rb),
                                        data, len,
                                        pdMS_TO_TICKS(80));
}

/* Écriture vers un registre 16 bits d'un périphérique I2C.
 * Construit un buffer [reg_hi, reg_lo, data...] et envoie en une transaction. */
static esp_err_t i2c_write_reg16(uint8_t addr, uint16_t reg,
                                  const uint8_t *data, size_t len)
{
    /* Buffer temporaire : 2 octets de registre + données (max pratique : 8 o) */
    uint8_t buf[2 + 8];
    if (len > sizeof(buf) - 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)reg;
    memcpy(buf + 2, data, len);
    return i2c_master_write_to_device(I2C_NUM_0,
                                      addr,
                                      buf, 2U + len,
                                      pdMS_TO_TICKS(80));
}

/* ── Lecture tactile GT911 ───────────────────────────────────────────────── */

/* Lit l'état du tactile GT911 via I2C et remplit `state`.
 *
 * Protocole GT911 :
 *   1. Lire 9 octets depuis le registre 0x814E (statut + 8 octets du 1er point).
 *   2. Appeler meshpay_hal_gt911_decode_raw pour extraire pressed/raw_x/raw_y.
 *   3. Si bit 0x80 du 1er octet est positionné, acquitter en écrivant 0 à 0x814E.
 *
 * Passthrough coordonnées sans transformation (calibration ultérieure). */
static esp_err_t tdeck_touch_read(void *ctx, meshpay_touch_state_t *state)
{
    (void)ctx;
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Réinitialise l'état : pas de pression par défaut */
    state->pressed = false;
    state->x = 0;
    state->y = 0;

    /* GT911 non détecté au boot (ni 0x5D ni 0x14) → pas de tactile disponible. */
    if (s_tdeck_touch_addr == 0) {
        return ESP_OK;
    }

    /* Lecture de la trame statut GT911 : 1 octet statut + 8 octets données point 1 */
    uint8_t frame[9];
    esp_err_t err = i2c_read_reg16(s_tdeck_touch_addr, TDECK_GT911_REG_STATUS,
                                    frame, sizeof(frame));
    if (err != ESP_OK) {
        /* Pas de pression ou périphérique absent : on retourne ESP_OK (état = non pressé) */
        return ESP_OK;
    }

    /* Variables temporaires uint16_t requises par la signature de gt911_decode_raw
     * (coordonnées brutes GT911 sont toujours positives). */
    bool pressed = false;
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    err = meshpay_hal_gt911_decode_raw(frame, sizeof(frame),
                                       &pressed, &raw_x, &raw_y);
    if (err != ESP_OK) {
        return ESP_OK; /* décodage échoué : état non pressé, non fatal */
    }

    state->pressed = pressed;
    /* Conversion uint16_t → int16_t : les coords GT911 sont dans [0..479], jamais négatives. */
    state->x = (int16_t)raw_x;
    state->y = (int16_t)raw_y;

    /* Acquittement GT911 : si le bit 0x80 du registre statut est positionné,
     * l'hôte doit écrire 0 à 0x814E pour permettre la mise à jour suivante. */
    if ((frame[0] & 0x80U) != 0U) {
        const uint8_t clear = 0x00;
        (void)i2c_write_reg16(s_tdeck_touch_addr, TDECK_GT911_REG_STATUS,
                               &clear, 1);
    }

    return ESP_OK;
}

/* ── Lecture clavier I2C (ESP32-C3 @0x55) ───────────────────────────────── */

/* Lit un octet ASCII depuis le contrôleur clavier I2C du T-Deck.
 *
 * Le clavier (ESP32-C3 @0x55) renvoie 0 si aucune touche n'est pressée,
 * sinon le code ASCII brut. On passe par meshpay_hal_tdeck_keyboard_decode
 * pour filtrer les codes invalides et normaliser. */
static esp_err_t tdeck_keyboard_read(void *ctx, uint8_t *out_ascii)
{
    (void)ctx;
    if (out_ascii == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ascii = 0;

    uint8_t raw = 0;
    esp_err_t err = i2c_master_read_from_device(I2C_NUM_0,
                                                 TDECK_KEYBOARD_ADDR,
                                                 &raw, 1,
                                                 pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        /* Aucune touche disponible ou erreur de bus : on retourne ESP_OK (key=0) */
        return ESP_OK;
    }

    bool has_key = false;
    char ch = 0;
    err = meshpay_hal_tdeck_keyboard_decode(raw, &has_key, &ch);
    if (err != ESP_OK) {
        return ESP_OK; /* décodage échoué : pas de touche, non fatal */
    }
    *out_ascii = has_key ? (uint8_t)ch : 0U;
    return ESP_OK;
}

/* ── Alimentation ────────────────────────────────────────────────────────── */

/* Met sous tension la section clavier/alimentation du T-Deck et maintient
 * le CS de la carte SD à l'état HAUT pour ne pas perturber le bus SPI partagé.
 * Doit être appelé en PREMIER, avant toute autre initialisation matérielle. */
static esp_err_t tdeck_power_on(void)
{
    /* Configuration des GPIOs de contrôle en sortie */
    const uint64_t output_mask =
        (1ULL << TDECK_PIN_KB_POWERON) |
        (1ULL << TDECK_PIN_SD_CS)      |
        (1ULL << TDECK_PIN_DC)         |
        (1ULL << TDECK_PIN_CS);
    gpio_config_t cfg = {
        .pin_bit_mask   = output_mask,
        .mode           = GPIO_MODE_OUTPUT,
        .pull_up_en     = GPIO_PULLUP_DISABLE,
        .pull_down_en   = GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* KB_POWERON HIGH : active l'alimentation du clavier et des périphériques */
    gpio_set_level(TDECK_PIN_KB_POWERON, 1);
    /* SD_CS HIGH : déselectionne la SD card (bus SPI partagé) */
    gpio_set_level(TDECK_PIN_SD_CS, 1);
    /* CS écran HIGH : déselectionne l'écran pendant le démarrage */
    gpio_set_level(TDECK_PIN_CS, 1);
    /* DC : état quelconque pour l'instant */
    gpio_set_level(TDECK_PIN_DC, 1);

    /* Délai de stabilisation de l'alimentation avant tout accès SPI */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "T-Deck power on (KB_POWERON=GPIO%u HIGH, SD_CS=GPIO%u HIGH)",
             (unsigned)TDECK_PIN_KB_POWERON, (unsigned)TDECK_PIN_SD_CS);
    return ESP_OK;
}

/* ── Init SPI ────────────────────────────────────────────────────────────── */

/* Initialise le bus SPI2 et y attache le device ST7789.
 * Tolère ESP_ERR_INVALID_STATE si le bus est déjà initialisé (partagé avec LoRa). */
static esp_err_t init_spi(meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = TDECK_PIN_MOSI,
        .miso_io_num     = TDECK_PIN_MISO,
        .sclk_io_num     = TDECK_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ST7789_SPI_MAX_TRANSFER,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Bus déjà initialisé (partagé avec LoRa) — on continue */
        ESP_LOGD(TAG, "SPI2 bus déjà initialisé, on partage");
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = ST7789_SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = TDECK_PIN_CS,
        .queue_size     = 4,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t handle = NULL;
    err = spi_bus_add_device(SPI2_HOST, &dev, &handle);
    if (err != ESP_OK) {
        return err;
    }

    driver->spi_handle = (void *)handle;
    return ESP_OK;
}

/* ── Rétroéclairage LEDC ─────────────────────────────────────────────────── */

/* Configure le rétroéclairage via LEDC PWM 8 bits à 5 kHz, duty=255 (pleine
 * luminosité). Pattern identique au Waveshare S3. */
static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = TDECK_BL_LEDC_MODE,
        .timer_num       = TDECK_BL_LEDC_TIMER,
        .duty_resolution = TDECK_BL_LEDC_RESOLUTION,
        .freq_hz         = TDECK_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t ch = {
        .speed_mode = TDECK_BL_LEDC_MODE,
        .channel    = TDECK_BL_LEDC_CHANNEL,
        .timer_sel  = TDECK_BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = TDECK_PIN_BL,
        .duty       = TDECK_BL_LEDC_MAX_DUTY,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        return err;
    }

    err = ledc_update_duty(TDECK_BL_LEDC_MODE, TDECK_BL_LEDC_CHANNEL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "T-Deck backlight LEDC GPIO%u duty=%u",
                 (unsigned)TDECK_PIN_BL, (unsigned)TDECK_BL_LEDC_MAX_DUTY);
    }
    return err;
}

/* ── Séquence d'init ST7789 ─────────────────────────────────────────────── */

/* Initialise le contrôleur ST7789 en mode paysage 320×240, RGB565.
 * Séquence standard validée sur ST7789V2 / T-Deck :
 *   SWRESET → SLPOUT → COLMOD (RGB565) → MADCTL (paysage) → INVON → NORON → DISPON */
static esp_err_t run_st7789_init(meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    esp_err_t err;

    /* Reset logiciel — pas de GPIO reset sur le T-Deck */
    err = send_cmd(driver, ST7789_CMD_SWRESET);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Sortie du mode sleep */
    err = send_cmd(driver, ST7789_CMD_SLPOUT);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Format de couleur : RGB565 (16 bits/pixel) */
    err = send_cmd_data(driver, ST7789_CMD_COLMOD, ST7789_COLMOD_RGB565);
    if (err != ESP_OK) return err;

    /* Orientation paysage : MV (swap axes) + MX (miroir X).
     * À affiner au banc si l'image apparaît retournée ou miroir. */
    err = send_cmd_data(driver, ST7789_CMD_MADCTL, ST7789_MADCTL_LANDSCAPE);
    if (err != ESP_OK) return err;

    /* Inversion ON : requise sur ST7789 pour des couleurs correctes */
    err = send_cmd(driver, ST7789_CMD_INVON);
    if (err != ESP_OK) return err;

    /* Normal display ON */
    err = send_cmd(driver, ST7789_CMD_NORON);
    if (err != ESP_OK) return err;

    /* Display ON */
    err = send_cmd(driver, ST7789_CMD_DISPON);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

/* ── Flush / remplissage ─────────────────────────────────────────────────── */

/* Définit la fenêtre d'écriture plein écran ST7789 (CASET + RASET) et envoie
 * RAMWR, puis les pixels RGB565 fournis convertis en big-endian.
 *
 * pixels : tableau de width×height uint16_t RGB565 (little-endian machine).
 * Conversion big-endian faite EN LOCAL (et pas via la fonction du fichier
 * Waveshare) : ce fichier embarque le NOUVEAU driver I2C, incompatible avec
 * l'ANCIEN driver I2C utilisé ici pour le GT911/clavier → sinon abort au boot
 * (check_i2c_driver_conflict, deux pilotes I2C liés dans le même binaire). */
static esp_err_t tdeck_display_flush(void *ctx,
                                     const void *pixels,
                                     uint16_t width,
                                     uint16_t height)
{
    meshpay_hal_lilygo_tdeck_driver_t *driver =
        (meshpay_hal_lilygo_tdeck_driver_t *)ctx;
    if (driver == NULL || !driver->initialized || pixels == NULL ||
        width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Fenêtre colonnes (CASET) : 0..(width-1) */
    const uint16_t x2 = width - 1U;
    const uint8_t caset[4] = {
        0x00, 0x00,
        (uint8_t)(x2 >> 8), (uint8_t)x2,
    };
    /* Fenêtre lignes (RASET) : 0..(height-1) */
    const uint16_t y2 = height - 1U;
    const uint8_t raset[4] = {
        0x00, 0x00,
        (uint8_t)(y2 >> 8), (uint8_t)y2,
    };

    esp_err_t err = send_cmd(driver, ST7789_CMD_CASET);
    if (err == ESP_OK) err = send_data(driver, caset, sizeof(caset));
    if (err == ESP_OK) err = send_cmd(driver, ST7789_CMD_RASET);
    if (err == ESP_OK) err = send_data(driver, raset, sizeof(raset));
    if (err == ESP_OK) err = send_cmd(driver, ST7789_CMD_RAMWR);
    if (err != ESP_OK) return err;

    /* Envoi des pixels par chunks avec conversion RGB565 → big-endian */
    const uint16_t *src = (const uint16_t *)pixels;
    size_t remaining = (size_t)width * (size_t)height;
    /* Buffer local de 1024 octets = 512 pixels max par chunk */
    uint8_t tx_buf[1024];
    while (remaining > 0) {
        const size_t chunk_px = (remaining < (sizeof(tx_buf) / 2U))
                                    ? remaining
                                    : (sizeof(tx_buf) / 2U);
        /* RGB565 machine (little-endian) → big-endian sur le bus, en local. */
        for (size_t i = 0; i < chunk_px; ++i) {
            const uint16_t px = src[i];
            tx_buf[i * 2U]      = (uint8_t)(px >> 8);
            tx_buf[i * 2U + 1U] = (uint8_t)(px & 0xFFU);
        }
        err = send_data(driver, tx_buf, chunk_px * 2U);
        if (err != ESP_OK) {
            return err;
        }
        src       += chunk_px;
        remaining -= chunk_px;
    }
    return ESP_OK;
}

/* Remplit tout l'écran d'une couleur unie rgb565 (utile au boot et pour les tests). */
static esp_err_t fill_screen(meshpay_hal_lilygo_tdeck_driver_t *driver,
                             uint16_t rgb565)
{
    /* Fenêtre colonnes plein écran */
    const uint8_t caset[4] = {
        0x00, 0x00,
        (uint8_t)((MESHPAY_HAL_TDECK_WIDTH  - 1U) >> 8),
        (uint8_t) (MESHPAY_HAL_TDECK_WIDTH  - 1U),
    };
    /* Fenêtre lignes plein écran */
    const uint8_t raset[4] = {
        0x00, 0x00,
        (uint8_t)((MESHPAY_HAL_TDECK_HEIGHT - 1U) >> 8),
        (uint8_t) (MESHPAY_HAL_TDECK_HEIGHT - 1U),
    };

    esp_err_t err = send_cmd(driver, ST7789_CMD_CASET);
    if (err == ESP_OK) err = send_data(driver, caset, sizeof(caset));
    if (err == ESP_OK) err = send_cmd(driver, ST7789_CMD_RASET);
    if (err == ESP_OK) err = send_data(driver, raset, sizeof(raset));
    if (err == ESP_OK) err = send_cmd(driver, ST7789_CMD_RAMWR);
    if (err != ESP_OK) return err;

    /* Préremplit un buffer de 256 octets (128 pixels) avec la couleur big-endian */
    uint8_t tx_buf[256];
    for (size_t i = 0; i < sizeof(tx_buf); i += 2U) {
        tx_buf[i]      = (uint8_t)(rgb565 >> 8);
        tx_buf[i + 1U] = (uint8_t)rgb565;
    }

    /* Envoie la couleur sur l'ensemble des pixels plein écran */
    size_t remaining =
        (size_t)MESHPAY_HAL_TDECK_WIDTH * (size_t)MESHPAY_HAL_TDECK_HEIGHT * 2U;
    while (remaining > 0) {
        const size_t chunk = (remaining < sizeof(tx_buf))
                                 ? remaining
                                 : sizeof(tx_buf);
        err = send_data(driver, tx_buf, chunk);
        if (err != ESP_OK) return err;
        remaining -= chunk;
    }
    return ESP_OK;
}

/* ── Ops display_init / display_flush ────────────────────────────────────── */

/* Fonction d'initialisation de l'écran appelée via meshpay_hal_display_init.
 * Séquence : power on → SPI → rétroéclairage → séquence ST7789 → remplissage bleu. */
static esp_err_t tdeck_display_init(void *ctx)
{
    meshpay_hal_lilygo_tdeck_driver_t *driver =
        (meshpay_hal_lilygo_tdeck_driver_t *)ctx;
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initialisation écran T-Deck ST7789 320×240");

    esp_err_t err = tdeck_power_on();
    if (err == ESP_OK) {
        err = init_spi(driver);
    }
    if (err == ESP_OK) {
        err = init_backlight();
    }
    if (err == ESP_OK) {
        err = run_st7789_init(driver);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "T-Deck display init failed: %s", esp_err_to_name(err));
        (void)meshpay_hal_lilygo_tdeck_driver_deinit(driver);
        return err;
    }

    /* Marque le driver comme initialisé AVANT le fill_screen car tdeck_display_flush
     * vérifie driver->initialized. */
    driver->initialized = true;

    /* Remplissage bleu pur au boot : validation visuelle immédiate au banc.
     * RGB565 0x001F = R=0 G=0 B=31 = bleu pur. */
    err = fill_screen(driver, TDECK_BOOT_FILL_RGB565);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "T-Deck fill bleu boot échoué: %s", esp_err_to_name(err));
        /* Non fatal : l'écran est initialisé, le remplissage couleur est cosmétique */
    }

    /* Init du bus I2C partagé tactile + clavier APRÈS le power-on (KB_POWERON déjà HIGH).
     * Toléré si le bus est déjà installé (cas de démarrage répété ou partage). */
    esp_err_t i2c_err = tdeck_init_i2c();
    if (i2c_err != ESP_OK) {
        /* Non fatal : l'écran fonctionne ; le tactile et le clavier seront absents. */
        ESP_LOGW(TAG, "I2C init échoué (tactile/clavier indisponibles) : %s",
                 esp_err_to_name(i2c_err));
    }

    /* Lecture batterie au boot (validation banc + diagnostic). */
    uint16_t batt_mv = 0;
    if (tdeck_battery_mv(driver, &batt_mv) == ESP_OK) {
        ESP_LOGI(TAG, "T-Deck batterie ~%u mV", (unsigned)batt_mv);
    }

    ESP_LOGI(TAG, "T-Deck display ready %ux%u",
             (unsigned)MESHPAY_HAL_TDECK_WIDTH,
             (unsigned)MESHPAY_HAL_TDECK_HEIGHT);
    return ESP_OK;
}

/* ── Batterie (ADC) ──────────────────────────────────────────────────────── */

#define TDECK_BATT_ADC_CHANNEL ADC_CHANNEL_3    /* GPIO4 = ADC1_CH3 sur ESP32-S3 */
#define TDECK_BATT_ADC_ATTEN   ADC_ATTEN_DB_12
/* Pont diviseur : tension batterie = tension pin × 2.11 (ADC_MULTIPLIER T-Deck). */
#define TDECK_BATT_MULT_NUM    211
#define TDECK_BATT_MULT_DEN    100

/* Initialise l'unité ADC1 + le canal batterie + la calibration (init paresseuse). */
static esp_err_t tdeck_battery_adc_init(meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->adc_ready) {
        return ESP_OK;
    }
    adc_oneshot_unit_handle_t adc = NULL;
    const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &adc);
    if (err != ESP_OK) {
        return err;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = TDECK_BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(adc, TDECK_BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(adc);
        return err;
    }
    /* Calibration : curve fitting si dispo, sinon line fitting, sinon brut. */
    adc_cali_handle_t cali = NULL;
    bool calibrated = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        const adc_cali_curve_fitting_config_t cfg = {
            .unit_id  = ADC_UNIT_1,
            .chan     = TDECK_BATT_ADC_CHANNEL,
            .atten    = TDECK_BATT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        calibrated = (adc_cali_create_scheme_curve_fitting(&cfg, &cali) == ESP_OK);
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        const adc_cali_line_fitting_config_t cfg = {
            .unit_id  = ADC_UNIT_1,
            .atten    = TDECK_BATT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        calibrated = (adc_cali_create_scheme_line_fitting(&cfg, &cali) == ESP_OK);
    }
#endif
    driver->adc_handle      = (void *)adc;
    driver->adc_cali_handle = (void *)cali;
    driver->adc_calibrated  = calibrated;
    driver->adc_ready       = true;
    return ESP_OK;
}

/* Op batterie : tension en mV (moyenne de 4 mesures × pont diviseur ×2.11). */
static esp_err_t tdeck_battery_mv(void *ctx, uint16_t *mv)
{
    meshpay_hal_lilygo_tdeck_driver_t *driver =
        (meshpay_hal_lilygo_tdeck_driver_t *)ctx;
    if (driver == NULL || mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = tdeck_battery_adc_init(driver);
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_unit_handle_t adc = (adc_oneshot_unit_handle_t)driver->adc_handle;
    int raw_sum = 0;
    for (int i = 0; i < 4; ++i) {
        int raw = 0;
        err = adc_oneshot_read(adc, TDECK_BATT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        raw_sum += raw;
    }
    const int raw = raw_sum / 4;
    int pin_mv = 0;
    if (driver->adc_calibrated && driver->adc_cali_handle != NULL) {
        err = adc_cali_raw_to_voltage((adc_cali_handle_t)driver->adc_cali_handle,
                                      raw, &pin_mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        pin_mv = (raw * 3300) / 4095;   /* fallback non calibré */
    }
    uint32_t batt = (uint32_t)pin_mv * TDECK_BATT_MULT_NUM / TDECK_BATT_MULT_DEN;
    if (batt > UINT16_MAX) {
        batt = UINT16_MAX;
    }
    *mv = (uint16_t)batt;
    return ESP_OK;
}

/* Table des ops pour le T-Deck : écran ST7789, tactile GT911, clavier I2C, batterie.
 * Les ops LoRa, ESP-NOW et stockage sont NULL (hors périmètre du driver d'écran). */
static const meshpay_hal_ops_t TDECK_OPS = {
    .display_init  = tdeck_display_init,
    .display_flush = tdeck_display_flush,
    .touch_read    = tdeck_touch_read,
    .keyboard_read = tdeck_keyboard_read,
    .battery_mv    = tdeck_battery_mv,
};

/* ── Driver init / deinit publics ────────────────────────────────────────── */

esp_err_t meshpay_hal_lilygo_tdeck_driver_init(
    meshpay_hal_lilygo_tdeck_driver_t *driver,
    meshpay_hal_t *hal)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return meshpay_hal_init(hal, MESHPAY_BOARD_LILYGO_TDECK, &TDECK_OPS, driver);
}

esp_err_t meshpay_hal_lilygo_tdeck_driver_deinit(
    meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driver->spi_handle != NULL) {
        (void)spi_bus_remove_device((spi_device_handle_t)driver->spi_handle);
    }
    /* Ne pas appeler spi_bus_free ici car le bus SPI2 peut être partagé avec le LoRa */
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#else /* CONFIG_IDF_TARGET_ESP32S3 non défini */

/* Stubs pour les cibles non-S3 (compilation croisée, tests hôte, etc.) */

esp_err_t meshpay_hal_lilygo_tdeck_driver_init(
    meshpay_hal_lilygo_tdeck_driver_t *driver,
    meshpay_hal_t *hal)
{
    (void)driver;
    (void)hal;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t meshpay_hal_lilygo_tdeck_driver_deinit(
    meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return ESP_OK;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */

/* ══════════════════════════════════════════════════════════════════════════
 * Phase 1 — Décodage clavier pur (testable hors banc, toutes cibles)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Le clavier T-Deck (ESP32-C3 @0x55) renvoie 0 quand aucune touche n'est
 * pressée, sinon le code ASCII de la touche. Décodage pur, sans I2C. */
esp_err_t meshpay_hal_tdeck_keyboard_decode(uint8_t raw, bool *has_key, char *ch)
{
    if (has_key == NULL || ch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (raw == 0) {
        *has_key = false;
        *ch = 0;
        return ESP_OK;
    }
    *has_key = true;
    *ch = (char)raw;
    return ESP_OK;
}
