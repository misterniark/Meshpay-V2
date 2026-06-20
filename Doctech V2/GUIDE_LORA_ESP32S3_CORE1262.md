# Guide de mise en route LoRa Core1262 / SX1262 sur Waveshare ESP32-S3-Touch-LCD-1.47

Ce document décrit la procédure complète pour faire fonctionner une puce LoRa Waveshare Core1262 / SX1262 soudée sur une carte Waveshare ESP32-S3-Touch-LCD-1.47, SKU 31202.

Le firmware de ce dépôt sert à tester la communication LoRa entre plusieurs cartes. Chaque carte écoute en continu, envoie un paquet `LORA_TEST` toutes les 5 secondes, puis affiche les paquets reçus sur l'USB Serial/JTAG.

Il affiche aussi un grand numéro sur le LCD intégré pour reconnaître immédiatement quelle carte fonctionne ou dysfonctionne pendant les essais.

## Matériel cible

- Carte: Waveshare ESP32-S3-Touch-LCD-1.47, SKU 31202.
- Radio: Waveshare Core1262 / SX1262.
- Console: USB Serial/JTAG intégré de l'ESP32-S3.
- ESP-IDF utilisé ici: `v5.4.3`.

## Câblage validé

| ESP32-S3 | Core1262 / SX1262 | Rôle |
| --- | --- | --- |
| GPIO 1 | SCK | Horloge SPI |
| GPIO 2 | MOSI | Données ESP32-S3 vers SX1262 |
| GPIO 10 | MISO | Données SX1262 vers ESP32-S3 |
| GPIO 4 | NSS / CS | Chip Select SPI |
| GPIO 5 | NRST / RESET | Reset SX1262 |
| GPIO 6 | BUSY | Etat radio, HIGH = occupé |
| GPIO 7 | DIO1 | IRQ `TX_DONE`, `RX_DONE`, erreurs |
| GPIO 8 | RXEN | Active le chemin RF RX |
| GPIO 9 | TXEN | Active le chemin RF TX |
| 3V3 P1 | VCC | Alimentation 3.3 V |
| GND P1 | GND | Masse commune |

Avant de chercher un bug logiciel, vérifier à l'ohmmètre:

- continuité de `MISO/GPIO10`, `MOSI/GPIO2`, `SCK/GPIO1`, `NSS/GPIO4`;
- continuité de `RESET/GPIO5`, `BUSY/GPIO6`, `DIO1/GPIO7`;
- absence de court-circuit entre deux GPIO voisins;
- masse commune entre ESP32-S3 et module Core1262;
- alimentation 3.3 V stable pendant l'émission.

## Point critique: Flash Encryption

Les cartes de test ont Flash Encryption déjà brûlée en eFuse.

Ne jamais utiliser:

```bash
idf.py flash
```

Utiliser uniquement:

```bash
idf.py -p <port> encrypted-flash
```

Le dépôt contient un script qui force ce chemin:

```bash
./tools/flash_all_encrypted.sh /dev/cu.usbmodem11101 /dev/cu.usbmodem11201 /dev/cu.usbmodem11401
```

La configuration contient aussi:

```text
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y
CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=y
# CONFIG_NVS_ENCRYPTION is not set
```

`CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=y` évite de brûler accidentellement une carte non chiffrée. `NVS_ENCRYPTION` est désactivé, car ce test LoRa n'a pas besoin de NVS chiffrée et cela évite l'erreur de partition `nvs_keys` manquante.

## Paramètres radio qui fonctionnent

| Paramètre | Valeur |
| --- | --- |
| Fréquence | `868100000` Hz |
| Modulation | LoRa |
| Spreading factor | SF7 |
| Bande passante | 125 kHz |
| Coding rate | 4/5 |
| Sync word | `0x1424`, LoRa privé |
| Puissance TX | 14 dBm |
| SPI | 1 MHz |
| TCXO DIO3 | enum `4`, soit 2.4 V |
| DIO2 RF switch | désactivé, car RXEN/TXEN externes sont pilotés par GPIO |

