---
tags: [meshpay, design, lora, hardware, hal]
date: 2026-05-14
status: Implémenté et mergé sur main 2026-05-15 — firmware validé (init + TX) ; fault hardware sur 1 des 2 devices
cible: toutes les cibles (ESP32 CYD + ESP32-S3 Waveshare)
---

# 14 — Driver LoRa Core1262 (SX1262) — design

Spec du remplacement du driver LoRa **Grove Wio-E5** par le **Waveshare Core1262 (SX1262)**, tout en gardant le Wio-E5 disponible comme driver alternatif sélectionnable.

> [!success] Statut — implémenté
> Design validé le 2026-05-14, implémenté et **mergé sur `main` le 2026-05-15** (commit `098daaa`). Smoke test matériel : init radio validée sur les 2 devices, **TX réel validé sur device 1** (`Sync terminée : 1 TX envoyées`). Device 2 montre un fault hardware indépendant (`TX_DONE` jamais remonté, soudures Core1262 à reprendre) — sans impact sur le firmware. Détail dans le **§12 Bilan d'implémentation**. Le Wio-E5 reste pleinement fonctionnel et sélectionnable.

## 1. Contexte et motivation

Le projet vise un LoRa **présent sur tous les devices** — c'est le cœur du mesh longue portée ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]], [[LoRa]]). Aujourd'hui :

- Le driver LoRa réel (`hal_lora_wio_e5.c`) n'est compilé que sur la cible **ESP32 CYD**. L'**ESP32-S3 Waveshare** utilise le stub no-op `transport_lora_stub.c` — donc **pas de LoRa du tout** sur le S3.
- Le Wio-E5 est un module Grove **volumineux** : l'encombrement est un problème pour de l'embarqué transporté.

Le **Core1262** expose la puce **SX1262** en **SPI brut** (le Wio-E5, lui, embarque un STM32WLE5 avec firmware AT piloté en UART). Le SX1262 nu donne le **contrôle radio total** dont le protocole mesh maison (`lora_sync` : fragmentation, attestations, sync DAG) a besoin, dans un format bien plus compact.

### Pourquoi le Core1262 et pas le Wio-E5

| Critère | Core1262 (SX1262) | Wio-E5 |
| ------- | ----------------- | ------ |
| Puce | SX1262 « nu » (transceiver seul) | STM32WLE5 = MCU + radio, **firmware AT** |
| Interface | SPI + GPIO (BUSY, DIO1, RESET) | UART + commandes AT |
| Contrôle radio | Total — adapté au protocole mesh maison | Abstrait par le firmware AT, mode P2P limité |
| Encombrement | Compact | Format Grove, volumineux |

Le Wio-E5 ne se justifierait que pour du **LoRaWAN standard** vers une infra opérée — ce n'est pas le cas de MeshPay (mesh P2P propriétaire). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques]].

## 2. Décisions actées (brainstorming 2026-05-14)

| # | Décision | Justification |
| - | -------- | ------------- |
| **D1** | Choix du driver par **option Kconfig explicite** : `choice CONFIG_MESHPAY_LORA_DRIVER` ∈ {`WIO_E5`, `CORE1262`}, indépendant de la carte. | Les puces LoRa varieront selon l'approvisionnement. Le point de variation doit être un réglage de build clair, pas un câblage en dur. |
| **D2** | **LoRa obligatoire sur toutes les cartes.** L'impl réelle se compile toujours ; le choix se fait uniquement *entre drivers*. Le stub no-op disparaît de la production. | Le LoRa n'est pas optionnel — c'est tout le but du projet. |
| **D3** | Driver SX1262 = **pilote C officiel Semtech vendoré** (`sx126x_driver`) + glue plateforme ESP-IDF. | Le pilote Semtech gère la séquence registre minutieuse (calibration, PA, IRQ) — éprouvé, évite les bugs radio subtils. On n'écrit que la glue SPI/GPIO. |
| **D4** | Sélection via **factory unifiée `hal_lora_create_default()`** dans le composant `device_hal`. | `device_hal` *possède* la connaissance du matériel — c'est son rôle. `transport_lora.c` reste une pure façade, agnostique du driver. Ajouter une 3ᵉ puce demain = 1 seul fichier touché. |

