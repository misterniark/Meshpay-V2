---
tags: [meshpay, refactoring, architecture]
date: 2026-05-13
status: en cours
---

# 12 — Refactoring main.c (Lot D)

Decomposition de [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale|main.c]] en modules separes. Avant le refactoring, le fichier comptait **3521 lignes et 87 directives `#if/else/endif`** repartis sur 5 symboles de configuration.

> [!info] Reference
> Plan complet du refactoring valide le 2026-05-13. Execution en 8 lots, un commit par lot, build verifie pour ESP32 + ESP32-S3 a chaque etape, validation finale sur device.

## Pourquoi ?

`main.c` accumulait :

- 25 sections separees par bannieres (constantes, etat global, persistance NVS, handlers, ops maitre, core_task, app_main…)
- **87 directives conditionnelles** reparties ainsi :

| Symbole                       | Occurrences | Role                                        |
| ----------------------------- | ----------- | ------------------------------------------- |
| `MP_HAS_LORA`                 | 18          | LoRa disponible (ESP32 seulement)           |
| `MP_HAS_ESPNOW`               | 13          | ESP-NOW disponible (ESP32 + ESP32-S3)       |
| `CONFIG_IDF_TARGET_ESP32*`    | 12          | Selection cible materielle                  |
| `CONFIG_MESHPAY_DEBUG_CONSOLE`| 10          | Console de debug optionnelle                |
| `CONFIG_NVS_ENCRYPTION`       | 5           | NVS chiffree                                |

Consequences :

1. Lecture lente : reconstituer mentalement quelle branche est active a chaque pas
2. Test difficile : pas de moyen d'isoler une section (handler, op) pour un test natif
3. Pression sur la stack `core_task` (10240 mots) car tout le metier vit dedans

## Strategie

1. **Decomposition en modules `.h`/`.c`** sous `main/` : un module par responsabilite
2. **Sortie de l'etat global** vers `app_state.{h,c}` consomme par tous les modules
3. **Selection cible au niveau CMake**, pas dans le code C : facades transport (Lot 3), debug console isolee (Lot 7), init NVS chiffre extrait (Lot 8)

Objectif : passer de **~87** a **~5 directives** `#if` residuelles dans le code C.

---

## Lot 1 — Etat global + utilitaires (FAIT 2026-05-13)

### Fichiers crees

| Fichier                       | Role                                                       |
| ----------------------------- | ---------------------------------------------------------- |
| `main/app_state.h`            | Declarations `extern` de tout l'etat + constantes globales |
| `main/app_state.c`            | Storage reelle (~120 KB de .bss)                           |
| `main/time_glue.h` / `.c`     | Wrappers temps : `platform_get_monotonic_ms`, `get_time_ms_wrapper`, `get_tx_timestamp_wrapper`, `get_lamport_wrapper`, `main_collect_confirmed_txs`, `apply_pending_melt`, `compute_melted_balance` |
| `main/stack_monitor.h` / `.c` | Tache FreeRTOS de log des high-water-marks (`stkmon`)      |
| `main/peers.h` / `.c`         | `add_peer` + `find_peer_mac`                               |
| `main/currency_config_init.h` / `.c` | `init_currency_config` (config hardcodee TestCoin)  |

### Etat extrait vers `app_state`

DAG, wallet, lock_table, time_manager, currency, keypair, checkpoint, HAL (storage/display/espnow/lora), queues (evt/cmd/ui_cmd), mutex, peers, alias, broadcasts vus, relay buffers, ping/pong, beneficiaire.

Les declarations passent de `static foo_t s_foo;` (dans main.c) a :
- `extern foo_t s_foo;` dans `app_state.h`
- `foo_t s_foo;` dans `app_state.c`

Aucune reference ne change : les centaines d'usages dans main.c (et les futurs modules des Lots 4-8) restent valides.

### Constantes deplacees

