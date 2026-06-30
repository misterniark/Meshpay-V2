# Plan d'implémentation — Palier 0 : Bring-up T-Deck Plus

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Faire reconnaître la carte fondateur LILYGO T-Deck Plus par `device_hal` et prouver chacun de ses périphériques (écran ST7789, tactile GT911, clavier I2C, ESP-NOW, LoRa, batterie), sans encore câbler le wizard UI.

**Architecture:** Nouvelle carte dans le HAL piloté par table d'ops. Les parties purement logicielles (enum board, op `keyboard_read`, décodage clavier, décodage GT911 brut, Kconfig/sdkconfig) sont faites en **TDD pur** (Phase 1, sans matériel). Les drivers matériels (Phase 2) reprennent la **structure** des drivers Waveshare (SPI LCD) et H752 (GT911 legacy I2C, batterie ADC), avec la séquence ST7789 et les pins T-Deck, validés au banc.

**Tech Stack:** ESP-IDF (C), CMake, Unity (tests cible), legacy I2C driver, `spi_master`, LEDC, `esp_adc`, SX126x driver existant.

**Référence spec :** `Doctech V2/chantier_palier0_bringup_tdeck.md`.

> **État (2026-06-30) : Phase 1 ✅ TERMINÉE et fusionnée sur `main`** (commits
> `9cf99ff..f333abe`). Tâches 1-5 faites en TDD (sous-agents) ; `test_app` et le
> firmware T-Deck compilent. ⚠️ Assertions Unity **non encore exécutées on-device**
> (cible-only S3). **Phase 2 (drivers au banc) = prochaine étape**, nécessite le
> T-Deck branché.

---

## Pinout (rappel, source variant Meshtastic `t-deck` + utilities.h LILYGO)

`KB_POWERON=10` · I2C SDA=18 SCL=8 · ST7789 CS=12 DC=11 MOSI=41 SCK=40 MISO=38 RST=-1 BL=42 (240×320) · GT911 0x5D INT=16 · clavier 0x55 · SX1262 CS=9 RST=17 DIO1=45 **BUSY=13\*** SCK=40 MOSI=41 MISO=38, AUX_CS(SD)=39, TCXO 1.8 V (enum 2), DIO2 RF-switch · batt ADC=4.
\* BUSY=13 à confirmer au banc (divergence LILYGO/Meshtastic).

---

## Structure des fichiers

| Fichier | Action | Responsabilité |
|---|---|---|
| `components/device_hal/include/meshpay/device_hal.h` | modifier | enum board, op `keyboard_read`, wrapper + proto, mock champ + queue, `gt911_decode_raw`, protos T-Deck, defines pins/écran T-Deck |
| `components/device_hal/device_hal.c` | modifier | wrapper `meshpay_hal_keyboard_read` |
| `components/device_hal/device_hal_mock.c` | modifier | mock `keyboard_read` + queue |
| `components/device_hal/device_hal_lilygo_t5s3_h752.c` | modifier | faire passer son decode par `gt911_decode_raw` (garder vert) |
| `components/device_hal/device_hal_lilygo_tdeck.c` | **créer** | driver carte T-Deck (power, écran, tactile, clavier, batterie, ops) |
| `components/device_hal/Kconfig` | modifier | `MESHPAY_BOARD_LILYGO_TDECK` |
| `components/device_hal/CMakeLists.txt` | modifier | ajouter la source |
| `components/device_hal/test/test_device_hal.c` | modifier | tests : keyboard op, keyboard decode, gt911 raw |
| `sdkconfig.defaults.tdeck` | **créer** | profil fondateur (board + radio + pins LoRa) |
| `main/app_main.c` | modifier | `configured_board()`, instance + init driver, chemin smoke |
| `scripts/hardware_smoke.sh` | modifier | `build-tdeck` |
| `components/hardware_smoke/...` | modifier | scénario de banc T-Deck |

---

# PHASE 1 — Échafaudage & logique testable (sans matériel, TDD)

## Task 1 : Enum board + Kconfig + sélection

**Files:**
- Modify: `components/device_hal/include/meshpay/device_hal.h:66-71`
- Modify: `components/device_hal/Kconfig:16-18`
- Modify: `main/app_main.c` (fonction `configured_board`, ~ligne 1701-1712)

- [ ] **Step 1 : Ajouter la valeur d'enum**

