---
tags: [meshpay, design, energie, power]
date: 2026-05-14
status: Phase 1 implémentée 2026-05-14
cible: ESP32-S3 (Waveshare) uniquement
---

# 13 — Gestion de l'énergie (design)

Spec de la feature « gestion de l'énergie » : économie d'énergie + détection de la source d'alimentation, **sur le Waveshare ESP32-S3 uniquement**.

> [!info] Statut
> Design validé le 2026-05-14 via brainstorming. Prochaine étape : plan d'implémentation puis exécution. Phase 1 ici ; le light sleep est reporté en Phase 2 (feature séparée).

## 1. Contexte et motivation

La vision du projet ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/01 - Vision et esprit du projet]]) cible un fonctionnement **sur batterie plusieurs jours** avec une **consommation minimale**. Or aujourd'hui :

- Aucun code de gestion d'énergie n'existe (pas d'ADC batterie, pas de mode sleep, pas de pilotage backlight au-delà de l'init).
- Le prototype est alimenté en USB (`specs.md` §3).
- `esp_pm` n'est utilisé nulle part.

Cette feature pose **l'infrastructure** : une machine d'états ACTIF/ÉCO et une abstraction de la source d'alimentation. Elle est conçue pour rester **inerte tant que le hardware batterie n'est pas câblé** (le stub `hal_power` renvoie toujours « USB »), puis s'activer d'elle-même quand le vrai `hal_power` arrivera.

### Pourquoi S3 uniquement ?

| Device | Rôle | Énergie |
| ------ | ---- | ------- |
| **CYD (ESP32)** | Nœud maître : LoRa, time sync, relay DAG. Doit rester pleinement réactif. Typiquement sur USB (hub fixe). | Pas de gestion d'énergie — `power_manager_stub.c` (no-op). |
| **Waveshare (ESP32-S3)** | Device client transporté, sur batterie. | Machine ACTIF/ÉCO — `power_manager.c` (impl réelle). |

Sélection par CMake (`if(CONFIG_IDF_TARGET_ESP32S3)`) — même pattern facade+stub que `transport_lora` (Lot D.3), ciblé à l'inverse.

## 2. Périmètre

### Dans le périmètre (Phase 1)

- HAL `hal_power` : abstraction de la source d'alimentation, avec impl stub.
- Module `power_manager` : machine à 2 états ACTIF / ÉCO, déclenchée par inactivité.
- État ÉCO Phase 1 : **backlight éteint** + **CPU frequency scaling** via `esp_pm` (sans light sleep).
- Hook pour ralentir la sync LoRa (câblé mais inerte aujourd'hui — le S3 n'a pas encore `lora_sync_task`).
- 6 tests unitaires natifs de la machine d'états.

### Hors périmètre

- **Light sleep** — reporté en Phase 2 (feature séparée). Trop pénible à débugger pendant les tests ; nécessite `CONFIG_FREERTOS_USE_TICKLESS_IDLE` qui risque d'interférer avec le timing UART/LVGL.
- **Monitoring batterie** (jauge %, ADC, alertes batterie faible) — non demandé.
- **Arrêt propre sur batterie critique** (flush NVS + deep sleep) — non demandé.
- **Participation du S3 au DAG via LoRa** — prérequis séparé (voir §9), pas le job de cette feature.
- Vrai `hal_power` câblé sur hardware — fait quand la carte batterie existera.

## 3. Architecture et composants

### A. HAL `hal_power` (composant `device_hal`)

Nouvelle interface HAL, même style que `hal_storage` / `hal_display` / `hal_lora`.

```c
/* components/device_hal/include/hal/hal_power.h */
typedef enum {
    POWER_SOURCE_USB,      /* alimenté en USB / secteur */
    POWER_SOURCE_BATTERY,  /* sur batterie */
    POWER_SOURCE_UNKNOWN,  /* indéterminé — traité comme BATTERY (prudent) */
} hal_power_source_t;

typedef struct {
    hal_power_source_t (*get_source)(void *ctx);
    void *ctx;
} hal_power_t;
```