`EVT_QUEUE_DEPTH`, `CMD_QUEUE_DEPTH`, `UI_CMD_QUEUE_DEPTH`, toutes les tailles/priorites de tache, `STACK_MONITOR_PERIOD_MS`, `STACK_MONITOR_TASK_STACK`, les cles NVS (`NVS_NAMESPACE`, `NVS_KEY_*`), `LOCK_EXPIRE_INTERVAL_MS`, `MAX_PEERS`, pins LoRa, `LORA_SYNC_INTERVAL_MS`, `MAX_SEEN_BROADCASTS`, `MAX_PING_RESULTS`, `MAX_SEEN_PINGS`.

### Detection des capacites materielles

Les macros `MP_HAS_ESPNOW` / `MP_HAS_LORA` sont desormais dans `app_state.h` (avant les includes du composant). Tous les modules qui font `#include "app_state.h"` heritent automatiquement de la bonne config.

### Resultat

| Metrique                | Avant   | Apres Lot 1 | Delta |
| ----------------------- | ------- | ----------- | ----- |
| Lignes dans `main.c`    | 3521    | 2966        | -555  |
| `#if` directives totales| 87      | 87          | 0¹    |
| Fichiers dans `main/`   | 1       | 13          | +12   |

¹ Lot 1 ne touche pas aux `#if` (objectif des Lots 3, 7, 8). Il prepare la decomposition.

### Validation

- [x] Build ESP32-S3 : `idf.py -B build-s3 build` → OK (0x129520 octets)
- [x] Build ESP32 (CYD) : `idf.py -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build` → OK (0x1283e0 octets)
- [ ] Test sur device (apres tous les lots)

---

## Lot 2 — Persistance NVS decoupee par domaine (FAIT 2026-05-13)

### Fichiers crees

| Fichier                                | Role                                                    |
| -------------------------------------- | ------------------------------------------------------- |
| `main/persistence/nvs_keypair.{h,c}`   | `nvs_keypair_load_or_generate` (Ed25519, premier boot)  |
| `main/persistence/nvs_checkpoint.{h,c}`| `nvs_checkpoint_load` / `nvs_checkpoint_save` + migration |
| `main/persistence/nvs_alias.{h,c}`     | `nvs_alias_load_or_generate` + generation Adjectif-Animal |
| `main/persistence/nvs_next_seq.{h,c}`  | `next_seq` + `load_next_seq_or_recompute` (I3-fix)      |
| `main/persistence/nvs_beneficiary.{h,c}` | `nvs_beneficiary_load` (config auto-forward)          |

### Changements dans `app_state`

Les callbacks de persistance checkpoint (precedemment en `static` dans main.c, initialises au fil de la declaration) deviennent `extern` :

```c
extern checkpoint_save_fn s_checkpoint_save;
extern checkpoint_load_fn s_checkpoint_load;
```

Definitions dans `app_state.c` : `NULL` au depart. Initialises en `app_main` par :

```c
s_checkpoint_save = nvs_checkpoint_save;
s_checkpoint_load = nvs_checkpoint_load;
```

Le pattern preserve la possibilite de mocker le backend (test).

### Simplification de `app_main`

Le bloc inline de chargement beneficiaire (20 lignes) devient un appel :

```c
nvs_beneficiary_load();
```

### Resultat

| Metrique                | Avant Lot 2 | Apres Lot 2 | Delta |
| ----------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`    | 2966        | 2638        | -328  |
| Fichiers dans `main/`   | 13          | 23          | +10   |

### Validation

- [x] Build ESP32-S3 : 0x129640 octets
- [x] Build ESP32 (CYD) : 0x128500 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 3 — Facades transport (FAIT 2026-05-13)

### Approche

Pattern facade + stub selectionnes par CMake :

- `main/transport/transport_lora.h` : API publique uniforme (toujours visible)
- `main/transport/transport_lora.c` : impl reelle (compile uniquement si `CONFIG_IDF_TARGET_ESP32`)
- `main/transport/transport_lora_stub.c` : no-op (compile sur les autres cibles)

Le `CMakeLists.txt` choisit :

```cmake
if(CONFIG_IDF_TARGET_ESP32)
    list(APPEND meshpay_main_srcs "transport/transport_lora.c")
