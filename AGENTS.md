# Mesh Pay — Agent Notes (MeshPayV2)

> **Fichier reconstruit le 2026-06-20** : l'original avait été écrasé par le plugin
> claude-mem (le fichier ne contenait plus qu'un bloc `<claude-mem-context>`).
> Reconstruit à partir de l'`AGENTS.md` de l'ancien projet `../Mesh Pay`, **adapté à
> l'architecture V2 « Reticulum Edition »** (drivers et composants vérifiés dans le code
> au 3 juin 2026). Compléter / corriger si des détails de l'original manquent.
> Voir `CLAUDE.md` pour les conventions, pinouts, ports série et structure des modules.

## ESP-IDF

Toujours utiliser **ESP-IDF 5.4.3** pour ce projet :

```zsh
source /Users/misterniark/.espressif/v5.4.3/esp-idf/export.sh >/dev/null
idf.py build
idf.py -C test_app build
```

Le wrapper local fait la même chose et doit être préféré :

```zsh
./scripts/idf.sh build
./scripts/idf.sh -C test_app build
```

Ne **pas** utiliser ce chemin pour Mesh Pay :

```zsh
/Users/misterniark/Library/CloudStorage/Dropbox/Code/esp/esp-idf/export.sh
```

Cette installation pointe vers ESP-IDF 5.5.2 et son environnement Python est cassé /
manquant sur cette machine. Le projet a été validé avec ESP-IDF 5.4.3.

## Matériels connus (cibles V2)

Carte sélectionnée par `CONFIG_MESHPAY_BOARD_*`, backend radio par
`CONFIG_MESHPAY_RADIO_*` (voir `components/device_hal/Kconfig`).

- **CYD / ESP32-2432S028** (`MESHPAY_BOARD_CYD`) : ESP32, ILI9341 + XPT2046.
  Radio LoRa en **UART** (`MESHPAY_RADIO_LORA_UART`, module Wio-E5 sur le banc CYD).
  Pas de PSRAM.
- **Waveshare ESP32-S3-Touch-LCD-1.47** (`MESHPAY_BOARD_WAVESHARE_S3_TOUCH`) :
  ESP32-S3, JD9853 (SPI) + AXS5106L (I2C), Core1262/SX1262 en **SPI**.
  Cible **portefeuille** par défaut en ESP32-S3 ; backend
  `MESHPAY_RADIO_ESPNOW_LORA_CORE1262` (découverte ESP-NOW + sync DAG LoRa).
- **LILYGO T5 E-Paper S3 Pro 4.7 (H752)** (`MESHPAY_BOARD_LILYGO_T5S3_H752`) :
  ESP32-S3, ED047TC1 960×540 via bus parallèle I80 / `esp_lcd` (driver intégré à
  `device_hal`, **pas** de composant `lilygo_epd47_h752` séparé en V2), tactile
  GT911 `0x5D` (fallback CST `0x5A`), SX1262 intégré. Cible **DAG monitor** lecture
  seule (`MESHPAY_DAG_MONITOR_ONLY=y`, `MESHPAY_RADIO_LORA_CORE1262`). PSRAM octale
  8 Mo, flash 16 Mo. Option `MESHPAY_LILYGO_H752_TPS65185` pour la révision H752-01
  (écritures PMIC `0x6B`).

> La variante H752-01 PRO via epdiy / PCA9535 de l'ancien projet n'est pas portée en
> V2 : le driver V2 passe par `esp_lcd` I80, pas par `managed_components/vroland__epdiy`.

## Points de vigilance

- **Flash encryption** déjà activée en mode **DEVELOPMENT** sur ESP32-S3. Éviter les
  flashs plaintext inutiles : chaque flash plaintext consomme un compte de la
  `FLASH_CRYPT_CNT` et peut bricker. Toujours passer par le flash chiffré
  (`CLAUDE.md` → `hardware_smoke.sh flash-encrypted`).
- Si un flash / monitor semble bloquer, vérifier l'état physique de **BOOT / GPIO0**
  et faire un **power cycle** avant de conclure à une panne firmware (galère
  récurrente sur H752 e-paper : boot loops, hang sur `epd_clear`, ROM download mode).
- Drivers LoRa V2 : `components/device_hal/device_hal_lora_core1262.c`
  (+ `sx126x_hal.c`) pour le SX1262 SPI ; LoRa UART générique pour CYD.
- Driver display / touch H752 : `components/device_hal/device_hal_lilygo_t5s3_h752.c`.
- Le flash chiffré de banc passe par `./scripts/hardware_smoke.sh flash-encrypted`
  (protégé par `MESHPAY_HW_CONFIRM=flash`), jamais un `idf.py flash` plaintext.