Deux réglages ont été importants pendant les tests:

- `CONFIG_LORA_TEST_TCXO_CTRL_VOLTAGE=4`: avec 1.8 V, la radio remontait `TCXO/XOSC start error`.
- `CONFIG_LORA_TEST_SPI_CLOCK_HZ=1000000`: 1 MHz rend le bus plus tolérant avec des soudures filaires ou longues.

## Préparer l'environnement

Depuis le dossier du projet:

```bash
cd "/Users/misterniark/Documents/test puce lora"
source ~/.espressif/v5.4.3/esp-idf/export.sh
```

Détecter les ports USB:

```bash
ls /dev/cu.usbmodem*
```

Ports observés pendant le test:

```text
/dev/cu.usbmodem11101  node 8611E4
/dev/cu.usbmodem11201  node 8613C8
/dev/cu.usbmodem11401  node 861464
```

## Lire l'écran LCD

Le firmware identifie automatiquement les trois cartes connues avec les trois derniers octets de leur adresse MAC:

| Port USB observé | Node MAC | Grand numéro affiché |
| --- | --- | --- |
| `/dev/cu.usbmodem11101` | `8611E4` | `1` |
| `/dev/cu.usbmodem11201` | `8613C8` | `2` |
| `/dev/cu.usbmodem11401` | `861464` | `3` |

L'écran affiche:

- `NODE 1`, `NODE 2` ou `NODE 3` en haut;
- un très grand chiffre au centre;
- l'état radio: `BOOT`, `READY`, `RX`, `TX` ou `ERR`;
- les compteurs `T`, `R`, `E` pour TX, RX et erreurs.

Interprétation rapide:

- fond vert/bleu: la carte démarre, reçoit ou émet;
- fond rouge `ERR`: la radio SX1262 a remonté une erreur, utile pour repérer la carte à inspecter.

Pour changer ou forcer le numéro affiché:

```bash
idf.py menuconfig
```

Puis:

```text
Waveshare S3 Core1262 LoRa test
  Node number override shown on LCD
```

Laisser `0` pour l'auto-détection des trois cartes connues.

## Diagnostic GPIO/SPI au démarrage

`CONFIG_LORA_TEST_DIAGNOSTICS_ENABLED=y` active un diagnostic court avant le test LoRa normal. Il ne remplace pas le moniteur série ni le multimètre, mais il permet de pointer plus précisément le groupe de GPIO suspect.

Tests effectués:

- toggles et relecture des sorties `SCK/GPIO1`, `MOSI/GPIO2`, `NSS/GPIO4`, `RESET/GPIO5`, `RXEN/GPIO8`, `TXEN/GPIO9`;
- test pull-up/pull-down de `MISO/GPIO10` lorsque `NSS` est haut;
- reset SX1262 puis mesure du temps avant `BUSY/GPIO6 = 0`;
- lectures répétées `GetStatus`;
- écritures/relectures répétées du registre sync word `0x0740`;
- activation TCXO DIO3 puis lecture `DeviceErrors`;
- vérification que `DIO1/GPIO7` n'est pas bloqué haut quand l'IRQ SX1262 est claire.

Les résultats détaillés sont dans les logs:

```text
DIAG_GPIO_OUTPUT ...
DIAG_MISO_FLOAT ...
DIAG_RESET_BUSY ...
DIAG_SPI_STATUS ...
DIAG_SYNC_WORD ...
DIAG_TCXO ...
DIAG_DIO1_IDLE ...
DIAG_SUMMARY ...
```

Lecture rapide des verdicts:

| Verdict écran | Interprétation |
| --- | --- |
| `DIAG OK` | Aucun défaut détecté pendant le diagnostic court |
| `SUS NSS` | `NSS/GPIO4` ne suit pas correctement les niveaux |
| `SUS SCK` | `SCK/GPIO1` bloqué ou court-circuité |
| `SUS MOSI` | écriture SPI instable: suspecter `MOSI/GPIO2`, `SCK/GPIO1`, `NSS/GPIO4` ou `MISO/GPIO10` |
| `SUS MISO` | lectures SPI flottantes ou bloquées: suspecter d'abord `MISO/GPIO10`, puis `NSS/GPIO4` |
| `SUS BUSY` | `BUSY/GPIO6` reste haut ou le SX1262 ne sort pas du reset |
| `SUS TCXO` | erreur `TCXO/XOSC`: vérifier 3V3, GND et module Core1262 |
| `SUS DIO1` | `DIO1/GPIO7` reste haut alors que l'IRQ SX1262 est claire |
| `SUS SPI` | bus SPI instable sans GPIO unique isolé |

Réglages:

```text
CONFIG_LORA_TEST_DIAG_SPI_LOOPS=120
CONFIG_LORA_TEST_DIAG_REG_LOOPS=40
```

## Compiler

```bash
idf.py set-target esp32s3
idf.py build
```

Les valeurs importantes sont dans:

- `sdkconfig.defaults`;
- `main/Kconfig.projbuild`;
- `idf.py menuconfig` > `Waveshare S3 Core1262 LoRa test`.

Pour repartir d'une configuration propre:

```bash
rm -f sdkconfig
idf.py set-target esp32s3
idf.py build
```

## Flasher les cartes

Toujours flasher avec chiffrement:

```bash
./tools/flash_all_encrypted.sh /dev/cu.usbmodem11101 /dev/cu.usbmodem11201 /dev/cu.usbmodem11401
```

Après `encrypted-flash`, les cartes peuvent rester en bootloader. Les relancer:

```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh
for p in /dev/cu.usbmodem11101 /dev/cu.usbmodem11201 /dev/cu.usbmodem11401; do
  python "$IDF_PATH/components/esptool_py/esptool/esptool.py" --chip esp32s3 -p "$p" run
done
```

## Surveiller une carte

Ouvrir le moniteur série:

```bash
idf.py -p /dev/cu.usbmodem11101 monitor
```

Quitter le moniteur:

```text
Ctrl+]
```

Au démarrage, une carte saine affiche:

```text
Waveshare S3 Core1262 LoRa test node=8611E4 display=1 pins sck=1 mosi=2 miso=10 nss=4 reset=5 busy=6 dio1=7 rxen=8 txen=9
SX1262 status after standby: raw=0x22 cmd=1 mode=2
SX1262 status after tcxo: raw=0x22 cmd=1 mode=2
Core1262 ready freq=868100000Hz sf=7 bw=125kHz cr=4/5 power=14dBm tcxo_enum=4 sync=0x1424
```

Quand l'émission fonctionne:

```text
TX_DONE seq=0 len=41 text="LORA_TEST node=8611E4 seq=0 uptime=1638ms"
```

Quand la réception fonctionne:

```text
RX len=43 rssi=-25.0dBm snr=12.8dB irq=0x0002 text="LORA_TEST node=861464 seq=10 uptime=53188ms"
```

## Séquence d'initialisation SX1262

Le firmware fait volontairement une initialisation simple et visible dans les logs:

1. reset matériel du SX1262 via `GPIO5`;
2. passage en standby RC;
3. sélection du mode paquet LoRa;
4. activation TCXO sur DIO3 à 2.4 V;
5. effacement et lecture des `DeviceErrors`;
6. régulateur en DCDC;
7. calibration image selon la fréquence;
8. désactivation du RF switch DIO2 interne;
9. configuration fréquence, PA, OCP, puissance TX;
10. configuration LoRa SF7, BW 125 kHz, CR 4/5;
11. écriture et relecture du sync word `0x1424`;
12. routage IRQ vers DIO1;
13. lancement RX continu.

## Diagnostic rapide

### `TCXO/XOSC start error DeviceErrors=0x0020`

La radio répond au SPI, mais l'oscillateur ne démarre pas.

Actions:

- vérifier que `CONFIG_LORA_TEST_TCXO_CTRL_VOLTAGE=4`;
- vérifier l'alimentation 3.3 V;
- vérifier la masse;
- inspecter les soudures du module Core1262.

### `0xffff`, `status=0xff`, `errors=0xffff`