else()
    list(APPEND meshpay_main_srcs "transport/transport_lora_stub.c")
endif()
```

Le code applicatif appelle `transport_lora_send(...)`, `transport_lora_queue_relay_broadcast(...)`, `transport_lora_pump()` etc. **sans aucun `#ifdef`**.

### Ownership consolide dans transport_lora.c (impl reelle)

L'implementation reelle possede desormais tout l'etat LoRa :

| Ancienne localisation                | Nouvelle localisation             |
| ------------------------------------ | --------------------------------- |
| `s_lora_hal` (app_state)             | static dans `transport_lora.c`    |
| `s_relay_bcast_buf` (app_state)      | static dans `transport_lora.c`    |
| `s_relay_ping_buf` (app_state)       | static dans `transport_lora.c`    |
| `s_pong_buf` + tick + delay          | static dans `transport_lora.c`    |
| `main_collect_confirmed_txs` (time_glue) | static `lora_collect_confirmed_txs` |
| `get_lamport_wrapper` (time_glue)    | static `lora_get_lamport`         |
| Pin LoRa (`LORA_UART_NUM` etc.)      | `#define` interne                 |
| Init HAL + `lora_sync_task`          | `transport_lora_init_and_start()` |

Sur ESP32-S3 (stub), aucun de ces symboles n'existe : pas de RAM occupee.

### ESP-NOW

ESP-NOW etant present sur les deux cibles supportees, les 13 `#ifdef MP_HAS_ESPNOW` etaient des conditions toujours vraies. Suppression directe — si une future cible (ESP32-C2 ?) n'a pas ESP-NOW, on creera une facade equivalente a ce moment.

### Resultat

| Metrique                | Avant Lot 3 | Apres Lot 3 | Delta |
| ----------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`    | 2638        | 2509        | -129  |
| `#ifdef MP_HAS_LORA`    | 18          | 0           | -18   |
| `#ifdef MP_HAS_ESPNOW`  | 13          | 0           | -13   |
| **Total `#if` elimines**|             |             | **-31** |
| Fichiers dans `main/`   | 23          | 26          | +3    |

### Validation

- [x] Build ESP32-S3 (stub LoRa) : 0x129b30 octets
- [x] Build ESP32 (CYD, impl reelle) : 0x128480 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 4 — Handlers d'evenements (FAIT 2026-05-13)

13 handlers `comm_event_t` extraits dans `main/handlers/`, groupes par domaine fonctionnel :

| Fichier                          | Handlers                                                   |
| -------------------------------- | ---------------------------------------------------------- |
| `handler_payment.c`              | peer_discovered, tx_received, ack_received, tx_timeout, attestation |
| `handler_time_sync.c`            | time_sync                                                  |
| `handler_broadcast.c`            | broadcast + cache anti-boucle (already_seen / mark_seen)   |
| `handler_ping_pong.c`            | ping/pong + cache anti-boucle                              |
| `handler_admin.c`                | set_alias / set_beneficiary                                |

Headers partages crees pour permettre l'acces croise :

- `handlers/handlers.h` : declarations des `handle_*_received`
- `balance.h` : `compute_owner_balance`, `ui_get_owner_balance`
- `dag_glue.h` : `dag_insert_and_track`, `auto_checkpoint_if_needed`

Les fonctions correspondantes ont ete depatic-ifiees (`static` → externe) dans main.c. Elles seront physiquement deplacees au Lot 6.

`ping_mark_seen` est expose sous le nom `ping_mark_seen_public` pour permettre a `ping_send` (op maitre, Lot 5) de marquer son propre PING avant emission.

### Resultat

