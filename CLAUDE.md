# Claude — Notes projet Mesh Pay (MeshPayV2)

> Pour le matériel, l'ESP-IDF et les pièges de flash, voir **AGENTS.md**.
> Ce fichier-ci complète AGENTS.md avec ce qui est spécifique à mes sessions :
> conventions, workflow, configuration Kconfig, ports série, structure des modules.
>
> **MeshPayV2 = « Reticulum Edition »** : port C clean-room du Reticulum Network Stack
> (composants `rns_*`) + couche métier monnaie / DAG / wallet. Nouveau projet, pas une
> migration directe de `../Mesh Pay`.

---

## Conventions de session

- **Langue** : tout en français (réponses, commentaires, messages de commit).
- **Planifier d'abord, coder ensuite** : pas d'implémentation avant une décomposition explicite.
- **Pas de complaisance** : si je pense que l'utilisateur a tort, je le dis.
- **Erreurs** : ne jamais masquer. Chaque erreur consignée avec message exact + contexte + cause probable + tentative de correction. Pas de `TEST_IGNORE` ni de désactivation de test pour faire passer un build.
- **Tests** : écrire un test unitaire pour toute nouvelle fonction (Unity, déjà intégré). Tests dans `components/<mod>/test/`. Exécuter après chaque tâche significative, pas après chaque micro-modif.
- **Demander** : si quelque chose n'est pas clair, poser la question avant d'agir.

## Configuration applicative (Kconfig)

Carte cible — `CONFIG_MESHPAY_BOARD_*` (`components/device_hal/Kconfig`) :

| Option | Carte |
|---|---|
| `MESHPAY_BOARD_WAVESHARE_S3_TOUCH` | Waveshare ESP32-S3 Touch LCD 1.47 (portefeuille, défaut S3) |
| `MESHPAY_BOARD_LILYGO_T5S3_H752` | LILYGO T5 E-Paper S3 Pro 4.7 H752 (monitor) |
| `MESHPAY_BOARD_CYD` | ESP32 CYD 2432S028 |
| `MESHPAY_BOARD_UNKNOWN` | générique (défaut) |

Backend radio — `CONFIG_MESHPAY_RADIO_*` (`components/device_hal/Kconfig`) :

| Option | Effet |
|---|---|
| `MESHPAY_RADIO_ESPNOW` | ESP-NOW seul (défaut) |
| `MESHPAY_RADIO_LORA_UART` | LoRa via UART raw/framed (CYD) |
| `MESHPAY_RADIO_LORA_CORE1262` | LoRa SX1262 / Core1262 en SPI |
| `MESHPAY_RADIO_ESPNOW_LORA_CORE1262` | ESP-NOW (découverte) + LoRa Core1262 (sync DAG) — profil S3 |
| `MESHPAY_RADIO_DISABLED` | aucune radio |

Modes / options :

| Option | Effet |
|---|---|
| `MESHPAY_DAG_MONITOR_ONLY` | Build « monitor » : court-circuite le wallet dans `app_main`, écoute la DAG, pas de TX applicative (cible H752). |
| `MESHPAY_LILYGO_H752_TPS65185` | Active les écritures PMIC TPS65185 (`0x6B`) sur la révision H752-01. Laisser `n` sur H752 original (évite des écritures aveugles). |

Pins LoRa Core1262 et params radio (fréquence 868.1 MHz, TCXO, puissance TX) sont aussi des options Kconfig `MESHPAY_LORA_C1262_*` / `MESHPAY_LORA_UART_*` / `MESHPAY_ESPNOW_CHANNEL` — défauts par cible dans `sdkconfig.defaults.*` (voir Pinout plus bas).

> ⚠️ **Supprimés en V2** (ne plus référencer) : `MESHPAY_GT911_ONLY_DIAG`,
> `MESHPAY_LORA_DISCOVERY_PING`, et les anciens `MESHPAY_LORA_DRIVER_CORE1262` /
> `MESHPAY_LORA_DRIVER_WIO_E5` — remplacés par les choix `MESHPAY_RADIO_*` ci-dessus.

