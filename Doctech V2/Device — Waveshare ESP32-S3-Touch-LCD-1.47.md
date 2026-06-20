---
tags:
  - meshpay
  - meshpay/hardware
  - meshpay/device
  - tech/esp32
Projets:
  - Mesh Pay
Topics:
  - Embarqué
  - Matériel
Date: 2026-05-21
aliases:
  - Waveshare 1.47
  - ESP32-S3-Touch-LCD-1.47
  - Wallet Waveshare
---

# Device — Waveshare ESP32-S3-Touch-LCD-1.47

> [!abstract] TL;DR
> Carte **ESP32-S3** Waveshare avec écran **JD9853 172×320** SPI, tactile **capacitif AXS5106L** I2C, et module LoRa **Core1262 (SX1262)** externe sur le header d'extension P1. C'est le **device wallet** principal du banc. ⚠️ **Flash Encryption brûlée en eFuse** : flasher **uniquement** en `encrypted-flash`.

## Identité matérielle

| Élément | Valeur |
|---|---|
| Référence carte | Waveshare ESP32-S3-Touch-LCD-1.47 |
| Révision / SKU | SKU Waveshare **31202** (référencé en commentaire dans `hal_display_jd9853.c`, Lot E.5). Couple contrôleurs : **JD9853 (écran) + AXS5106L (tactile)**. |
| SoC | ESP32-S3 (Xtensa dual-core) |
| Flash | 16 Mo (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`) |
| PSRAM | 8 Mo, octal SPI, 80 MHz |
| Écran | JD9853, dalle native 172×320, RGB565 — utilisée en paysage **320×172** |
| Tactile | AXS5106L, capacitif, I2C |
| Radio | Module **Waveshare Core1262** (SX1262, SPI) — externe sur header P1 |
| Console série | USB-Serial-JTAG (USB-C natif) — voir pièges |
| MAC connue (device de dev) | `44:1b:f6:86:16:64` |
| Cible Kconfig | `CONFIG_MESHPAY_BOARD_WAVESHARE_ESP32S3_TOUCH_LCD_147=y` |

## Rôle dans Mesh Pay

Device **wallet** : UI de paiement complète (LVGL). Sur le banc sans maître physique CYD, il tourne avec `CONFIG_MESHPAY_BENCH_SELF_MASTER=y` (interdit en build RELEASE par garde-fou compile-time).

## Câblage — Écran JD9853 (SPI)

Bus **SPI2_HOST**. Source : `components/device_hal/src/esp32s3/hal_display_jd9853.c`.

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 39 | Données → écran |
| SCK | 38 | Horloge SPI |
| CS | 21 | Chip-select LCD |
| DC | 45 | Data/Command — **GPIO 45 = strapping pin VDD_SPI** (état ignoré après boot, OK en sortie) |
| RST | 40 | Reset LCD — corrigé au **Lot E.5** (était 47, qui est le RST du *tactile*) |
| Rétroéclairage (BL) | 46 | Actif-**HAUT**, PWM LEDC 5 kHz 8 bits |

- **Type de bus écran** : SPI (SPI2_HOST). Écran en écriture seule (`MISO = -1`).
- **Fréquence configurée** : **40 MHz** (validée stable).
- **Rotation voulue** : paysage 320×172. La séquence d'init pose `MADCTL = 0x00` (portrait natif 172×320) puis l'**override en `0x60`** (MV=1, MX=1 → paysage avec miroir X), orientation attendue par l'UI Mesh Pay.
- **Ordre des couleurs** : **RGB** (`MADCTL` bit 3 BGR = 0). Format pixel : RGB565 (`COLMOD = 0x05`).
- **Offset** : `Y_OFFSET = 34`. La dalle 172×320 occupe une VRAM 240×320 ; la zone visible (axe court) commence à 34. En paysage (`MADCTL 0x60`) l'offset s'applique sur l'axe **Y** (registre `RASET`) ; le `CASET` d'init couvre 34…205.

## Câblage — Tactile AXS5106L (I2C)

| Signal | GPIO | Note |
|---|---|---|
| SDA | 42 | |
| SCL | 41 | |
| RST | 47 | Doit être **pulsé au boot** pour sortir l'AXS5106L de son état initial (Lot E.6) |
| INT | 48 | Interruption pénétration tactile |
| Adresse I2C | `0x63` | Corrigée au Lot E.6 — voir pièges |

- **Bus I2C** : **100 kHz** (Standard mode) — abaissé de 400 kHz → 100 kHz au **Lot E.6**, l'AXS5106L étant peu fiable à 400 kHz. Registre de lecture tactile : `0x01` (lit 14 octets) ; probe optionnel `DEV_ID` : registre `0x08`.
- **Fréquence max stable** : 100 kHz retenu comme valeur sûre ; 400 kHz testé instable.
- **Axes (swap / inversion)** : tactile capacitif → coordonnées quasi natives, **pas de calibration linéaire**. Le panneau renvoie `raw_x ∈ [0,171]` (axe court) et `raw_y ∈ [0,319]` (axe long). La HAL **échange les axes** pour coller au `MADCTL 0x60` : `x = raw_y`, `y = raw_x`. **Aucun miroir / aucune inversion** appliqués.

## Câblage — Radio LoRa Core1262 / SX1262 (SPI)

Module **Waveshare Core1262 (SX1262)** sur le **header d'extension P1**, bus **SPI3_HOST** (valeur Kconfig `SPI_HOST=2` ; l'écran occupe SPI2_HOST). Brochage vérifié sur les schémas officiels Waveshare.

| Signal | GPIO | Kconfig |
|---|---|---|
| SCK | 1 | `CONFIG_MESHPAY_LORA_C1262_PIN_SCK=1` |
| MOSI | 2 | `CONFIG_MESHPAY_LORA_C1262_PIN_MOSI=2` |
| MISO | 10 | `CONFIG_MESHPAY_LORA_C1262_PIN_MISO=10` — **déplacé de IO3 → IO10** (voir pièges) |
| NSS (CS) | 4 | `CONFIG_MESHPAY_LORA_C1262_PIN_NSS=4` |
| RESET | 5 | `CONFIG_MESHPAY_LORA_C1262_PIN_RESET=5` |
| BUSY | 6 | `CONFIG_MESHPAY_LORA_C1262_PIN_BUSY=6` |
| DIO1 (IRQ) | 7 | `CONFIG_MESHPAY_LORA_C1262_PIN_DIO1=7` |
| RXEN | 8 | `CONFIG_MESHPAY_LORA_C1262_PIN_RXEN=8` — switch RF RX |
| TXEN | 9 | `CONFIG_MESHPAY_LORA_C1262_PIN_TXEN=9` — switch RF TX |

> [!danger] Alimentation du module — 3,3 V uniquement
> Alimenter le Core1262 sur la broche **VCC3V3** du header P1 — **jamais VBUS** (5 V grillerait le SX1262).

- **DIO2** laissé non connecté. **DIO3** = TCXO interne, géré par firmware (`sx126x_set_dio3_as_tcxo_ctrl`) — TCXO **1,8 V** validé au smoke test.
- Le Core1262 expose le commutateur RF (SPDT) via **deux broches dédiées RXEN/TXEN** — d'où 9 GPIO et non 7. DIO2 n'est *pas* relié en interne au switch sur ce module (contrairement au SX1262 du H752).
- Les GPIO **IO1–IO11** sont libres et sortis sur le header P1 ; le reste des GPIO est pris par écran / tactile / SD / PSRAM octal / USB.




## Pièges & apprentissages des tests

> [!danger] Flash Encryption brûlée — irréversible
> Le device de dev (`44:1b:f6:86:16:64`) a Flash Encryption activée en eFuse (`SPI_BOOT_CRYPT_CNT=0b001`, clé XTS-AES-128 en BLOCK_KEY0). Le ROM bootloader déchiffre toute lecture flash via cette clé.
> - **Toujours** `idf.py -p … encrypted-flash` (passe `--encrypt` à esptool). **Jamais** `idf.py flash` : un flash en clair → le ROM déchiffre du clair → magic byte invalide → **boot loop** `invalid header: 0x6b55ffb3`.
> - Cause : `CONFIG_SECURE_FLASH_ENC_ENABLED=y` + mode *development* dans `sdkconfig.defaults`. eFuse brûlé au 1er boot. Probable cause de la régression S36 (smoke test Lot E.0 échoué).

> [!bug] MISO déplacé de GPIO 3 → GPIO 10
> GPIO 3 sur ESP32-S3 est un **strapping pin** qui contrôle la source du signal JTAG au boot. Avec la console USB-Serial-JTAG active, brancher MISO sur GPIO 3 rendait l'init SX1262 instable selon l'état haute-impédance du module au boot — et, combiné au bug F-LT-001, crashait le firmware au premier paiement reçu (NULL deref sur `s_lora_hal.send`). Corrigé : **MISO = GPIO 10**.

> [!bug] Lot E.5 / E.6 — corrections tactile & RST
> - **Lot E.5** : le RST du LCD était câblé sur GPIO 47, qui est en réalité le RST du **tactile**. Corrigé : LCD RST = **GPIO 40**.
> - **Lot E.6** : l'adresse I2C de l'AXS5106L était `0x3B` (code original, faux → NACK silencieux). Vraie adresse = **`0x63`** (driver Rust de référence `toto04/axs5106l`). Registre de lecture tactile corrigé de `0x00` → **`0x01`**. Le RST tactile (GPIO 47) doit être **pulsé au boot**.

> [!warning] Strapping pins ESP32-S3 utilisés
> Trois broches de la carte sont des strapping pins : GPIO 45 (DC écran, = VDD_SPI), GPIO 46 (rétroéclairage), GPIO 3 (ex-MISO, abandonné). Leur état au reset est imposé par le boot ROM ; ils ne sont pilotés par l'application qu'**après** le boot. Le rétroéclairage (GPIO 46) a été un point de confusion lors du débogage de l'écran noir (Lots E.3 à E.5).

> [!success] Smoke test Core1262 — 2026-05-15
> Flash `encrypted-flash` OK, driver Core1262 initialisé sur matériel : `Core1262 initialise (868100000 Hz, SF9, 14 dBm)` + `Mode reception active`. TCXO 1,8 V validé. Reste à valider : échange TX/RX entre deux devices.

> [!tip] Reset après flash & console
> - Le reset RTS via esptool **n'est pas fiable** sur USB-Serial-JTAG ESP32-S3 → **cold reset physique** (débrancher/rebrancher) requis. Ouvrir le port (`cat /dev/cu.usbmodem<N>`) peut aussi déclencher un reset via DTR.
> - Console : `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` → logs `ESP_LOG` visibles sur le port USB-C natif.
> - Le numéro de port (`usbmodem101` vs `usbmodem1101`) change selon le port physique du Mac : détecter avec `ls /dev/cu.usbmodem*` à chaque session.

## Voir aussi

- [[Device — CYD ESP32-2432S028]]
- [[Device — LILYGO T5 E-Paper S3 Pro 4.7 (H752)]]
- [[14 - Driver LoRa Core1262 (design)]]
- [[02 - Architecture générale]]