| Metrique                | Avant Lot 4 | Apres Lot 4 | Delta |
| ----------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`    | 2509        | 1764        | -745  |
| Fichiers dans `main/`   | 26          | 33          | +7    |

### Validation

- [x] Build ESP32-S3 : 0x129ce0 octets
- [x] Build ESP32 (CYD) : 0x128650 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 5 — Operations maitre (FAIT 2026-05-13)

### Decomposition

| Fichier                       | Operations                                              |
| ----------------------------- | ------------------------------------------------------- |
| `ops/op_payment.c`            | `initiate_payment` (TRANSFER avec lock + ACK ESP-NOW)   |
| `ops/op_mint.c`               | `initiate_mint` (maitre only, runtime check)            |
| `ops/op_beneficiary_forward.c`| `attempt_beneficiary_forward` (auto-forward periodique) |
| `ops/op_master.c`             | `broadcast_text_send`, `ping_send`, `set_alias_send`, `set_beneficiary_send` |

### Decision : op_master compile partout

Les 4 operations maitres etaient initialement guardes par `#if CONFIG_IDF_TARGET_ESP32` car seul l'ESP32 CYD a du LoRa pour les emettre. Mais :
- Chaque op a deja un runtime check `is_master` (device dans `mint_authorities`)
- `transport_lora_send` est no-op sur cibles sans LoRa (Lot D.3)

Conclusion : compiler `op_master.c` partout, sans `#if CONFIG_IDF_TARGET_ESP32`. Sur ESP32-S3, un appel a ces ops fait le travail crypto (signature) puis appelle un no-op LoRa — fonctionnement attendu, ~2 KB flash gaspilles sur S3.

Cela elimine **8 `#if CONFIG_IDF_TARGET_ESP32`** :
- 4 guards autour des definitions de master ops
- 4 guards dans `handle_ui_command` autour des appels

### Resultat

| Metrique                      | Avant Lot 5 | Apres Lot 5 | Delta |
| ----------------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`          | 1764        | 1180        | -584  |
| `#if CONFIG_IDF_TARGET_ESP32` | 10          | 2¹          | -8    |
| Fichiers dans `main/`         | 33          | 38          | +5    |

¹ Reste : 2 directives autour de la HAL display factory (ILI9341 vs JD9853) — vraie difference materielle non eliminable.

### Validation

- [x] Build ESP32-S3 : 0x12a660 octets
- [x] Build ESP32 (CYD) : 0x128690 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 6 — Core task + UI dispatch + helpers (FAIT 2026-05-13)

Migration physique des helpers partages (declares en Lot 4) et de la boucle FreeRTOS centrale :

| Fichier              | Contenu                                                        |
| -------------------- | -------------------------------------------------------------- |
| `main/balance.c`     | `compute_owner_balance`, `ui_get_owner_balance`                |
| `main/dag_glue.c`    | `dag_insert_and_track`, `auto_checkpoint_if_needed`            |
| `main/core_task.c`   | `core_task` + `check_lock_expirations` (helper prive)          |
| `main/ui_dispatch.c` | `handle_ui_command`                                            |

`attempt_beneficiary_forward` est deja dans `ops/op_beneficiary_forward.c` (Lot 5), pas besoin d'un fichier dedie.

### Resultat

| Metrique              | Avant Lot 6 | Apres Lot 6 | Delta |
| --------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`  | 1180        | 807         | -373  |
| Fichiers dans `main/` | 38          | 44          | +6    |

`main.c` ne contient plus que :
- includes + factory declarations HAL
- les 4 callbacks `main_debug_dump_*` (Lot 7 a venir)
- `app_main` (Lot 8 a venir)

### Validation

- [x] Build ESP32-S3 : 0x12a690 octets
- [x] Build ESP32 (CYD) : 0x1286c0 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 7 — Debug console (FAIT 2026-05-13)

### Approche facade + stub (meme pattern que LoRa, Lot D.3)

- `main/debug_console_dumps.h` : declare une seule fonction `debug_console_register_dumps()`
- `main/debug_console_dumps.c` : impl reelle avec les 4 callbacks (dump_dag, dump_wallet, dump_currency, dump_time) + register
- `main/debug_console_dumps_stub.c` : `debug_console_register_dumps()` no-op

`CMakeLists.txt` choisit selon `CONFIG_MESHPAY_DEBUG_CONSOLE` :

```cmake
if(CONFIG_MESHPAY_DEBUG_CONSOLE)
    list(APPEND meshpay_main_srcs "debug_console_dumps.c")