Réponse SPI invalide ou bus qui flotte.

Actions prioritaires:

- vérifier `MISO/GPIO10`;
- vérifier `NSS/GPIO4`;
- vérifier `SCK/GPIO1`;
- vérifier que `NSS` n'est pas court-circuité;
- garder `CONFIG_LORA_TEST_SPI_CLOCK_HZ=1000000`;
- raccourcir les fils si possible.

### `Sync word readback failed`

Le firmware a écrit le registre LoRa `0x0740`, puis la relecture ne correspond pas.

Ca pointe presque toujours vers:

- transaction SPI instable;
- MISO mal soudé;
- NSS qui ne sélectionne pas proprement le SX1262;
- radio occupée ou bloquée.

### `BUSY timeout busy=1`

Le SX1262 garde `BUSY` à HIGH trop longtemps.

Actions:

- vérifier `BUSY/GPIO6`;
- vérifier `RESET/GPIO5`;
- vérifier que le module sort bien du reset;
- couper puis remettre l'alimentation USB;
- inspecter les soudures du Core1262.

### `TX watchdog timeout`

Le firmware a demandé une émission, mais DIO1 n'a pas signalé `TX_DONE` à temps.

Actions:

- vérifier `DIO1/GPIO7`;
- vérifier `TXEN/GPIO9`;
- vérifier l'antenne;
- vérifier que la radio ne remonte pas aussi `0xffff`;
- réduire les collisions en éloignant le démarrage des cartes ou en changeant `CONFIG_LORA_TEST_TX_INTERVAL_MS`.

### RX corrompu ou texte illisible

Ca peut arriver si deux cartes émettent en même temps ou si une carte instable pollue le test.

Actions:

- tester avec seulement deux cartes;
- augmenter `CONFIG_LORA_TEST_TX_INTERVAL_MS`;
- couper la carte qui affiche des erreurs `0xffff`;
- vérifier que toutes les cartes utilisent la même fréquence et le même sync word.

## Résultat observé le 2026-06-03

- `/dev/cu.usbmodem11101`, node `8611E4`: initialisation OK, `TX_DONE` OK, RX stable depuis `861464`.
- `/dev/cu.usbmodem11401`, node `861464`: communication confirmée par les paquets reçus sur `8611E4`.
- `/dev/cu.usbmodem11201`, node `8613C8`: reçoit les deux autres et émet parfois, mais le SX1262 décroche par intermittence avec `0xffff`, `TX watchdog timeout` ou `TCXO/XOSC start error`.

La conclusion pratique: le firmware et les réglages LoRa fonctionnent. Si une carte donne `0xffff` ou `BUSY timeout`, chercher en priorité un problème de soudure, de continuité ou d'alimentation autour du module Core1262.

## Commandes utiles

Build:

```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh
idf.py build
```

Flash chiffré des trois cartes:

```bash
./tools/flash_all_encrypted.sh /dev/cu.usbmodem11101 /dev/cu.usbmodem11201 /dev/cu.usbmodem11401
```

Relance après flash:

```bash
for p in /dev/cu.usbmodem11101 /dev/cu.usbmodem11201 /dev/cu.usbmodem11401; do
  python "$IDF_PATH/components/esptool_py/esptool/esptool.py" --chip esp32s3 -p "$p" run
done
```

Monitor:

```bash
idf.py -p /dev/cu.usbmodem11101 monitor
```

Changer les paramètres:

```bash
idf.py menuconfig
```

Menu:

```text
Waveshare S3 Core1262 LoRa test
```

## Règles à retenir

- Toujours `encrypted-flash`, jamais `flash`.
- Garder TCXO DIO3 à 2.4 V, enum `4`.
- Garder SPI à 1 MHz tant que le câblage est soudé à la main.
- Toutes les cartes doivent avoir la même fréquence et le même sync word.
- `TX_DONE` prouve que l'émission radio a fini.
- `RX len=... text="LORA_TEST ..."` prouve que les modules se reçoivent.
- `0xffff` est un symptôme matériel ou SPI avant d'être un problème LoRa.