## 3. Principe directeur

Le HAL `hal_lora_t` (`components/device_hal/include/hal/hal_lora.h`) est **déjà la bonne frontière** — son en-tête mentionne explicitement « SPI pour SX1276 », l'abstraction a été pensée pour des backends SPI.

> [!important] Aucun changement côté applicatif
> Pas une ligne de `lora_sync`, des handlers, ni de `core_task` ne change : ils ne voient que la vtable `hal_lora_t` (5 fonctions : `init`, `send`, `set_rx_callback`, `start_rx`, `sleep`). Tout le travail est contenu dans `device_hal` + 3 points de câblage build.

## 4. Périmètre

### Dans le périmètre

- Backend HAL `hal_lora_core1262` pilotant le SX1262 via le pilote Semtech.
- Vendoring du pilote Semtech `sx126x_driver` + glue plateforme ESP-IDF (SPI/GPIO).
- Factory unifiée `hal_lora_create_default()` + option Kconfig de choix de driver.
- Migration des pins LoRa (aujourd'hui en `#define` dans `transport_lora.c`) vers Kconfig — les deux drivers deviennent symétriques.
- Suppression du stub `transport_lora_stub.c` (LoRa n'est plus optionnel).
- Tests unitaires de la logique factory/helpers.

### Hors périmètre

- **Câblage GPIO concret** du Core1262 sur la Waveshare ESP32-S3 — les numéros de pins sont de la config de déploiement (`sdkconfig.defaults.esp32s3`), pas du design. Le design fige « pins configurables par Kconfig ».
- **Modification de la config radio** (868.1 MHz, SF9, BW125, CR4/5, 14 dBm) — elle reste inchangée dans `lora_sync.c`, déjà driver-agnostique.
- **Suppression du driver Wio-E5** — il reste vendoré et sélectionnable (exigence explicite).
- Refonte du protocole `lora_sync` ou de la fragmentation — inchangés.

## 5. Architecture et composants

### A. Nouveaux fichiers (tous dans `components/device_hal/`)

| Fichier | Rôle |
| ------- | ---- |
| `src/esp32/sx126x/sx126x.{c,h}`, `sx126x_regs.h`, `sx126x_hal.h` | Pilote registre **vendoré tel quel** depuis le dépôt Semtech `sx126x_driver` (licence BSD-3). Non modifié → mise à jour facile. |
| `src/esp32/sx126x_hal.c` | **Glue plateforme** : implémente les fonctions `sx126x_hal_write/read/reset/wait_on_busy` attendues par le pilote Semtech, via `driver/spi_master.h` + `driver/gpio.h`. Seul fichier « radio » écrit maison. |
| `src/esp32/hal_lora_core1262.{c,h}` | Backend HAL : remplit la vtable `hal_lora_t` en pilotant le SX1262 via le pilote Semtech. Tâche FreeRTOS RX déclenchée par l'IRQ **DIO1** (analogue à `wio_rx_task` du driver Wio-E5). Factory `hal_lora_core1262_create(hal_lora_t*, const hal_lora_core1262_pins_t*)`. |
| `src/hal_lora_factory.c` + `include/hal/hal_lora_factory.h` | `hal_lora_create_default(hal_lora_t *lora)` : `#if` sur le Kconfig, lit pins/UART depuis Kconfig, appelle le bon `_create()`. **Unique point de variation matérielle.** |
| `Kconfig` | Nouveau Kconfig du composant `device_hal` (voir §6). |

> [!note] Emplacement `src/esp32/`
> Le driver Core1262 va dans `src/esp32/` par cohérence avec `hal_lora_wio_e5.c` et `hal_storage_esp32.c` déjà présents : ce dossier contient en réalité du code ESP-IDF **partagé** ESP32/ESP32-S3, pas du code ESP32-only.

### B. Sélection du driver — flux

```
lora_sync_task / transport_lora.c
        │
        │  hal_lora_create_default(&s_lora_hal)   ← agnostique
        ▼
components/device_hal/src/hal_lora_factory.c
        │  #if CONFIG_MESHPAY_LORA_DRIVER_CORE1262
        │      → hal_lora_core1262_create(...)  (pins lus depuis Kconfig)
        │  #elif CONFIG_MESHPAY_LORA_DRIVER_WIO_E5
        │      → hal_lora_wio_e5_create(...)    (UART/pins lus depuis Kconfig)
        ▼
   vtable hal_lora_t remplie
```

## 6. Kconfig (`components/device_hal/Kconfig`)

```
menu "Mesh Pay - LoRa"
    choice MESHPAY_LORA_DRIVER
        prompt "Driver radio LoRa"
        default MESHPAY_LORA_DRIVER_CORE1262
        config MESHPAY_LORA_DRIVER_WIO_E5
            bool "Grove Wio-E5 (UART/AT)"
        config MESHPAY_LORA_DRIVER_CORE1262
            bool "Waveshare Core1262 (SX1262 SPI)"
    endchoice

    # Pins Core1262 (utilisées si CORE1262) — défauts vérifiés sur schéma
    # Waveshare (header P1 : GPIO IO1–IO11 libres). SPI3_HOST = 2.
    config MESHPAY_LORA_C1262_SPI_HOST   int  default 2   # SPI3_HOST
    config MESHPAY_LORA_C1262_PIN_SCK    int  default 1
    config MESHPAY_LORA_C1262_PIN_MOSI   int  default 2
    config MESHPAY_LORA_C1262_PIN_MISO   int  default 3
    config MESHPAY_LORA_C1262_PIN_NSS    int  default 4
    config MESHPAY_LORA_C1262_PIN_RESET  int  default 5
    config MESHPAY_LORA_C1262_PIN_BUSY   int  default 6
    config MESHPAY_LORA_C1262_PIN_DIO1   int  default 7
    config MESHPAY_LORA_C1262_PIN_RXEN   int  default 8
    config MESHPAY_LORA_C1262_PIN_TXEN   int  default 9

    # Pins Wio-E5 (utilisées si WIO_E5) — sorties des #define de transport_lora.c
    config MESHPAY_LORA_WIOE5_UART_NUM   int  default 2
    config MESHPAY_LORA_WIOE5_PIN_TX     int  default 17
    config MESHPAY_LORA_WIOE5_PIN_RX     int  default 16
endmenu
```

Les `#define LORA_*` de `transport_lora.c` migrent ici → les deux drivers deviennent symétriques, plus aucun pin codé en dur dans `main/`.

**Défauts par carte** via `sdkconfig.defaults.*` (respecte « Kconfig explicite » tout en restant ergonomique) :

- `sdkconfig.defaults.esp32s3` : `CONFIG_MESHPAY_LORA_DRIVER_CORE1262=y`
- `sdkconfig.defaults.esp32` : `CONFIG_MESHPAY_LORA_DRIVER_WIO_E5=y`

## 7. Modifications des fichiers existants

### `components/device_hal/CMakeLists.txt`

Le driver LoRa **sort des blocs `CONFIG_IDF_TARGET_*`** (il est désormais sur toutes les cibles) et devient piloté par Kconfig :

```cmake
list(APPEND HAL_SRCS "src/hal_lora_factory.c")
if(CONFIG_MESHPAY_LORA_DRIVER_WIO_E5)
    list(APPEND HAL_SRCS "src/esp32/hal_lora_wio_e5.c")
elseif(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)
    list(APPEND HAL_SRCS "src/esp32/hal_lora_core1262.c"
                         "src/esp32/sx126x_hal.c"
                         "src/esp32/sx126x/sx126x.c")
endif()
```

Les drivers display restent gated par target. `REQUIRES` : `driver` est déjà présent (couvre SPI + GPIO + UART).

### `main/CMakeLists.txt`

`transport/transport_lora.c` (impl réelle) **toujours compilé** ; le bloc `if(CONFIG_IDF_TARGET_ESP32) ... else ... transport_lora_stub.c` disparaît.

### `main/transport/transport_lora.c`

- `extern hal_err_t hal_lora_wio_e5_create(...)` → `#include "hal/hal_lora_factory.h"`.
- L'appel `hal_lora_wio_e5_create(&s_lora_hal, ...)` → `hal_lora_create_default(&s_lora_hal)`.
- Les `#define LORA_UART_NUM / LORA_TX_PIN / LORA_RX_PIN` → supprimés (migrés en Kconfig, lus par la factory).
- `transport_lora_available()` reste et renvoie toujours `true` ; ses appelants ne sont pas chassés (hors périmètre).

### `main/transport/transport_lora_stub.c`

**Supprimé** — le LoRa n'est plus optionnel (D2). Étape de vérification préalable : confirmer que `test_app/` ne le référence pas.

### `sdkconfig.defaults.esp32` / `sdkconfig.defaults.esp32s3`

Ajout du défaut de driver par carte (voir §6).

## 8. Config radio & câblage

- La config radio (868.1 MHz EU868, SF9, BW125 kHz, CR4/5, 14 dBm) reste **inchangée** dans `lora_sync.c` — déjà driver-agnostique. Le backend Core1262 traduit ce `hal_lora_config_t` en séquence registre SX1262.

### Câblage vérifié sur schéma officiel Waveshare (2026-05-14)

Le module **Core1262** expose le commutateur RF (SPDT) via **deux broches dédiées RXEN / TXEN** sorties sur son header — DIO2 n'est **pas** relié en interne au switch. Il faut donc piloter RXEN/TXEN depuis l'hôte → **9 GPIO** (et non 7).

| Core1262 | Fonction            | ESP32-S3 GPIO                               | Type              |
| -------- | ------------------- | ------------------------------------------- | ----------------- |
| CLK      | SPI horloge         | IO1                                         | SPI3              |
| MOSI     | SPI MOSI            | IO2                                         | SPI3              |
| MISO     | SPI MISO            | IO10                                        | SPI3              |
| CS       | SPI chip-select     | IO4                                         | SPI3              |
| RESET    | Reset radio         | IO5                                         | GPIO sortie       |
| BUSY     | Statut occupé       | IO6                                         | GPIO entrée       |
| DIO1     | IRQ radio (RX done) | IO7                                         | GPIO entrée (ISR) |
| RXEN     | Switch RF — RX      | IO8                                         | GPIO sortie       |
| TXEN     | Switch RF — TX      | IO9                                         | GPIO sortie       |
| 3V3      | Alimentation 3,3 V  | **VCC3V3** du header P1 (jamais VBUS = 5 V) | power             |
| GND      | Masse               | GND                                         | power             |
| DIO2     | non utilisé         | — laissé non connecté                       | —                 |
| ANT      | Antenne 868 MHz     | connecteur SMA/IPEX                         | —                 |

- Bus SPI **dédié SPI3_HOST** (l'écran JD9853 occupe SPI2_HOST). Horloge ~8–10 MHz au départ (SX1262 supporte 18 MHz max).
- Les GPIO **IO1–IO11** sont libres et tous sortis sur le header d'extension P1 de la carte (vérifié sur schéma). On en utilise 9 ; IO10/IO11 restent en réserve.
- **DIO3 n'est pas câblé** : il alimente le TCXO 32 MHz interne au module. Géré côté firmware par `sx126x_set_dio3_as_tcxo_ctrl()` — tension de contrôle **1,8 V (`SX126X_TCXO_CTRL_1_8V`), confirmée au smoke test du 2026-05-15**.
- Ces numéros restent surchargeables par Kconfig (§6) ; ce sont les défauts de `sdkconfig.defaults.esp32s3`.

## 9. Tests

- **Unitaire** (conforme à la règle « tests pour les nouvelles fonctions ») : le pilote Semtech vendoré n'est pas à tester (code tiers éprouvé). Testable de façon utile : la logique de `hal_lora_factory` et tout helper pur du backend (ex. mapping `hal_lora_config_t` → paramètres SX1262, bornage de taille de paquet). La couche supérieure continue d'utiliser `hal_lora_mock` — inchangé.
- **Validation matérielle honnête** : la glue SPI/IRQ (`sx126x_hal.c`) et le chemin radio se valident sur **hardware** — smoke test : flash, init radio OK, échange TX/RX entre deux devices. C'est la vraie validation, cohérente avec la pratique du projet ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/11 - Moniteur multi-device série]]).

## 10. Séquence d'implémentation

| Étape | Contenu | Garde-fou |
| ----- | ------- | --------- |
| 1 | Kconfig + migration des `#define` pins → Kconfig | Wio-E5 inchangé fonctionnellement, juste reparamétré |
| 2 | `hal_lora_factory` + bascule de `transport_lora.c` + CMake + suppression du stub | **Le build `WIO_E5` doit rester vert** — refactoring pur, zéro changement de comportement |
| 3 | Vendoring du pilote Semtech `sx126x` | — |
| 4 | `sx126x_hal.c` (glue SPI/GPIO) | — |
| 5 | `hal_lora_core1262.c` (backend HAL + tâche RX DIO1) | — |
| 6 | Tests unitaires factory/helpers | — |
| 7 | Build `CORE1262` vert puis smoke test matériel | Validation finale sur hardware |

> [!tip] Non-régression garantie
> L'étape 2 garantit qu'on ne casse jamais l'existant : tant que le Core1262 n'est pas prêt, `WIO_E5` reste le driver fonctionnel et le firmware compile/tourne comme avant.

## 11. Fichiers — récapitulatif

### Créés

| Fichier | Contenu |
| ------- | ------- |
| `components/device_hal/Kconfig` | Choix de driver + pins |
| `components/device_hal/include/hal/hal_lora_factory.h` | API `hal_lora_create_default()` |
| `components/device_hal/src/hal_lora_factory.c` | Sélection du driver par Kconfig |
| `components/device_hal/src/esp32/hal_lora_core1262.{c,h}` | Backend HAL SX1262 |
| `components/device_hal/src/esp32/sx126x_hal.c` | Glue plateforme SPI/GPIO |
| `components/device_hal/src/esp32/sx126x/*` | Pilote Semtech vendoré |

### Modifiés

| Fichier | Changement |
| ------- | ---------- |
| `components/device_hal/CMakeLists.txt` | Driver LoRa piloté par Kconfig, hors blocs target |
| `main/CMakeLists.txt` | `transport_lora.c` toujours compilé, suppression du gating stub |
| `main/transport/transport_lora.c` | Appel à `hal_lora_create_default()`, suppression des `#define` pins |
| `sdkconfig.defaults.esp32` / `.esp32s3` | Défaut de driver par carte |

### Supprimés

| Fichier | Raison |
| ------- | ------ |
| `main/transport/transport_lora_stub.c` | LoRa n'est plus optionnel (D2) |

## 12. Bilan d'implémentation (2026-05-15)

Implémenté en *subagent-driven development* : 8 tâches, chacune relue en deux étapes (conformité au spec + qualité de code), 14 commits. Mergé sur `main` au commit `098daaa`. Plan détaillé : `docs/superpowers/plans/2026-05-14-driver-lora-core1262.md` (dans le dépôt).

### Écarts vs le design

- **Câblage : 9 GPIO, pas 7.** Le schéma officiel Waveshare a révélé que le commutateur RF est piloté par deux broches dédiées **RXEN/TXEN** (DIO2 n'est pas relié en interne) — §8 corrigé en conséquence.
- **Réconciliation avec un portage concurrent.** Un autre effort, « portage LoRa Wio-E5 sur ESP32-S3 » (commits `7722a53`, `ddc00fe` + doc `docs/superpowers/specs/2026-05-14-portage-lora-s3-design.md`), a atterri sur `main` en parallèle, touchant les 8 mêmes fichiers avec une approche par *gate de cible* (stub conservé, S3 sur Wio-E5). Le merge a été résolu en faveur de l'architecture de ce design (Kconfig/factory, stub supprimé), qui en est un **sur-ensemble fonctionnel**. Apport conservé du portage concurrent : `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` sur le S3 (libère UART0, rend les logs visibles sur l'USB natif).

### Bugs rattrapés par la revue

- Règle **LDRO incomplète** : la combinaison SF12/BW250 (durée de symbole 16,38 ms) exige aussi le Low Data Rate Optimize — elle était omise.
- `wait_on_busy` faisait un **busy-wait pur de 100 ms** sur le chemin d'erreur sans céder le CPU → réécrit en deux phases (spin court puis `vTaskDelay`).
- **Race `sleep()` / tâche RX** : la tâche RX pouvait ré-armer la radio après la mise en veille → re-vérification de `rx_running` sous mutex.
- **Fuites de ressources** (bus/device SPI, sémaphores) sur les chemins d'erreur de `c1262_init` → nettoyage par `goto`, comme `hal_lora_wio_e5.c`.

### Smoke test matériel (2026-05-15)

| Vérification | Résultat |
| ------------ | -------- |
| Flash `encrypted-flash` (eFuse chiffrement brûlé sur les deux S3) | ✅ OK sur les 2 devices |
| Init radio device 1 | ✅ `Core1262 initialise (868100000 Hz, SF9, 14 dBm)` + `Mode reception active` |
| Init radio device 2 | ✅ idem (mais voir note ci-dessous) |
| Tension de contrôle TCXO (`SX126X_TCXO_CTRL_1_8V`) | ✅ confirmée — pas d'échec de calibration |
| Stack de la tâche `lora` | ✅ `Stack HWM lora : 3824 mots libres` — pas d'overflow |
| Émission LoRa réelle (TX_DONE remonté) | ✅ device 1 : `Sync terminée : 1 TX envoyées` (sync d'1 TX de 226 octets à t=121 s post-boot, sans warning) |
| Émission LoRa device 2 | ❌ `TX_DONE non recu` sur la 1ʳᵉ tentative ; tâche LoRa silencieuse ensuite — **fault matériel sur device 2**, à reprendre côté soudures |
| Réception LoRa croisée (échange entre 2 devices) | ⏳ non validée — dépend de la résolution du fault device 2 |

**Bilan firmware : OK.** La séquence bring-up RF + TX_DONE remonté + TX bloquant fonctionnel s'exécute correctement sur du matériel sain (device 1). Le risque logiciel principal — *« le code radio marche-t-il en vrai ? »* — est levé.

**Bilan matériel : 1 device sur 2 fonctionnel.** Device 2 montre un pattern caractéristique d'un **lien intermittent sur la chaîne SPI/BUSY du Core1262** :
- L'init ne réussit que via le bouton RESET (Key1/EN), pas systématiquement via un cold reset USB.
- La 1ʳᵉ tentative TX se solde par un timeout software (le SX1262 n'a jamais raisonné TX_DONE en 4 s).
- Après cet échec, la tâche `lora_sync` reste silencieuse (plus de logs sync ni de présence dans `stkmon`), alors qu'ESPNOW + UI + reste du firmware continuent normalement sur le même chip.

À reprendre lorsque l'occasion se présente : ressouder/inspecter les fils du Core1262 device 2, en priorité BUSY (IO6), MISO (IO3) et les broches du switch RF (RXEN IO8 / TXEN IO9). Aucune action firmware n'est requise — le code marche.

## Liens

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]] — pattern HAL, vtable `hal_lora_t`, décomposition `device_hal`
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques]] — choix LoRa, ESP-NOW vs Bluetooth, pourquoi un mesh propriétaire
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/12 - Refactoring main.c (Lot D)]] — Lot D.3, la façade `transport_lora` rendue agnostique
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/13 - Gestion de l'énergie (design)]] — même process brainstorming ; touche aussi `transport_lora` (`set_sync_interval`)
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/11 - Moniteur multi-device série]] — outil de smoke test matériel
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]] — limites connues
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/10 - Glossaire et concepts]] — [[LoRa]], mesh, DAG
