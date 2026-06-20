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
  - H752
  - LILYGO T5 E-Paper S3 Pro
  - T5 E-Paper
  - Moniteur DAG
---

# Device — LILYGO T5 E-Paper S3 Pro 4.7 (H752)

> [!abstract] TL;DR
> Carte **ESP32-S3** LILYGO avec écran **e-paper ED047TC1 960×540** (bus parallèle 8 bits), tactile **GT911** I2C, et radio **SX1262 intégrée** sur SPI partagé avec la carte SD. Rôle dans Mesh Pay : **moniteur DAG** (observe la DAG via LoRa, pas d'UI wallet). Driver e-paper **natif** `lilygo_epd47_h752`.

## Identité matérielle

| Élément | Valeur |
|---|---|
| Référence carte | LILYGO T5 E-Paper S3 Pro 4.7", révision **H752** |
| SoC | ESP32-S3 |
| Flash | 16 Mo |
| PSRAM | 8 Mo, octal SPI, 80 MHz |
| Écran | e-paper **ED047TC1**, 960×540, niveaux de gris, bus parallèle 8 bits |
| PMIC e-paper | TPS65185 (génère les rails ±15 V / Vcom) |
| Tactile | GT911, capacitif, I2C |
| Radio | **SX1262 intégré** (SPI partagé avec SD) |
| Horloge temps réel | PCF85063 (I2C) |
| Console série | USB-Serial-JTAG — **obligatoire**, voir pièges |
| Cible Kconfig | `CONFIG_MESHPAY_BOARD_LILYGO_T5S3_H752=y` |


## Rôle dans Mesh Pay

Build « **moniteur** » : observe la DAG des transactions via LoRa et l'affiche sur l'e-paper. Pas d'UI wallet ni de paiement. Activé par `CONFIG_MESHPAY_DAG_MONITOR_ONLY`. Banc avec `CONFIG_MESHPAY_BENCH_SELF_MASTER=y`.

## Câblage — Écran e-paper ED047TC1 (bus parallèle)

L'ED047TC1 n'est **pas** un écran SPI : il se pilote via un **bus parallèle 8 bits** cadencé (périphérique I2S/LCD de l'ESP32-S3), plus des lignes de contrôle et un registre de configuration série. Source : `components/device_hal/src/esp32s3/lilygo_epd47_h752/ed047tc1.h` (bloc `CONFIG_IDF_TARGET_ESP32S3`).

| Groupe | Signal | GPIO |
|---|---|---|
| Bus données | D0 | 11 |
| | D1 | 12 |
| | D2 | 13 |
| | D3 | 14 |
| | D4 | 21 |
| | D5 | 47 |
| | D6 | 45 |
| | D7 | 38 |
| Contrôle | CKV (clock vertical) | 39 |
| | STH (start horizontal) | 9 |
| | CKH (latch / edge) | 10 |
| Registre config (shift register) | CFG_DATA | 2 |
| | CFG_CLK | 42 |
| | CFG_STR | 1 |
| Rétroéclairage / frontlight | BL_EN | 40 |

- **Type de bus écran** : ce n'est **pas** du SPI — l'ED047TC1 est piloté par un **bus parallèle 8 bits cadencé** via le périphérique **I2S** de l'ESP32-S3 (timing des lignes généré par RMT, APB 80 MHz ÷ 8 → résolution 0,1 µs). Il n'y a donc ni horloge SPI ni `MOSI/MISO/CS/DC` au sens écran SPI.
- Résolution : **960×540**. Framebuffer : `(960 × 540) / 2` octets (**4 bits/pixel**, niveaux de gris).
- **Ordre des couleurs** : sans objet — écran **monochrome / niveaux de gris**. Le firmware convertit le RGB565 de LVGL en gris via `rgb565_to_epd_gray()`.
- **Rotation / offsets** : pas d'offset VRAM. L'orientation logique est gérée applicativement (voir la transformation tactile plus bas).
- **Frontlight** : PWM LEDC (5 kHz, 8 bits) sur GPIO 40, **logique active-HAUT** (initialisé à 0 = éteint).

### PMIC e-paper TPS65185

| Paramètre | Valeur | Note |
|---|---|---|
| Adresse I2C | **`0x6B`** | straps A0/A1 câblés Vcc/Vcc — **PAS `0x68`** (voir pièges) |
| Vcom | -2,0 V | `H752_TPS65185_VCOM_MV = 2000` (valeur absolue, dalle ED047TC1) |

## Câblage — Tactile GT911 (I2C)

Bus I2C partagé `I2C_NUM_0` (mêmes SDA/SCL que le PMIC et la RTC).

| Signal | GPIO | Note |
|---|---|---|
| SDA | 6 | |
| SCL | 5 | |
| INT | 15 | Le GT911 pilote activement la ligne (push-pull) |
| RST | 41 | Sert aussi à sélectionner l'adresse I2C au boot |
| Adresse I2C | `0x5D` | Adresse alternative possible : `0x14` |

- **Bus I2C** : `I2C_NUM_0`, **400 kHz**, pull-ups internes activés.
- **Contrôleur tactile auto-détecté** : la HAL tente d'abord le **GT911** sur `0x5D`, puis sur `0x14`, et **se rabat sur un contrôleur CST (`0x5A`)** si aucun GT911 ne répond — certains exemplaires H752 embarquent un tactile CST et non un GT911.
- **Axes / rotation (swap + inversion)** : la HAL applique `transform_touch_xy()` = rotation 90° → `x = raw_y` et `y = EPD_HEIGHT − raw_x` (échange des axes **+ inversion** de l'axe issu de X), avec saturation aux bornes 960×540.
- **Calibration** : aucune calibration linéaire (tactile capacitif, coordonnées natives).

> [!info] Scan du bus I2C partagé (SDA=6 / SCL=5)
> Un scan I2C renvoie quatre périphériques :
> - `0x51` — RTC **PCF85063**
> - `0x55` — périphérique interne
> - `0x5D` — tactile **GT911**
> - `0x6B` — PMIC e-paper **TPS65185**

## Câblage — Radio LoRa SX1262 (SPI)

SX1262 **intégré** à la carte, bus **SPI3_HOST** (valeur Kconfig `SPI_HOST=2`). Le bus SPI est **partagé avec le lecteur de carte SD**. Pins surchargés dans `sdkconfig.defaults.esp32s3`.

| Signal | GPIO | Kconfig |
|---|---|---|
| SCK | 18 | `CONFIG_MESHPAY_LORA_C1262_PIN_SCK=18` |
| MOSI | 17 | `CONFIG_MESHPAY_LORA_C1262_PIN_MOSI=17` |
| MISO | 8 | `CONFIG_MESHPAY_LORA_C1262_PIN_MISO=8` |
| NSS (CS) | 46 | `CONFIG_MESHPAY_LORA_C1262_PIN_NSS=46` |
| RESET | 43 | `CONFIG_MESHPAY_LORA_C1262_PIN_RESET=43` |
| BUSY | 44 | `CONFIG_MESHPAY_LORA_C1262_PIN_BUSY=44` |
| DIO1 (IRQ) | 3 | `CONFIG_MESHPAY_LORA_C1262_PIN_DIO1=3` |
| RXEN | **-1** | `CONFIG_MESHPAY_LORA_C1262_PIN_RXEN=-1` |
| TXEN | **-1** | `CONFIG_MESHPAY_LORA_C1262_PIN_TXEN=-1` |

> [!warning] Switch RF piloté par DIO2 — pas de RXEN/TXEN
> Contrairement au Core1262 du Waveshare, le SX1262 du H752 pilote son commutateur RF **en interne via DIO2**. Il n'y a **aucune** broche RXEN/TXEN exposée → mettre `RXEN = TXEN = -1` dans la config. Le driver ne tente alors pas de piloter de GPIO de switch.

- Le **lecteur SD** partage SCK/MOSI/MISO (18/17/8) avec son propre CS sur **GPIO 16**.

## Alimentation, boutons & divers

| Élément | GPIO |
|---|---|
| Bouton BOOT | 0 |
| Bouton KEY | 48 |
| Mesure tension batterie (ADC) | 4 |
| RTC interrupt (PCF85063) | 7 |

## Build & flash

```zsh
./scripts/idf.sh -B build-epaper set-target esp32s3
./scripts/idf.sh -B build-epaper -p /dev/cu.usbmodem<N> encrypted-flash
```

- Cible IDF : `esp32s3`. Defaults : `sdkconfig.defaults` + `sdkconfig.defaults.esp32s3`.
- Build dir dédié : `build-epaper`.
- Diagnostic tactile standalone : `CONFIG_MESHPAY_GT911_ONLY_DIAG=y` (build dir `build-gt911`) court-circuite `app_main` pour ne lancer qu'un test I2C du GT911 — **banc uniquement**.

## Pièges & apprentissages des tests

> [!danger] Console OBLIGATOIRE sur USB-Serial-JTAG
> Le SX1262 a son **RESET sur GPIO 43** et son **BUSY sur GPIO 44** — ce sont précisément les broches **UART0** par défaut de l'ESP32-S3. Sans `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, la console se rabattrait sur UART0 et entrerait en **conflit direct** avec le module LoRa. La console doit donc rester sur l'USB-Serial-JTAG (USB-C natif).

> [!bug] PMIC TPS65185 à l'adresse 0x6B (pas 0x68)
> La documentation FastEPD/epdiy mentionne `0x68` pour le TPS65185. **C'est faux sur ce H752** : les straps A0/A1 sont câblés à Vcc, l'adresse réelle est **`0x6B`**. Un scan I2C le confirme. Sans configuration explicite du PMIC, l'e-paper ne sort pas du gris (pas de noir profond).

> [!bug] `epd_clear_area_cycles` n'est pas en microsecondes
> Le paramètre `epd_clear_area_cycles` du driver LILYGO est un **compteur de ticks internes** (`write_row(time * 10)` dans `epd_push_pixels`), **pas** une durée en µs. Le défaut LILYGO est 50. Une valeur mal interprétée comme µs donne un effacement incorrect.

> [!warning] Driver e-paper natif, pas FastEPD/epdiy
> Pour la révision **H752**, on utilise le driver **natif** `components/device_hal/src/esp32s3/lilygo_epd47_h752/` (et non FastEPD/epdiy). FastEPD/epdiy est réservé à la révision **H752-01 PRO** (board v7 + expander PCA9535).

> [!tip] Reset après flash & ports série
> - Reset RTS via esptool **non fiable** sur USB-Serial-JTAG ESP32-S3 → **cold reset physique** requis.
> - Deux devices sont souvent branchés simultanément : `/dev/cu.usbmodem101` et `/dev/cu.usbmodem1101`. Pour identifier le H752 : filtrer les logs sur le tag `hal_lilygo_h752`.

## Voir aussi

- [[Device — CYD ESP32-2432S028]]
- [[Device — Waveshare ESP32-S3-Touch-LCD-1.47]]
- [[17 - Moniteur DAG LoRa e-paper]]
- [[14 - Driver LoRa Core1262 (design)]]
- [[02 - Architecture générale]]