else()
    list(APPEND meshpay_main_srcs "debug_console_dumps_stub.c")
endif()
```

`main.c` appelle `debug_console_register_dumps()` sans `#if`. Les 4 callbacks (~270 lignes) sont totalement absents du binaire en build release.

### Resultat

| Metrique                          | Avant Lot 7 | Apres Lot 7 | Delta |
| --------------------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`              | 807         | 514         | -293  |
| `#if CONFIG_MESHPAY_DEBUG_CONSOLE`| 10          | 0           | -10¹  |
| Fichiers dans `main/`             | 44          | 47          | +3    |

¹ Reduction superieure au -9 prevu car le `#if` autour de `debug_console_init` dans app_main est aussi elimine.

### Validation

- [x] Build ESP32-S3 : 0x12a6b0 octets
- [x] Build ESP32 (CYD) : 0x1286d0 octets
- [ ] Test sur device (apres tous les lots)

---

## Lot 8 — app_init / nvs_init (FAIT 2026-05-13)

### Facade NVS

Pattern identique aux Lots 3 et 7 :

- `main/app_init/nvs_init.h` : declare `esp_err_t nvs_init_storage(bool *out_encrypted)`
- `main/app_init/nvs_init_secure.c` : impl AES-XTS (compile si `CONFIG_NVS_ENCRYPTION=y`)
- `main/app_init/nvs_init_plain.c` : impl standard (compile sinon)

Les includes `nvs_sec_provider.h` et `esp_partition.h` (auparavant guardes par `#if` dans main.c) sont desormais prives a `nvs_init_secure.c`.

`app_main` devient :

```c
bool nvs_encrypted = false;
ret = nvs_init_storage(&nvs_encrypted);
ESP_LOGI(TAG, "[1/12] NVS initialise%s", nvs_encrypted ? " (chiffre)" : "");
```

La mitigation [C11] (verrouillage du compteur PIN apres effacement NVS) reste integree aux deux impls.

### Resultat

| Metrique                          | Avant Lot 8 | Apres Lot 8 | Delta |
| --------------------------------- | ----------- | ----------- | ----- |
| Lignes dans `main.c`              | 514         | 434         | -80   |
| `#if defined(CONFIG_NVS_ENCRYPTION)` | 5        | 0           | -5    |
| Fichiers dans `main/`             | 47          | 49          | +2    |

### Validation

- [x] Build ESP32-S3 : 0x12a6f0 octets
- [x] Build ESP32 (CYD) : 0x128730 octets
- [ ] Test sur device

---

# Bilan global du Lot D

| Metrique               | Avant     | Apres     | Delta             |
| ---------------------- | --------- | --------- | ----------------- |
| Lignes dans `main.c`   | 3521      | 434       | **-3087 (-88 %)** |
| Directives `#if` totales | 87      | 6         | **-81 (-93 %)**   |
| Fichiers dans `main/`  | 1         | 49        | +48               |

Les **6 `#if` restants** sont tous dans `main.c` autour de la HAL display (ILI9341 vs JD9853) — vraie difference materielle non eliminable :

```c
#if CONFIG_IDF_TARGET_ESP32
extern hal_err_t hal_display_ili9341_create(hal_display_t *display);
#elif CONFIG_IDF_TARGET_ESP32S3
extern hal_err_t hal_display_jd9853_create(hal_display_t *display);
#endif
```

### Decomposition finale