- `hal_power_stub.c` : `get_source` renvoie toujours `POWER_SOURCE_USB`. Factory `hal_power_stub_create(hal_power_t *out)`.
- Compilé pour **toutes les cibles** (le stub est inoffensif et léger).
- Une vraie impl future lira un GPIO/ADC — non incluse ici.

### B. Module `power_manager` (`main/`)

| Fichier | Cible | Contenu |
| ------- | ----- | ------- |
| `main/power_manager.h` | toutes | API publique (déclarations) |
| `main/power_manager.c` | ESP32-S3 | impl réelle : machine d'états |
| `main/power_manager_stub.c` | ESP32 (CYD) | no-op : `init` ne fait rien, `get_state` renvoie toujours ACTIF |

Sélection CMake dans `main/CMakeLists.txt` :

```cmake
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND meshpay_main_srcs "power_manager.c")
else()
    list(APPEND meshpay_main_srcs "power_manager_stub.c")
endif()
```

**Contrainte d'isolation forte** : `power_manager.c` **n'inclut jamais `app_state.h`** ni aucun header interne de `main/`. Toutes ses dépendances (temps, source d'alim, backlight, config `esp_pm`) passent par le `power_manager_config_t` injecté à l'init. Il ne dépend que de son propre header `power_manager.h` — lequel inclut `<stdint.h>` et `hal/hal_power.h` (pour le type `hal_power_source_t`, header plateforme-pur sans dépendance IDF). → compilable en isolation pour ses tests natifs (cf. §7).

### API publique

```c
/* main/power_manager.h */
typedef enum {
    POWER_STATE_ACTIF,
    POWER_STATE_ECO,
} power_state_t;

typedef struct {
    uint64_t           (*get_time_ms)(void);            /* source de temps monotone */
    hal_power_source_t (*get_power_source)(void);        /* lit hal_power */
    void               (*set_backlight)(uint8_t pct);    /* 0 = éteint, 100 = max */
    void               (*apply_pm_config)(power_state_t state); /* configure esp_pm */
    uint32_t           eco_timeout_ms;                   /* défaut POWER_ECO_TIMEOUT_MS */
} power_manager_config_t;

void          power_manager_init(const power_manager_config_t *cfg);
void          power_manager_notify_activity(void);   /* thread-safe */
void          power_manager_tick(void);              /* appelé périodiquement par core_task */
power_state_t power_manager_get_state(void);
```

### C. Points d'intégration (impl réelle uniquement)

| Point | Rôle |
| ----- | ---- |
| `app_init` / `app_main` | Init `hal_power` (stub) + `power_manager` ; configuration `esp_pm` initiale (état ACTIF) |
| `core_task` | À chaque `comm_event_t` reçu et à chaque `ui_cmd_t` drainé → `power_manager_notify_activity()` ; appelle `power_manager_tick()` à chaque tour de boucle |
| `ui_task` | Sur point tactile `pressed` détecté → `power_manager_notify_activity()` |
| `power_manager` → `hal_display.set_backlight()` | Coupe / rallume le backlight (API HAL déjà existante) |
| `power_manager` → `transport_lora` | Hook `transport_lora_set_sync_interval()` — **inerte aujourd'hui** (pas de `lora_sync_task` sur S3) |

Le `power_manager` ne touche **jamais** à l'état métier (DAG, wallet, locks). Il n'orchestre que backlight + `esp_pm` + intervalle LoRa.

## 4. Machine d'états ACTIF / ÉCO

```
   notify_activity()                          tick() : inactif
   (touch / event réseau / cmd UI)            depuis >= timeout
        │                                          │
        ▼                                          ▼
   ┌─────────┐ ──────── tick() inactif ────▶ ┌─────────────┐
   │  ACTIF  │           >= timeout          │     ÉCO     │
   │         │ ◀──── notify_activity() ───── │             │
   └─────────┘                              └─────────────┘
```

### État ACTIF

= comportement actuel du firmware, rien ne change.

- Backlight : 100
- `esp_pm` : `min_freq = max_freq = 240`, `light_sleep_enable = false`
- Intervalle LoRa : normal

### État ÉCO (Phase 1)

| Effet | Valeur |
| ----- | ------ |
| Backlight | 0 (éteint) |
| `esp_pm` | `min_freq = 80`, `max_freq = 240`, `light_sleep_enable = false` |
| Intervalle LoRa | lent (hook inerte aujourd'hui) |

> Le light sleep (`light_sleep_enable = true` + tickless idle) est **Phase 2**. La machine d'états ne change pas — seul le contenu de `apply_pm_config(POWER_STATE_ECO)` évoluera.

### Transitions

- **ACTIF → ÉCO** : déclenché par `power_manager_tick()` quand `now - last_activity_ms >= eco_timeout_ms`. `tick()` est appelé par `core_task` à chaque tour de boucle (période ~1 s, déjà existante via le timeout du `xQueueReceive`).
- **ÉCO → ACTIF** : déclenché **immédiatement** par `power_manager_notify_activity()`.

### Seuils selon la source d'alimentation

`hal_power.get_source()` est relu **à chaque `tick()`** (pas seulement au boot) — brancher/débrancher l'USB change le comportement à chaud.

| Source | Timeout d'inactivité | Comportement |
| ------ | -------------------- | ------------ |
| `POWER_SOURCE_USB` | **désactivé** (jamais d'ÉCO) | Reste toujours ACTIF — branché, pas besoin d'économiser. Si déjà en ÉCO et l'USB est rebranché → retour ACTIF au prochain `tick()`. |
| `POWER_SOURCE_BATTERY` | `POWER_ECO_TIMEOUT_MS` = **120 000 ms** (2 min) | Passe en ÉCO après 2 min sans interaction |
| `POWER_SOURCE_UNKNOWN` | identique à `BATTERY` | Choix prudent |

> Comme le stub `hal_power` renvoie toujours `USB`, **le device reste toujours ACTIF aujourd'hui**. La machine d'états est inerte tant que le hardware batterie n'est pas câblé — c'est voulu : on livre l'infrastructure testée, elle s'activera d'elle-même.

## 5. Mécanisme `esp_pm` (Phase 1)

Phase 1 = **CPU frequency scaling sans light sleep**. Ne nécessite que `CONFIG_PM_ENABLE`, **pas** `CONFIG_FREERTOS_USE_TICKLESS_IDLE` — donc aucun risque sur le timing UART / LVGL.

### Configuration au boot (`app_init`, toutes cibles)

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 240,           /* plancher haut = pas de scaling */
    .light_sleep_enable = false,
};
esp_pm_configure(&pm);
```

→ comportement identique à aujourd'hui.

### Transition ACTIF → ÉCO (`power_manager.c`, S3 uniquement)

`apply_pm_config(POWER_STATE_ECO)` :

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 80,            /* autorise le scaling vers le bas */
    .light_sleep_enable = false,   /* Phase 1 : pas de light sleep */
};
esp_pm_configure(&pm);
```

### Transition ÉCO → ACTIF

`apply_pm_config(POWER_STATE_ACTIF)` : remet la config « boot » (`min = max = 240`).

### Prérequis sdkconfig

À ajouter dans `sdkconfig.defaults.esp32s3` :

- `CONFIG_PM_ENABLE=y`

> `CONFIG_FREERTOS_USE_TICKLESS_IDLE` et `light_sleep_enable` restent **désactivés** — ce sera la Phase 2.

### Risque résiduel

`esp_pm` + frequency scaling reste raisonnablement sûr (pas de tickless idle). Le `light_sleep_enable` reste `false`, isolé en un seul point (`apply_pm_config`), désactivable instantanément. La Phase 2 (light sleep) sera traitée séparément avec sa propre validation.

## 6. Signaux d'activité

`power_manager_notify_activity()` : une seule fonction, thread-safe (met à jour `last_activity_ms`, protégé par mutex léger ou accès atomique).

| Source | Où | Quand |
| ------ | -- | ----- |
| Touch écran | `ui_task` | À chaque point tactile `pressed` détecté |
| Événement réseau | `core_task` | À chaque `comm_event_t` reçu de `s_evt_queue` |
| Commande UI | — | Couverte **transitivement** par le signal touch : tout `ui_cmd_t` provient d'un callback de bouton LVGL, lui-même déclenché par `lvgl_touch_read_cb` qui appelle déjà `notify_activity()` sur `pressed`. Pas de signal dédié à l'implémentation — il serait redondant. |

`power_manager_tick()` : appelé par `core_task` à chaque tour de boucle (~1 s). Il :

1. Relit `get_power_source()`
2. Calcule `now - last_activity_ms`
3. Décide la transition et applique les effets (backlight, `esp_pm`) **si l'état change**

→ `power_manager` ne crée **aucune tâche FreeRTOS**. Il se greffe sur `core_task` et `ui_task` existantes. Zéro nouvelle stack, zéro nouveau point de synchronisation.

## 7. Tests

Le `power_manager` est conçu pour être **testable en natif** — c'est tout l'intérêt de l'injection de dépendances.

### Stratégie

`power_manager_init()` reçoit un `power_manager_config_t` dont tous les champs sont des callbacks injectables :

| Champ | Réel | Test |
| ----- | ---- | ---- |
| `get_time_ms` | `get_time_ms_wrapper` | horloge simulée |
| `get_power_source` | `hal_power.get_source` | valeur forcée |
| `set_backlight` | `hal_display.set_backlight` | enregistre l'appel |
| `apply_pm_config` | `esp_pm_configure` wrapper | enregistre l'appel |

### Cas de test (6, natifs)

| Test | Vérifie |
| ---- | ------- |
| `usb_never_enters_eco` | Source = USB → reste ACTIF même après 10 min simulées |
| `battery_enters_eco_after_timeout` | Source = BATTERY, pas d'activité 120 s → passe ÉCO, backlight = 0, pm config ÉCO appliquée |
| `activity_returns_to_actif` | En ÉCO → `notify_activity()` → ACTIF immédiat, backlight = 100 |
| `activity_resets_timeout` | Activité à t = 119 s → pas d'ÉCO à t = 120 s |
| `power_source_change_live` | BATTERY → USB pendant l'ÉCO → revient ACTIF au prochain `tick()` |
| `unknown_treated_as_battery` | Source = UNKNOWN → comportement identique à BATTERY |

### Emplacement des tests

`power_manager.c` étant **dépendance-pure** (ne tire ni `app_state.h` ni `esp_pm`), le test est un composant de test dédié dont le `CMakeLists.txt` compile `../main/power_manager.c` + les cas de test, **sans tirer tout `main/`**. `device_hal` (où vit `hal_power_stub.c`) est déjà dans `test_app`.

### Garantie : aucune activation en test

Le mode ÉCO **ne se déclenche jamais** pendant les tests unitaires, pour 3 raisons indépendantes :

1. **`test_app` n'inclut pas le composant `main`** (vérifié dans `test_app/CMakeLists.txt` : `EXTRA_COMPONENT_DIRS` liste les `components/`, pas `main/`). `test_app` a son propre `app_main` (`test_main.c` → `unity_run_menu()`). `power_manager` n'y est ni compilé ni initialisé.
2. Les tests natifs de `power_manager` utilisent des **dépendances injectées** — jamais le vrai `esp_pm` ni le vrai backlight.
3. Même initialisé par erreur, le stub `hal_power` renvoie toujours `USB` → la machine reste ACTIF.

## 8. Constantes et configuration

| Constante | Valeur défaut | Emplacement |
| --------- | ------------- | ----------- |
| `POWER_ECO_TIMEOUT_MS` | `120000` (2 min) | `power_manager.h` |
| `POWER_ACTIF_MIN_FREQ_MHZ` | `240` | `main.c` |
| `POWER_ECO_MIN_FREQ_MHZ` | `80` | `main.c` |
| `POWER_MAX_FREQ_MHZ` | `240` | `main.c` |

> Les 3 constantes de fréquence vivent dans `main.c` (près de l'adaptateur `power_apply_pm_config`), **pas** dans `power_manager.c` : ce dernier est dépendance-pure et ne connaît pas `esp_pm`. C'est `main.c` qui traduit l'état `power_state_t` en config `esp_pm_config_t`.

## 9. Dépendance hors périmètre : participation du S3 au DAG

**Constat vérifié (2026-05-14)** : le Waveshare S3 ne participe **pas** aujourd'hui à la propagation du DAG à l'échelle du mesh.

- Le S3 merge dans son DAG les TX reçues via **ESP-NOW unicast** (quand il est partie directe d'un paiement).
- Mais `transport_lora_init_and_start()` est un no-op sur S3 (`transport_lora_stub.c`) → **`lora_sync_task` n'est jamais créée** → le S3 ne diffuse ni ne reçoit les `LORA_TX` du mesh.
- La propagation DAG mesh-wide passe **uniquement** par `lora_sync_task` (broadcast LoRa périodique). L'ESP-NOW ne fait que de l'unicast émetteur→destinataire.

**Conséquence** : le réveil-sync périodique évoqué pendant le brainstorming suppose que le S3 ait LoRa + `lora_sync_task`. C'est un **prérequis séparé**, à traiter dans sa propre spec. Cette feature énergie pose le **hook** `transport_lora_set_sync_interval()` — câblé mais inerte tant que `lora_sync_task` ne tourne pas sur S3. Quand le S3 aura LoRa, le réveil-sync périodique fonctionnera sans modification du `power_manager`.

## 10. Fichiers créés / modifiés

### Créés

| Fichier | Rôle |
| ------- | ---- |
| `components/device_hal/include/hal/hal_power.h` | Interface HAL source d'alim |
| `components/device_hal/src/hal_power_stub.c` | Impl stub (toujours USB) |
| `main/power_manager.h` | API publique du module |
| `main/power_manager.c` | Impl réelle (ESP32-S3) |
| `main/power_manager_stub.c` | Impl no-op (CYD) |
| `test_app/components/test_power_manager/` (ou équivalent) | Composant de test natif |

### Modifiés

| Fichier | Changement |
| ------- | ---------- |
| `components/device_hal/CMakeLists.txt` | Ajouter `hal_power_stub.c` |
| `main/CMakeLists.txt` | Sélection `power_manager.c` / `power_manager_stub.c` par cible |
| `main/main.c` (`app_main`) | Init `hal_power` + `power_manager` + config `esp_pm` boot |
| `main/core_task.c` | `notify_activity()` sur événement/cmd UI + `tick()` par tour de boucle |
| `components/ui/src/ui_task.c` | `notify_activity()` sur touch |
| `main/transport/transport_lora.{h,c}` + `transport_lora_stub.c` | Ajouter `transport_lora_set_sync_interval()` |
| `sdkconfig.defaults.esp32s3` | `CONFIG_PM_ENABLE=y` |
| `test_app/CMakeLists.txt` | Inclure le composant de test `power_manager` |

## 11. Phasage

| Phase | Contenu | Statut |
| ----- | ------- | ------ |
| **Phase 1** (cette spec) | HAL `hal_power` + stub, `power_manager` ACTIF/ÉCO, backlight off + CPU freq scaling, hook LoRa inerte, 6 tests natifs | À implémenter |
| **Phase 2** (spec séparée) | Activer `light_sleep_enable = true` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` ; validation timing UART/LVGL | Reporté |
| Prérequis (spec séparée) | Faire participer le S3 au DAG (`lora_sync_task` sur S3) — active le réveil-sync périodique | Hors périmètre |

## Liens

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/01 - Vision et esprit du projet]] — la frugalité énergétique comme principe fondateur
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]] — pattern HAL et décomposition `main/`
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/12 - Refactoring main.c (Lot D)]] — pattern facade+stub réutilisé ici (`transport_lora`)
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite]] — l'intervalle de sync LoRa comme levier conso
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]] — logging INFO coûteux en batterie (amélioration connexe possible)
