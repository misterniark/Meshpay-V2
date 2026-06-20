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
  - CYD
  - Cheap Yellow Display
  - ESP32-2432S028
---

# Device — CYD ESP32-2432S028

> [!abstract] TL;DR
> Carte « Cheap Yellow Display » à base d'**ESP32** (pas S3), écran **ILI9341 320×240** SPI, tactile **résistif XPT2046**, et radio LoRa **externe Grove Wio-E5** (UART/AT). C'est la seule carte du parc qui n'utilise **pas** de Core1262/SX1262 piloté en SPI. Rôle prévu : device **maître/distributeur**.

## Identité matérielle

| Élément | Valeur |
|---|---|
| Référence carte | CYD ESP32-2432S028 (« Cheap Yellow Display » 2.8") |
| Révision / variante | Variante à **tactile résistif** — combinaison **ILI9341 + XPT2046**, ce qui correspond au SKU communément noté **2432S028R** (« R » = resistive). À distinguer des variantes capacitives (ST7789 + CST816). La révision exacte du PCB n'est pas sérigraphiée de façon fiable : se fier au couple contrôleur écran/tactile. |
| SoC | ESP32 (Xtensa dual-core, **pas** ESP32-S3) |
| Flash | 4 Mo (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`) |
| PSRAM | Non utilisée par le firmware |
| Écran | ILI9341, 320×240, RGB565, dalle native 240×320 utilisée en paysage |
| Tactile | XPT2046, résistif |
| Radio | Module Grove **Wio-E5** (SX1262 + STM32, commandes AT) — externe |
| Cible Kconfig | `CONFIG_MESHPAY_BOARD_CYD_ESP32_2432S028=y` |

## Rôle dans Mesh Pay

Par défaut le CYD est destiné au rôle **maître / distributeur produit**. La réserve de pré-mine reste désactivée tant que le manifeste signé n'est pas implémenté (`CONFIG_MESHPAY_PREMINE_RESERVE_AMOUNT=0`), pour éviter un MINT implicite.

## Câblage — Écran ILI9341 (SPI)

Bus dédié **SPI2_HOST**. Source : `components/device_hal/src/esp32/hal_display_ili9341.c`.

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 13 | Données → écran |
| SCK | 14 | Horloge SPI |
| CS | 15 | Chip-select LCD |
| DC | 2 | Data/Command (0 = commande, 1 = données) |
| RST | — | Pas de broche reset dédiée (`-1`) |
| Rétroéclairage (BL) | 21 | PWM LEDC 5 kHz, 8 bits (0–255) — **logique active-HAUT** (duty 255 = pleine luminosité, initialisé au max) |

- **Type de bus écran** : SPI (SPI2_HOST / « HSPI »). Écran en écriture seule, `MISO` non utilisé côté LCD.
- **Fréquences configurées** (validées stables sur le matériel) : **40 MHz** pour les données pixels, **26 MHz** pour les commandes.
- **Rotation voulue** : paysage 320×240 via `MADCTL = 0x28` (bit MV=1 → échange lignes/colonnes).
- **Ordre des couleurs** : **BGR** (`MADCTL` bit 3 = 1). Format pixel : RGB565 (`COLMOD = 0x55`).
- **Offsets** : aucun — la fenêtre d'adressage couvre toute la VRAM.

## Câblage — Tactile XPT2046 (SPI séparé)

Bus **SPI3_HOST**, distinct du bus écran. Tactile **résistif** (lecture de coordonnées analogiques).

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 32 | |
| MISO | 39 | Input seul côté ESP32 |
| SCK | 25 | |
| CS | 33 | |
| IRQ | 36 | Pénétration tactile (pen-down) |

- **Type de bus tactile** : SPI **dédié** (SPI3_HOST / « VSPI »), distinct du bus écran. Horloge **2 MHz**.
- **Broche IRQ** : **active BAS** (0 = écran touché, 1 = relâché).
- **Calibration** : linéaire, valeurs brutes `200 … 3900` → `0 … (largeur/hauteur − 1)`, saturation aux bornes, moyenne sur **3 échantillons**. Commandes XPT2046 : `0xD0` = X, `0x90` = Y (12 bits, mode différentiel).
- **Axes (swap / inversion)** : mapping **direct** — `raw_x → x`, `raw_y → y`. **Aucun swap, aucune inversion** appliqués dans la HAL. Si l'orientation tactile ne colle pas à l'écran, c'est ici qu'il faut corriger : échanger X/Y, ou inverser une borne `MIN/MAX` pour inverser un axe.

## Câblage — Radio LoRa Wio-E5 (UART/AT)

> [!warning] Pas de Core1262 sur cette carte
> Le CYD ne porte **pas** de module Core1262/SX1262 piloté en SPI. Il utilise un module **Grove Wio-E5** (LoRa-E5, basé SX1262 + MCU STM32WLE5) commandé en **UART via commandes AT**. Driver figé : `CONFIG_MESHPAY_LORA_DRIVER_WIO_E5=y`.

| Signal | GPIO / valeur | Kconfig |
|---|---|---|
| Port UART | UART2 | `CONFIG_MESHPAY_LORA_WIOE5_UART_NUM=2` |
| TX (ESP32 → Wio-E5) | 17 | `CONFIG_MESHPAY_LORA_WIOE5_PIN_TX=17` |
| RX (Wio-E5 → ESP32) | 16 | `CONFIG_MESHPAY_LORA_WIOE5_PIN_RX=16` |
| Alimentation | 3,3 V | Connecteur Grove |

Le code applicatif passe par `hal_lora_create_default()` ; côté Wio-E5 c'est `hal_lora_wio_e5_create()` qui ouvre le port UART. La console série du firmware reste sur **UART0** (pas de conflit : le Wio-E5 est sur UART2).

## Build & flash

```zsh
./scripts/idf.sh -B build set-target esp32
./scripts/idf.sh -B build flash monitor -p /dev/cu.usbmodem<N>
```

- Defaults : `sdkconfig.defaults` + `sdkconfig.defaults.esp32`.
- Cible IDF : `esp32`. **Pas** de flash encryption forcée sur cette carte → `idf.py flash` standard suffit (contrairement au Waveshare S3).
- Secure Boot V2 désactivé par défaut : il exigerait un ESP32 révision ≥ 3.0 (ECO3) et une clé RSA-3072.
- Sauvegarde de config disponible : `sdkconfig.cyd.bak`.

## Pièges & apprentissages des tests

> [!bug] F-HW-016 — Doublon `ILI9341_CMD_MADCTL`
> Un ancien `#define ILI9341_CMD_MADCTL = 0x3A` entrait en collision avec `COLMOD` (lui aussi `0x3A`). Supprimé : la constante correcte de MADCTL est `0x36` (`ILI9341_CMD_MADCTL_R`). Un mauvais MADCTL casse l'orientation/les couleurs de l'écran.

> [!tip] Bus SPI séparés écran/tactile
> Contrairement à beaucoup de cartes, l'écran (SPI2_HOST) et le tactile (SPI3_HOST) sont sur **deux contrôleurs SPI distincts**. Ne pas tenter de les fusionner sur un bus partagé.

> [!info] LoRa = UART, pas SPI
> Toute la logique « pinout SPI Core1262 » (NSS/BUSY/DIO1/RXEN/TXEN) ne s'applique **pas** ici. Le débogage radio du CYD se fait au niveau des commandes AT sur UART2.

## Voir aussi

- [[Device — Waveshare ESP32-S3-Touch-LCD-1.47]]
- [[Device — LILYGO T5 E-Paper S3 Pro 4.7 (H752)]]
- [[14 - Driver LoRa Core1262 (design)]]
- [[02 - Architecture générale]]