## Builds parallèles et flash

Le script de banc `./scripts/hardware_smoke.sh` compile chaque profil dans un build dir
isolé avec un `sdkconfig` propre à la cible (n'altère pas le `sdkconfig` racine) :

```zsh
./scripts/hardware_smoke.sh build-s3         # Waveshare S3 (ESP-NOW + LoRa Core1262)
./scripts/hardware_smoke.sh build-s3-secure  # idem + flash encryption (dev) + NVS chiffrée
./scripts/hardware_smoke.sh build-h752       # LilyGo H752 monitor e-paper (radio LoRa Core1262)
./scripts/hardware_smoke.sh build-cyd        # ESP32 CYD (LoRa UART)
```

Build / flash manuel sur un dossier précis via le wrapper :

```zsh
./scripts/idf.sh -B build-h752-secure -p /dev/cu.usbmodem1101 build
```

**Flash obligatoirement chiffré** sur ESP32-S3 (eFuse `FLASH_CRYPT_CNT` déjà brûlée) —
passer par le garde-fou du script :

```zsh
MESHPAY_HW_CONFIRM=flash PORT=/dev/cu.usbmodem1101 ./scripts/hardware_smoke.sh flash-encrypted
```

Un `idf.py flash` normal (plaintext) gaspille un compte de la counter et peut bricker.
`flash-encrypted` refuse de démarrer si `build-s3-secure` n'a pas produit le `sdkconfig`
sécurisé.

## Reset série après flash

ESP32-S3 USB-CDC ne reset pas tout seul. Forcer via DTR/RTS :

```python
import serial, time
p = serial.Serial(port, 115200, timeout=0.3)
p.setRTS(True);  p.setDTR(False); time.sleep(0.1)
p.setDTR(True);  p.setRTS(False); time.sleep(0.05); p.setDTR(False)
```

Python pyserial est dans `~/.espressif/python_env/idf5.4_py3.9_env/bin/python3`.

## Ports série

Sur cette machine macOS, deux devices typiquement branchés simultanément :

- `/dev/cu.usbmodem101` : un device
- `/dev/cu.usbmodem1101` : l'autre

Pour identifier qui est qui : reset DTR/RTS + lire 3 s de logs. Repères de boot :
`… firmware boot ready (schema vN)` puis `reticulum node ready …`.

## Capture parallèle des deux ports

Le script `/tmp/dual_capture.py` (créé en session) capture 30 s les deux ports en
parallèle avec horodatage relatif et préfixe `[WALLET]` / `[H752]`. Pratique pour
observer une transaction LoRa cross-device.

## Pinout

Valeurs par défaut des options Kconfig `MESHPAY_LORA_*`, surchargées par cible dans
`sdkconfig.defaults.*`.

### Wallet (Waveshare ESP32-S3-Touch-LCD-1.47)
LoRa Core1262 SPI host 2 : `NSS=4 MOSI=2 MISO=10 SCK=1 RESET=5 BUSY=6 DIO1=7 RXEN=8 TXEN=9` (AUX_CS=-1). 868.1 MHz, TCXO=1.8 V, TX 14 dBm.

### Monitor (LILYGO T5 E-Paper S3 Pro 4.7 H752)
- LoRa SX1262 SPI host 2 : `NSS=46 MOSI=17 MISO=8 SCK=18 RESET=43 BUSY=44 DIO1=3` — pas de RXEN/TXEN GPIO (RF switch interne / DIO2). `AUX_CS=16` = CS carte SD (tenir haut pendant les transactions LoRa). TCXO=2.4 V.
- I2C tactile : `SDA=6 SCL=5`. Sur ce bus : GT911 `0x5D`, TPS65185 PMIC `0x6B` (FastEPD documente `0x68` mais c'est faux ici), plus deux périphériques internes `0x51` / `0x55`.
- Backlight : `GPIO 40`.
- Panel : ED047TC1 960×540, driver intégré à `components/device_hal/` (`device_hal_lilygo_t5s3_h752.c`, bus parallèle I80 / `esp_lcd`).

## Structure des modules (V2 — Reticulum Edition)

28 composants ESP-IDF, test Unity dans `components/<mod>/test/`.

```
components/
  # --- Stack Reticulum (RNS) ---
  rns_crypto/           Ed25519/X25519 (Monocypher) + SHA/AES/HMAC/PBKDF2 (mbedTLS), RNG injectable
  rns_identity/         Identité 64 o, hash 16 o, signature, secret partagé X25519
  rns_destination/      Noms dotted, hash 16 o, types Single/Plain/Link (helper meshpay.wallet)
  rns_packet/           Pack/unpack wire MTU 500, header type 1/2, contextes Reticulum
  rns_packet_crypto/    Chiffrement DATA Single (X25519 éphémère, AES-256-CBC, HMAC-SHA256)
  rns_announce/         Encode/decode/verify announce, table known_destinations
  rns_transport_core/   Routage, cache anti-rejeu 48 IDs, table de chemins, forward multi-hop
  rns_iface_espnow/     Interface ESP-NOW (fragmentation L2)
  rns_iface_lora/       Interface LoRa (fragmentation 255 o, demi-duplex)
  rns_link_request/     LINKREQUEST + proof LRPROOF
  rns_request_response/ Requêtes ciblées corrélées sur link
  rns_resource/         Transfert batch > MTU (fragments, checksum SHA-256, reassembly)
  rns_node/             Façade haut niveau : init, announce, send, poll, callbacks RX/proof/request
  rns_radio/            Pont rns_node <-> device_hal (fragmentation TX/RX ESP-NOW/LoRa)
  rns_fixtures/         Vecteurs de compat. générés depuis Reticulum Python

  # --- Métier ---
  meshpay_tx/           Transactions TRANSFER/MINT, CBOR compact, signature Ed25519
  dag/                  Fenêtre 250 TX, merge, conflits (from,seq), tips, checkpoint 200
  dag_sync/             SUMMARY / REQUEST / BATCH via Request/Response/Resource
  dag_monitor/          Mode observation DAG (cible H752)
  currency/             Config monnaie, autorités MINT, supply / fee / fonte (demurrage)
  wallet/               Solde, verrou local 30 s, next_seq, politique PIN
  payment_engine/       Création / validation paiement, ACK/proof, feedback UI
  storage/              Persistance NVS chiffrée (identité, alias, PIN hash, seq, checkpoint)

  # --- Plateforme ---
  device_hal/           HAL display/touch/storage/LoRa(UART+Core1262)/ESP-NOW/power, boards CYD/Waveshare/H752
  ui/                   Logique écrans (Setup/PIN/Home/Pay/Receive/History/Network/Locked), portable sans LVGL
  app_main/             Logique d'orchestration FreeRTOS testable (ui_task / reticulum_task / core_task)
  hardware_smoke/       Manifeste des scénarios de banc + garde-fous flash
  project_skeleton/     Métadonnées projet (nom, version de schéma)
main/
  app_main.c            Entry point : DAG monitor (CONFIG_MESHPAY_DAG_MONITOR_ONLY) ou wallet, sélection radio au boot
```

## Documentation projet

Spécifications et guides dans `Doctech V2/` (repo) et l'Obsidian
`…/Projet/Mesh Pay/Doctech V2/`. Références :

- `Doctech V2/Spécifications Mesh Pay - Réseau Reticulum pour ESP32 bare-metal.md` — spec centrale.
- `Doctech V2/preprod.md` — checklist de durcissement pré-production (Priorité 0, sécurité, radio, release).
- `Doctech V2/GUIDE_LORA_ESP32S3_CORE1262.md` — câblage / mise en route LoRa S3.
- `plan.md` (racine) — état d'avancement composant par composant.

## Mémoire persistante

- Mémoire native inter-sessions : `~/.claude/projects/-Users-misterniark-Dropbox-Code-MeshPayV2/memory/` (`MEMORY.md` = index).
- Historique claude-mem (observations passées) : skill `mem-search` ou `get_observations([IDs])`.