Dans `device_hal.h`, l'enum `meshpay_board_t` :
```c
typedef enum {
    MESHPAY_BOARD_UNKNOWN = 0,
    MESHPAY_BOARD_CYD,
    MESHPAY_BOARD_WAVESHARE_S3_TOUCH,
    MESHPAY_BOARD_LILYGO_T5S3_H752,
    MESHPAY_BOARD_LILYGO_TDECK,
} meshpay_board_t;
```

- [ ] **Step 2 : Ajouter l'option Kconfig**

Dans `components/device_hal/Kconfig`, à l'intérieur du `choice MESHPAY_BOARD`, après le bloc H752 :
```
    config MESHPAY_BOARD_LILYGO_TDECK
        bool "LilyGo T-Deck / T-Deck Plus (founder)"
```

- [ ] **Step 3 : Câbler la sélection runtime**

Dans `main/app_main.c`, fonction `configured_board()`, ajouter une branche avant le `#else` :
```c
#elif CONFIG_MESHPAY_BOARD_LILYGO_TDECK
    return MESHPAY_BOARD_LILYGO_TDECK;
```

- [ ] **Step 4 : Vérifier la compilation des tests cible**

Run: `./scripts/idf.sh -C test_app -B build-tests build` (ou la cible de tests du projet)
Expected: compile sans erreur (l'enum est utilisé, aucune régression).

- [ ] **Step 5 : Commit**
```bash
git add components/device_hal/include/meshpay/device_hal.h components/device_hal/Kconfig main/app_main.c
git commit -m "feat(tdeck): enregistre la carte LILYGO T-Deck (enum + Kconfig + sélection)"
```

---

## Task 2 : Op `keyboard_read` (HAL + wrapper + mock + test)

**Files:**
- Modify: `components/device_hal/include/meshpay/device_hal.h:79-94` (ops), `215-238` (mock + protos)
- Modify: `components/device_hal/device_hal.c`
- Modify: `components/device_hal/device_hal_mock.c`
- Test: `components/device_hal/test/test_device_hal.c`

- [ ] **Step 1 : Écrire le test qui échoue**

Dans `test_device_hal.c` :
```c
TEST_CASE("device hal mock keyboard read returns queued ascii", "[device_hal]")
{
    meshpay_hal_t hal;
    meshpay_hal_mock_t mock;
    meshpay_hal_mock_init(&mock, &hal, MESHPAY_BOARD_LILYGO_TDECK);

    uint8_t ch = 0xFF;
    /* Sans touche en attente : 0 (aucune touche), ESP_OK. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8(0, ch);

    /* Une touche mise en file est restituée puis consommée. */
    meshpay_hal_mock_queue_keyboard(&mock, 'A');
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8('A', ch);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_keyboard_read(&hal, &ch));
    TEST_ASSERT_EQUAL_UINT8(0, ch);

    /* Garde-fou argument. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, meshpay_hal_keyboard_read(&hal, NULL));
}
```

- [ ] **Step 2 : Lancer → échec attendu**

Run: tests cible (filtre `[device_hal]`)
Expected: FAIL — `meshpay_hal_keyboard_read` / `meshpay_hal_mock_queue_keyboard` non déclarés.

- [ ] **Step 3 : Déclarer l'op, le wrapper, le mock dans le header**

Dans `device_hal.h`, ajouter le pointeur d'op à la fin de `meshpay_hal_ops_t` (après `battery_status`) :
```c
    esp_err_t (*keyboard_read)(void *ctx, uint8_t *out_ascii);
```
Ajouter le champ mock à `meshpay_hal_mock_t` (après `battery_mv`) :
```c
    uint8_t keyboard_byte;
```
Ajouter les prototypes (près des autres) :
```c
esp_err_t meshpay_hal_keyboard_read(meshpay_hal_t *hal, uint8_t *out_ascii);
void meshpay_hal_mock_queue_keyboard(meshpay_hal_mock_t *mock, uint8_t ascii);
```

- [ ] **Step 4 : Implémenter le wrapper**

Dans `device_hal.c`, après `meshpay_hal_touch_read` (l'op clavier peut être absente → renvoyer 0/`ESP_OK` plutôt qu'erreur si NULL serait moins net ; on garde le contrat CALL_OP comme les autres) :
```c
esp_err_t meshpay_hal_keyboard_read(meshpay_hal_t *hal, uint8_t *out_ascii)
{
    if (out_ascii == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return CALL_OP(hal, keyboard_read, out_ascii);
}
```

- [ ] **Step 5 : Implémenter le mock**

Dans `device_hal_mock.c`, ajouter l'op + l'enregistrer + la file :
```c
static esp_err_t mock_keyboard_read(void *ctx, uint8_t *out_ascii)
{
    if (out_ascii == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_hal_mock_t *mock = (meshpay_hal_mock_t *)ctx;
    *out_ascii = mock->keyboard_byte;   /* 0 = aucune touche */
    mock->keyboard_byte = 0;            /* consommée (one-shot) */
    return ESP_OK;
}
```
Ajouter `.keyboard_read = mock_keyboard_read,` à `MOCK_OPS`. Ajouter :
```c
void meshpay_hal_mock_queue_keyboard(meshpay_hal_mock_t *mock, uint8_t ascii)
{
    if (mock != NULL) {
        mock->keyboard_byte = ascii;
    }
}
```

- [ ] **Step 6 : Lancer → vert**

Run: tests cible (filtre `[device_hal]`)
Expected: PASS (et tous les autres tests device_hal restent verts).

- [ ] **Step 7 : Commit**
```bash
git add components/device_hal/include/meshpay/device_hal.h components/device_hal/device_hal.c components/device_hal/device_hal_mock.c components/device_hal/test/test_device_hal.c
git commit -m "feat(hal): ajoute l'op keyboard_read (+ mock + test)"
```

---

## Task 3 : Décodage GT911 brut réutilisable

**But :** extraire la lecture trame→coords brutes du décodage GT911 pour que H752 *et* T-Deck l'utilisent, chacun avec sa transform.

**Files:**
- Modify: `components/device_hal/include/meshpay/device_hal.h` (proto)
- Modify: `components/device_hal/device_hal_lilygo_t5s3_h752.c:97-115`
- Test: `components/device_hal/test/test_device_hal.c`

- [ ] **Step 1 : Écrire le test qui échoue**

Dans `test_device_hal.c` :
```c
TEST_CASE("gt911 raw decode extracts pressed flag and coordinates", "[device_hal]")
{
    uint8_t frame[MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN] = {0};
    bool pressed = true;
    uint16_t x = 0, y = 0;

    /* status sans bit 0x80 → pas de point. */
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_hal_gt911_decode_raw(frame, sizeof(frame), &pressed, &x, &y));
    TEST_ASSERT_FALSE(pressed);

    /* status valide (0x80 | count=1), point (0x0123, 0x0045). */
    frame[0] = 0x81;
    frame[2] = 0x23; frame[3] = 0x01;
    frame[4] = 0x45; frame[5] = 0x00;
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_hal_gt911_decode_raw(frame, sizeof(frame), &pressed, &x, &y));
    TEST_ASSERT_TRUE(pressed);
    TEST_ASSERT_EQUAL_UINT16(0x0123, x);
    TEST_ASSERT_EQUAL_UINT16(0x0045, y);
}
```

- [ ] **Step 2 : Lancer → échec attendu**

Expected: FAIL — `meshpay_hal_gt911_decode_raw` non déclaré.

- [ ] **Step 3 : Déclarer le proto**

Dans `device_hal.h`, près de `meshpay_hal_lilygo_h752_gt911_decode` :
```c
esp_err_t meshpay_hal_gt911_decode_raw(const uint8_t *frame,
                                       size_t frame_len,
                                       bool *pressed,
                                       uint16_t *raw_x,
                                       uint16_t *raw_y);
```

- [ ] **Step 4 : Implémenter + rebrancher H752 dessus**

Dans `device_hal_lilygo_t5s3_h752.c`, ajouter la fonction brute et faire passer le décodage H752 par elle (comportement identique) :
```c
esp_err_t meshpay_hal_gt911_decode_raw(const uint8_t *frame,
                                       size_t frame_len,
                                       bool *pressed,
                                       uint16_t *raw_x,
                                       uint16_t *raw_y)
{
    if (frame == NULL || pressed == NULL || raw_x == NULL || raw_y == NULL ||
        frame_len < MESHPAY_HAL_LILYGO_H752_GT911_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t status = frame[0];
    if ((status & 0x80U) == 0U || (status & 0x0FU) == 0U) {
        *pressed = false;
        *raw_x = 0;
        *raw_y = 0;
        return ESP_OK;
    }
    *pressed = true;
    *raw_x = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
    *raw_y = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
    return ESP_OK;
}

esp_err_t meshpay_hal_lilygo_h752_gt911_decode(const uint8_t *frame,
                                               size_t frame_len,
                                               meshpay_touch_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool pressed = false;
    uint16_t raw_x = 0, raw_y = 0;
    esp_err_t err =
        meshpay_hal_gt911_decode_raw(frame, frame_len, &pressed, &raw_x, &raw_y);
    if (err != ESP_OK) {
        return err;
    }
    memset(state, 0, sizeof(*state));
    if (!pressed) {
        return ESP_OK;
    }
    return meshpay_hal_lilygo_h752_transform_touch(raw_x, raw_y, state);
}
```

- [ ] **Step 5 : Lancer → vert (dont l'ancien test GT911 H752)**

Run: tests cible (filtre `[device_hal]`)
Expected: PASS — le nouveau test ET `lilygo h752 gt911 frame decodes status and point` (inchangé).

- [ ] **Step 6 : Commit**
```bash
git add components/device_hal/include/meshpay/device_hal.h components/device_hal/device_hal_lilygo_t5s3_h752.c components/device_hal/test/test_device_hal.c
git commit -m "refactor(hal): extrait gt911_decode_raw, réutilisable par T-Deck"
```

---

## Task 4 : Décodage clavier T-Deck (fonction pure + test)

**Files:**
- Modify: `components/device_hal/include/meshpay/device_hal.h` (proto + define)
- Create section in: `components/device_hal/device_hal_lilygo_tdeck.c` (le fichier sera créé Task 6 ; ici on crée d'abord la fonction pure + son test, le fichier peut être créé minimal avec juste cette fonction)
- Test: `components/device_hal/test/test_device_hal.c`

> Note : pour respecter le TDD sans matériel, cette fonction pure peut vivre dès maintenant dans un nouveau `device_hal_lilygo_tdeck.c` minimal (juste l'include + cette fonction), enrichi en Phase 2.

- [ ] **Step 1 : Écrire le test qui échoue**
```c
TEST_CASE("tdeck keyboard decode maps raw byte to key", "[device_hal]")
{
    bool has_key = true;
    char ch = '?';
    /* 0 = aucune touche. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_tdeck_keyboard_decode(0, &has_key, &ch));
    TEST_ASSERT_FALSE(has_key);
    /* ASCII imprimable. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hal_tdeck_keyboard_decode('z', &has_key, &ch));
    TEST_ASSERT_TRUE(has_key);
    TEST_ASSERT_EQUAL_CHAR('z', ch);
    /* Garde-fou. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_hal_tdeck_keyboard_decode('z', NULL, &ch));
}
```

- [ ] **Step 2 : Lancer → échec attendu** (fonction non déclarée).

- [ ] **Step 3 : Déclarer + implémenter**

Header :
```c
esp_err_t meshpay_hal_tdeck_keyboard_decode(uint8_t raw, bool *has_key, char *ch);
```
Impl (dans `device_hal_lilygo_tdeck.c`) :
```c
/* Le clavier T-Deck (ESP32-C3 @0x55) renvoie 0 quand aucune touche n'est
 * pressée, sinon le code ASCII de la touche. Décodage pur, testable hors banc. */
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
```

- [ ] **Step 4 : Ajouter la source au CMake** (sinon le test ne linke pas) — voir Task 5 Step 2 si fait en même temps.

- [ ] **Step 5 : Lancer → vert.**

- [ ] **Step 6 : Commit**
```bash
git add components/device_hal/include/meshpay/device_hal.h components/device_hal/device_hal_lilygo_tdeck.c components/device_hal/test/test_device_hal.c
git commit -m "feat(tdeck): décodage clavier pur + test"
```

---

## Task 5 : Profil de build (sdkconfig.defaults.tdeck) + CMake

**Files:**
- Create: `sdkconfig.defaults.tdeck`
- Modify: `components/device_hal/CMakeLists.txt:1-12`

- [ ] **Step 1 : Ajouter la source au CMake**

Dans `DEVICE_HAL_SRCS`, après `device_hal_waveshare_s3_touch.c` :
```cmake
        "device_hal_lilygo_tdeck.c"
```

- [ ] **Step 2 : Créer `sdkconfig.defaults.tdeck`**

```ini
# LILYGO T-Deck Plus — profil fondateur (création de monnaie).
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# T-Deck : 8 MB PSRAM.
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# CONFIG_MESHPAY_BOARD_UNKNOWN is not set
CONFIG_MESHPAY_BOARD_LILYGO_TDECK=y

# Profil fondateur : ESP-NOW (découverte) + LoRa Core1262 (sync DAG).
# Démarrage en ESP-NOW d'abord (bus SPI écran/LoRa partagé, cf. Task 11).
CONFIG_MESHPAY_RADIO_ESPNOW_LORA_CORE1262=y
CONFIG_MESHPAY_FORCE_ESPNOW_ONLY=y
CONFIG_MESHPAY_ESPNOW_CHANNEL=1

# SX1262 du T-Deck (bus SPI partagé avec l'écran).
CONFIG_MESHPAY_LORA_C1262_SPI_HOST=2
CONFIG_MESHPAY_LORA_C1262_PIN_SCK=40
CONFIG_MESHPAY_LORA_C1262_PIN_MOSI=41
CONFIG_MESHPAY_LORA_C1262_PIN_MISO=38
CONFIG_MESHPAY_LORA_C1262_PIN_NSS=9
CONFIG_MESHPAY_LORA_C1262_PIN_RESET=17
CONFIG_MESHPAY_LORA_C1262_PIN_BUSY=13
CONFIG_MESHPAY_LORA_C1262_PIN_DIO1=45
CONFIG_MESHPAY_LORA_C1262_PIN_RXEN=-1
CONFIG_MESHPAY_LORA_C1262_PIN_TXEN=-1
CONFIG_MESHPAY_LORA_C1262_PIN_AUX_CS=39
CONFIG_MESHPAY_LORA_C1262_FREQUENCY_HZ=868100000
CONFIG_MESHPAY_LORA_C1262_TCXO_CTRL_VOLTAGE=2
CONFIG_MESHPAY_LORA_C1262_CALIBRATE_IMAGE=y
CONFIG_MESHPAY_LORA_C1262_TX_POWER_DBM=14
```

- [ ] **Step 3 : Commit**
```bash
git add sdkconfig.defaults.tdeck components/device_hal/CMakeLists.txt
git commit -m "build(tdeck): profil sdkconfig fondateur + source device_hal"
```

---

# PHASE 2 — Driver matériel T-Deck (au banc)

> Les « tests » de Phase 2 sont des **observations au banc** (logs série, écran),
> pas des tests unitaires — conforme §6.2 de la spec. Chaque tâche : écrire la
> section de code (structure tirée des templates Waveshare/H752), builder, flasher,
> observer. Reset DTR/RTS après flash (cf. CLAUDE.md). Aucune désactivation de test.

## Task 6 : Squelette du driver + power (KB_POWERON)

**Files:** `components/device_hal/device_hal_lilygo_tdeck.c`, `device_hal.h` (defines + driver struct + protos)

- [ ] **Step 1 : Defines pins + dimensions écran (device_hal.h)**
```c
#define MESHPAY_HAL_TDECK_WIDTH 320
#define MESHPAY_HAL_TDECK_HEIGHT 240
#define MESHPAY_HAL_TDECK_KEYBOARD_ADDR 0x55
#define MESHPAY_HAL_TDECK_TOUCH_ADDR 0x5D
```

- [ ] **Step 2 : Driver struct (device_hal.h)**
```c
typedef struct {
    void *spi_handle;       /* spi_device_handle_t écran ST7789 */
    void *adc_handle;
    void *adc_cali_handle;
    uint8_t adc_cali_scheme;
    bool spi_bus_owned;     /* ce driver a-t-il initialisé le bus SPI ? */
    bool initialized;
    bool touch_available;
    bool adc_ready;
    bool adc_calibrated;
} meshpay_hal_lilygo_tdeck_driver_t;

esp_err_t meshpay_hal_lilygo_tdeck_driver_init(
    meshpay_hal_lilygo_tdeck_driver_t *driver, meshpay_hal_t *hal);
esp_err_t meshpay_hal_lilygo_tdeck_driver_deinit(
    meshpay_hal_lilygo_tdeck_driver_t *driver);
```

- [ ] **Step 3 : Pins privés + power dans le .c**
```c
#define TDECK_PIN_POWERON 10
#define TDECK_PIN_SDA 18
#define TDECK_PIN_SCL 8
#define TDECK_PIN_LCD_CS 12
#define TDECK_PIN_LCD_DC 11
#define TDECK_PIN_LCD_MOSI 41
#define TDECK_PIN_LCD_SCK 40
#define TDECK_PIN_LCD_MISO 38
#define TDECK_PIN_LCD_BL 42
#define TDECK_PIN_TOUCH_INT 16
#define TDECK_PIN_SD_CS 39       /* AUX_CS : tenir HAUT */

/* Le rail périphérique du T-Deck (clavier, tactile, écran) n'est alimenté
 * que si KB_POWERON est HAUT. À faire AVANT toute autre init (piège n°1). */
static esp_err_t tdeck_power_on(void)
{
    gpio_set_direction(TDECK_PIN_POWERON, GPIO_MODE_OUTPUT);
    gpio_set_level(TDECK_PIN_POWERON, 1);
    /* CS partagés inactifs hauts avant tout accès bus. */
    gpio_set_direction(TDECK_PIN_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(TDECK_PIN_SD_CS, 1);
    gpio_set_direction(TDECK_PIN_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(TDECK_PIN_LCD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(100));   /* laisser le C3 clavier booter */
    return ESP_OK;
}
```

- [ ] **Step 4 : driver_init / deinit + ops minimale**
```c
static const meshpay_hal_ops_t TDECK_OPS = {
    .display_init = tdeck_display_init,   /* Task 7 */
    .display_flush = tdeck_display_flush, /* Task 7 */
    .touch_read = tdeck_touch_read,       /* Task 8 */
    .keyboard_read = tdeck_keyboard_read, /* Task 9 */
    .battery_mv = tdeck_battery_mv,       /* Task 10 */
};

esp_err_t meshpay_hal_lilygo_tdeck_driver_init(
    meshpay_hal_lilygo_tdeck_driver_t *driver, meshpay_hal_t *hal)
{
    if (driver == NULL || hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));
    return meshpay_hal_init(hal, MESHPAY_BOARD_LILYGO_TDECK, &TDECK_OPS, driver);
}
```

- [ ] **Step 5 : Build profil tdeck** — `./scripts/hardware_smoke.sh build-tdeck` (créé Task 13) ou build manuel. Expected: compile.

- [ ] **Step 6 : Commit**
```bash
git commit -am "feat(tdeck): squelette driver + séquence power-on KB_POWERON"
```

## Task 7 : Écran ST7789 (SPI partagé-aware, init, flush, backlight)

**Structure tirée verbatim de `device_hal_waveshare_s3_touch.c`** (helpers `send_cmd`/`send_data`, table d'init, `display_flush` CASET/RASET, backlight LEDC). **Différences T-Deck :** contrôleur **ST7789** (pas JD9853) → table d'init ST7789 ; pins T-Deck ; fenêtre 320×240 ; **le bus SPI est partagé avec LoRa** → l'init du bus doit tolérer un bus déjà initialisé (`ESP_ERR_INVALID_STATE` = OK, on ajoute seulement le device).

- [ ] **Step 1 : Init SPI partagé-aware**
```c
static esp_err_t tdeck_init_spi(meshpay_hal_lilygo_tdeck_driver_t *driver)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = TDECK_PIN_LCD_MOSI,
        .miso_io_num = TDECK_PIN_LCD_MISO,
        .sclk_io_num = TDECK_PIN_LCD_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        driver->spi_bus_owned = true;   /* l'écran possède le bus */
    } else if (err != ESP_ERR_INVALID_STATE) {
        return err;                     /* déjà init par LoRa = OK */
    }
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TDECK_PIN_LCD_CS,
        .queue_size = 4,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    return spi_bus_add_device(SPI2_HOST, &dev,
                              (spi_device_handle_t *)&driver->spi_handle);
}
```
> ⚠️ Coordination bus partagé : si LoRa init le bus en premier (Task 11), l'écran réutilise. L'ordre d'init dans `app_main.c` (Task 12) doit garantir qu'un seul `spi_bus_initialize` réussit. Détail à régler au banc.

- [ ] **Step 2 : Helpers `send_cmd` / `send_data`** — copier ceux du Waveshare (DC=`TDECK_PIN_LCD_DC`), inchangés sinon.

- [ ] **Step 3 : Table d'init ST7789** — séquence standard ST7789 (SWRESET 0x01 +120ms, SLPOUT 0x11 +120ms, COLMOD 0x3A=0x55 (RGB565), MADCTL 0x36 (orientation paysage, à régler au banc), INVON 0x21, NORON 0x13, DISPON 0x29 +10ms). Format de table identique au Waveshare (`cmd, count|delayflag, data...`). Orientation/offset = **à régler visuellement au banc**.

- [ ] **Step 4 : `tdeck_display_flush`** — identique au Waveshare mais fenêtre 320×240 sans offset panneau (ou offset ST7789 à confirmer). Réutiliser `meshpay_hal_waveshare_s3_rgb565_to_be` (conversion RGB565→big-endian, agnostique).

- [ ] **Step 5 : Backlight** — LEDC sur `TDECK_PIN_LCD_BL` (copier `init_backlight` Waveshare, GPIO=42).

- [ ] **Step 6 : Banc** — flasher, observer un écran rempli / motif de test. Expected (log) : `T-Deck display ready 320x240`.

- [ ] **Step 7 : Commit**
```bash
git commit -am "feat(tdeck): écran ST7789 SPI (init partagé-aware, flush, backlight)"
```

## Task 8 : Tactile GT911 (transport legacy I2C + transform T-Deck)

**Structure tirée de `device_hal_lilygo_t5s3_h752.c`** (`init_i2c_touch`, `i2c_read_reg16`, `h752_touch_read`). Différences : pins SDA=18/SCL=8 ; pas de transform e-paper → **transform identité (ou orientation simple) pour 320×240**.

- [ ] **Step 1 : Init I2C (legacy) partagé clavier+tactile**
```c
static esp_err_t tdeck_init_i2c(void)
{
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TDECK_PIN_SDA, .scl_io_num = TDECK_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    return (err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}
```

- [ ] **Step 2 : `tdeck_touch_read`** — lire 9 octets via `i2c_master_write_read_device(I2C_NUM_0, 0x5D, reg16, 2, frame, 9, ...)` au registre status GT911 (0x814E), appeler `meshpay_hal_gt911_decode_raw`, puis appliquer une transform T-Deck (identité ou orientation), puis clear status. Copier la structure de `h752_touch_read` (branche GT911).

- [ ] **Step 3 : Banc** — appuyer sur l'écran, observer des coordonnées plausibles en log. Expected : `T-Deck touch x=.. y=..`.

- [ ] **Step 4 : Commit**
```bash
git commit -am "feat(tdeck): tactile GT911 (transport I2C + decode_raw)"
```

## Task 9 : Clavier I2C (transport → op keyboard_read)

- [ ] **Step 1 : `tdeck_keyboard_read`**
```c
static esp_err_t tdeck_keyboard_read(void *ctx, uint8_t *out_ascii)
{
    (void)ctx;
    if (out_ascii == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t raw = 0;
    esp_err_t err = i2c_master_read_from_device(
        I2C_NUM_0, MESHPAY_HAL_TDECK_KEYBOARD_ADDR, &raw, 1, pdMS_TO_TICKS(20));
    if (err != ESP_OK) { *out_ascii = 0; return ESP_OK; } /* pas de touche */
    bool has_key = false; char ch = 0;
    (void)meshpay_hal_tdeck_keyboard_decode(raw, &has_key, &ch);
    *out_ascii = has_key ? (uint8_t)ch : 0;
    return ESP_OK;
}
```

- [ ] **Step 2 : Banc** — appuyer sur des touches, observer l'ASCII en log. Expected : `T-Deck key='A'`.

- [ ] **Step 3 : Commit**
```bash
git commit -am "feat(tdeck): lecture clavier I2C 0x55 -> op keyboard_read"
```

## Task 10 : Batterie (ADC channel 4)

**Structure tirée de `h752_battery_adc_init` / `h752_battery_adc_mv`** (esp_adc oneshot + calibration). Différence : `ADC_CHANNEL_4` (GPIO4), multiplicateur 2.11.

- [ ] **Step 1 : `tdeck_battery_mv`** — copier la structure H752, `ADC_CHANNEL_4`, atten 12 dB, ×2.11 (au lieu de ÷2).
- [ ] **Step 2 : Banc** — observer une tension batterie plausible (3.0–4.2 V). Expected : `T-Deck batt ~3.9V`.
- [ ] **Step 3 : Commit** `git commit -am "feat(tdeck): lecture batterie ADC"`

## Task 11 : LoRa SX1262 sur bus SPI partagé (RISQUE)

**Pins via Kconfig (Task 5).** Le vrai travail = **partage du bus SPI avec l'écran**.

- [ ] **Step 1** : Désactiver `CONFIG_MESHPAY_FORCE_ESPNOW_ONLY` dans un build de test, vérifier l'ordre d'init (écran puis LoRa, ou inverse) pour qu'**un seul `spi_bus_initialize(SPI2_HOST)` réussisse** et que l'autre réutilise le bus. Vérifier dans `device_hal_lora_core1262.c` comment il acquiert le bus (init dédié vs add_device) et l'adapter pour tolérer un bus déjà initialisé.
- [ ] **Step 2** : Vérifier l'AUX_CS=39 (SD) tenu HAUT pendant LoRa (déjà géré par le driver core1262 via Kconfig) + que le CS écran (12) ne perturbe pas (géré par le SPI driver IDF si l'écran est un device du même bus).
- [ ] **Step 3 : Banc** — un TX LoRa + un RX entre deux cartes (ou announce). Expected : trame reçue.
- [ ] **Step 4 : Commit** `git commit -am "feat(tdeck): LoRa SX1262 sur bus SPI partagé écran"`

> Si le partage de bus s'avère lourd, **le Palier 0 reste validable en ESP-NOW-only** (FORCE_ESPNOW_ONLY=y) comme le wallet ; LoRa-sur-bus-partagé devient un sous-chantier suivi.

## Task 12 : Câblage app_main (instance, init, smoke)

**Files:** `main/app_main.c`

- [ ] **Step 1** : Ajouter le bloc statique board (près des lignes 131-138) :
```c
#if CONFIG_MESHPAY_BOARD_LILYGO_TDECK
static meshpay_hal_t s_display_hal;
static meshpay_hal_lilygo_tdeck_driver_t s_tdeck_display_driver;
static TaskHandle_t s_tdeck_touch_task;
#endif
```
- [ ] **Step 2** : Là où le driver display de chaque board est initialisé (chercher `meshpay_hal_waveshare_s3_touch_driver_init` / `meshpay_hal_lilygo_t5s3_h752_driver_init`), ajouter la branche T-Deck appelant `meshpay_hal_lilygo_tdeck_driver_init(&s_tdeck_display_driver, &s_display_hal)` + `meshpay_hal_display_init`.
- [ ] **Step 3** : Chemin smoke minimal : à `tdeck_power_on` puis init, logguer touch/keyboard/battery périodiquement (réutiliser une tâche de poll comme la touch task Waveshare). PAS de rendu UI complet (Palier D).
- [ ] **Step 4 : Banc** — boot complet, log `firmware boot ready` + périphériques.
- [ ] **Step 5 : Commit** `git commit -am "feat(tdeck): câblage app_main (board, init driver, smoke)"`

## Task 13 : Scénario hardware_smoke + script

**Files:** `scripts/hardware_smoke.sh`, `components/hardware_smoke/...`

- [ ] **Step 1** : Ajouter une fonction `build-tdeck` dans `scripts/hardware_smoke.sh` (copier `build-s3`, build dir isolé `build-tdeck`, `sdkconfig.defaults.tdeck`).
- [ ] **Step 2** : Ajouter le scénario T-Deck au manifeste `hardware_smoke` (boot + écran + tactile + clavier + ESP-NOW [+ LoRa]).
- [ ] **Step 3 : Commit** `git commit -am "test(tdeck): scénario hardware_smoke + build-tdeck"`

## Task 14 : Validation au banc (critères d'acceptation)

- [ ] La carte boote (`firmware boot ready`), `KB_POWERON` HAUT.
- [ ] Écran : motif de test affiché (orientation correcte).
- [ ] Tactile : appui → coordonnées plausibles en log.
- [ ] Clavier : touches → ASCII correct en log.
- [ ] ESP-NOW : announce échangé avec un pair.
- [ ] LoRa : TX/RX OK **ou** explicitement reporté (ESP-NOW-only documenté).
- [ ] Tests unitaires Phase 1 verts ; aucun test désactivé.

---

## Auto-revue (writing-plans)

- **Couverture spec** : §2 pinout → Task 5/6 ; §3 intégration HAL → Tasks 1,2,6-12 ; §4 entrée (tactile nav + clavier saisie) → Tasks 2,3,4,8,9 ; §5 réutilisation → Tasks 3,7,8,10 ; §6 tests → Tasks 2,3,4 (unit) + 14 (banc) ; §7 risques → Tasks 6 (power), 7/11 (bus partagé), 8 (GT911), Task 5 (TCXO/BUSY).
- **Placeholders** : les séquences ST7789 (Task 7 Step 3) et transforms (Task 8 Step 2) sont des **tâches de banc** explicites (orientation/offsets à régler visuellement), pas des placeholders logiciels — toute la logique testable (Phase 1) a son code complet.
- **Cohérence des noms** : `meshpay_hal_keyboard_read`, `meshpay_hal_mock_queue_keyboard`, `meshpay_hal_gt911_decode_raw`, `meshpay_hal_tdeck_keyboard_decode`, `meshpay_hal_lilygo_tdeck_driver_init`, ops `tdeck_*` — cohérents entre tâches.

## Risques résiduels (à lever au banc)
1. Bus SPI partagé écran/LoRa (Task 7/11) — le point le plus incertain.
2. BUSY LoRa = 13 (divergence sources).
3. Séquence/orientation ST7789 (Task 7).
4. Firmware factory du C3 clavier présent.