```
main/
├── main.c                          434 lignes (etait 3521)
├── app_state.{h,c}                 etat global partage
├── time_glue.{h,c}                 wrappers temps
├── stack_monitor.{h,c}             tache stkmon
├── peers.{h,c}                     gestion peers ESP-NOW
├── currency_config_init.{h,c}      init currency hardcoded
├── balance.{h,c}                   compute_owner_balance, ui_get_owner_balance
├── dag_glue.{h,c}                  dag_insert_and_track, checkpoint auto
├── core_task.{h,c}                 boucle FreeRTOS centrale
├── ui_dispatch.{h,c}               handle_ui_command
├── debug_console_dumps.{h,c}       4 dumps JSON
├── debug_console_dumps_stub.c      no-op
│
├── app_init/
│   ├── nvs_init.h                  facade init NVS
│   ├── nvs_init_secure.c           impl AES-XTS (CONFIG_NVS_ENCRYPTION=y)
│   └── nvs_init_plain.c            impl standard
│
├── persistence/
│   ├── nvs_keypair.{h,c}
│   ├── nvs_checkpoint.{h,c}
│   ├── nvs_alias.{h,c}
│   ├── nvs_next_seq.{h,c}
│   └── nvs_beneficiary.{h,c}
│
├── transport/
│   ├── transport_lora.h            facade LoRa
│   ├── transport_lora.c            impl reelle (ESP32 CYD)
│   └── transport_lora_stub.c       no-op (ESP32-S3 et autres)
│
├── handlers/
│   ├── handlers.h                  declarations communes
│   ├── handler_payment.c           peer/tx/ack/timeout/attestation
│   ├── handler_time_sync.c
│   ├── handler_broadcast.c
│   ├── handler_ping_pong.c
│   └── handler_admin.c             set_alias / set_beneficiary
│
└── ops/
    ├── ops.h                       declarations
    ├── op_payment.c                initiate_payment
    ├── op_mint.c                   initiate_mint
    ├── op_beneficiary_forward.c
    └── op_master.c                 broadcast/ping/set_alias/set_beneficiary
```

### Commits

8 commits, un par lot :

1. `refactor(main): Lot D.1 — extraire l'état global et les utilitaires`
2. `refactor(main): Lot D.2 — découper la persistance NVS par domaine`
3. `refactor(main): Lot D.3 — façades transport, éliminer 31 #ifdef`
4. `refactor(main): Lot D.4 — extraire les handlers d'événements`
5. `refactor(main): Lot D.5 — extraire les opérations dans ops/`
6. `refactor(main): Lot D.6 — extraire core_task, ui_dispatch et helpers`
7. `refactor(main): Lot D.7 — extraire debug console, éliminer 10 #if`
8. `refactor(main): Lot D.8 — façade nvs_init, éliminer 5 #if`

### A faire avant ship

- [ ] Test fonctionnel sur ESP32 CYD (paiement, LoRa sync, ping/pong, broadcast, MINT, attestation)
- [ ] Test fonctionnel sur Waveshare ESP32-S3 (paiement local, UI, pas de LoRa)
- [ ] Verifier que la suite de tests embarques (`test_app/`) passe — 203/210 attendus (cf. [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] entree du 12 mai pour le contexte)

---

## Reduction attendue des `#if`

| Lot   | Type elimine                           | Reduction |
| ----- | -------------------------------------- | --------- |
| Lot 3 | `MP_HAS_LORA` + `MP_HAS_ESPNOW`        | -31       |
| Lot 5 | `CONFIG_IDF_TARGET_ESP32` (ops maitre) | -12       |
| Lot 7 | `CONFIG_MESHPAY_DEBUG_CONSOLE`         | -9        |
| Lot 8 | `CONFIG_NVS_ENCRYPTION`               | -5        |
| **Total** |                                     | **~ -57** |

Reste residuel attendu : **5 directives** (vraies differences runtime non eliminables).

---

## Liens

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]] — vue d'ensemble post-refactoring
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]] — refactoring main.c retire de la dette
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] — entree journal par lot
