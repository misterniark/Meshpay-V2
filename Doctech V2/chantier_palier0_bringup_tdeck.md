# Spec — Palier 0 : Bring-up T-Deck Plus (carte fondateur)

> Statut : **spec validée, prête pour plan d'implémentation** · Créé le 2026-06-29
> Type : chantier matériel (`device_hal`), indépendant de la logique monnaie.
> Premier palier du chantier global **« Création de monnaie & rejointe de réseau »**.

---

## 0. Contexte — le chantier global

Feature visée : une **interface de création de monnaie** (règles + onboarding réseau).
Décisions de conception **verrouillées** (brainstorm 2026-06-29) :

- **Gouvernance** : émetteur **unique (fondateur)** — seule autorité MINT ; les membres
  démarrent à 0 (le self-mint / boot credit actuel disparaît).
- **Rejointe** : par **code d'invitation** saisi au pavé, qui ancre la confiance sur un
  **descripteur de monnaie signé** diffusé par radio (et fait office de **voucher**
  d'octroi hors-ligne — approche B).
- **Portée** : **mono-monnaie** par device (aligné sur l'infra mono-DAG actuelle).
- **Règles v1** : nom/symbole, frais + plafond, fonte (demurrage), crédit initial.
- **Fondateur = T-Deck Plus** (clavier physique → saisie texte native) ; membres = wallets
  existants (Waveshare / H752 / CYD).

Découpage en paliers (chacun = sa spec) :

| Palier | Contenu | Dépend de |
|---|---|---|
| **0 — Bring-up T-Deck Plus** *(cette spec)* | reconnaître la carte fondateur dans `device_hal` | — |
| A — Descripteur + persistance + durcissement validation | cœur monnaie runtime | — |
| B — Protocole de rejointe (descripteur radio + code) | adoption membre | A |
| C — Vouchers / crédit initial | octroi hors-ligne | A, B |
| D — UI (wizard fondateur clavier + écran rejointe) | assistant | 0, A, B |

> ⚠️ Cross-dépendance connue : les **vouchers (C)** s'appuient sur le **conflit DAG**
> appliqué au `voucher_nonce` ; le set des nonces réclamés **doit survivre au checkpoint
> élagueur** (cf. `chantier_phase_b_checkpoint.md`). À cabler ensemble le moment venu.

Palier 0 est **indépendant de la logique monnaie** : il peut avancer en parallèle de A,
et se valide seul (la carte boote et tous ses périphériques répondent).

---

## 1. Objectif & périmètre

**Objectif (borné)** : faire **reconnaître le T-Deck Plus par `device_hal`** et **prouver
chaque périphérique** nécessaire au rôle fondateur, **sans encore câbler le wizard UI**
(ça = Palier D).

**Livrable** : la carte boote ; l'écran dessine ; le tactile renvoie des coordonnées ; le
clavier renvoie des touches ; LoRa TX/RX et ESP-NOW fonctionnent — validé par un scénario
`hardware_smoke`.

**Dans le périmètre (IN)** : `KB_POWERON`, écran ST7789, tactile GT911 (réutilisé), clavier
I2C, LoRa SX1262 (réutilisé), ESP-NOW, lecture batterie (ADC).

**Différé (OUT)** : trackball (le tactile couvre la navigation — YAGNI), audio I2S, GPS,
carte SD (sauf son CS, tenu HAUT pendant LoRa), et **tout le wizard UI** (Palier D).

---

## 2. Faits matériels (pinout confirmé)

Sources : variant Meshtastic `variants/esp32s3/t-deck` + `utilities.h` LILYGO.

| Fonction | Pins / valeurs |
|---|---|
| **Power** | `KB_POWERON = 10` (⚠️ HIGH au boot, sinon clavier/tactile/écran muets) · batt ADC = 4 (multiplicateur 2.11) |
| **I2C** (bus partagé clavier + tactile) | SDA = 18, SCL = 8 |
| **Écran ST7789** (TFT LCD, 240×320 portrait / 320×240 paysage) | CS = 12, DC = 11, MOSI = 41, SCK = 40, MISO = 38, RST = -1, BL = 42 |
| **Tactile GT911** | I2C **0x5D**, INT = 16 |
| **Clavier** | I2C **0x55** (géré par un ESP32-C3 secondaire ; lire 1 octet = ASCII de la touche, 0 = rien) |
| **LoRa SX1262** | CS = 9, RST = 17, DIO1 = 45, **BUSY = 13\***, DIO2 = RF-switch interne, **TCXO 1.8 V**, bus SPI partagé (SCK = 40 / MOSI = 41 / MISO = 38) |
| **SD (AUX_CS)** | CS = 39 (tenir HAUT pendant les transactions LoRa) |
| *(différé)* | Trackball UP = 3 / DOWN = 15 / LEFT = 1 / RIGHT = 2 / PRESS = 0 · audio I2S · GPS |

\* **BUSY = 13** selon `utilities.h` LILYGO ; le variant Meshtastic étiquette 13 comme
« DIO2 », ce qui est incohérent avec `DIO2_AS_RF_SWITCH` (DIO2 = switch RF **interne**, non
câblé à un GPIO hôte). BUSY = 13 est donc le plus probable — **à confirmer au banc**.

---

## 3. Intégration dans `device_hal`

### 3.1 Contrat HAL (`meshpay_hal_ops_t`)
- **Réutilisés tels quels** : `display`, `touch`, `lora`, `storage`, `espnow`, `power`.
- **Ajout** : une seule op **`keyboard_read()`** (voir §4). Les cartes sans clavier laissent
  le pointeur à `NULL`.

### 3.2 Fichiers nouveaux
- `components/device_hal/Kconfig` : nouvelle option **`MESHPAY_BOARD_LILYGO_TDECK`** (+ la
  garde dans le `choice MESHPAY_BOARD`).
- `sdkconfig.defaults.tdeck` : board = T-Deck, radio = `ESPNOW_LORA_CORE1262` (profil
  fondateur), défauts `MESHPAY_LORA_C1262_*` aux pins T-Deck + **TCXO 1.8 V**.
- `components/device_hal/device_hal_lilygo_tdeck.c` : implémentation de la carte.

### 3.3 Sous-modules de `device_hal_lilygo_tdeck.c`
1. **Power / boot** : piloter `KB_POWERON = 10` à HIGH **avant toute autre init** ;
   lecture batterie sur ADC 4 (×2.11).
2. **Écran ST7789 SPI** : séquence d'init ST7789, framebuffer RGB565 320×240 (paysage),
   flush. Pattern proche du driver Waveshare existant (LCD SPI).
3. **Tactile GT911** (même puce, même adresse 0x5D que le H752) :
   - le **décodage de trame** est déjà extrait et testé (`meshpay_hal_lilygo_h752_gt911_decode`,
     `device_hal.h:308`, trame 9 o) → **réutilisable**. Le renommer en nom agnostique
     (`meshpay_hal_gt911_decode`) puisqu'il sert désormais deux cartes (petit refactor).
   - le **transport I2C** (lecture de la trame sur le bus) est aujourd'hui **enfoui dans le
     board H752** → à **réimplémenter** pour le bus T-Deck (SDA = 18 / SCL = 8, INT = 16).
   - → la **navigation** marche ensuite avec le mapping tactile → `meshpay_ui_action_t`
     déjà en place.
4. **Clavier** : `keyboard_read()` lit 1 octet sur I2C 0x55.
5. **LoRa SX1262** : **réutiliser `sx126x_hal.c` + `device_hal_lora_core1262.c`** — les pins
   sont **100 % paramétrables par Kconfig** (`MESHPAY_LORA_C1262_PIN_*`), donc la réutilisation
   se résume à poser les défauts : NSS=9, RST=17, BUSY=13, DIO1=45, SCK=40, MOSI=41, MISO=38,
   **RXEN=-1, TXEN=-1** (switch RF interne via DIO2), TCXO 1.8 V. Plus l'**arbitrage CS
   partagé** (tenir SD = 39 et écran = 12 inactifs HAUT pendant LoRa) — pattern `AUX_CS` H752.
6. **ESP-NOW** : natif S3 (`device_hal_espnow.c` existant).

---

## 4. Modèle d'entrée (clavier = saisie, tactile = navigation)

- **Tactile GT911 → navigation** : réutilise le contrat `touch_read` + le driver GT911
  existant. **Aucun changement de contrat** pour la nav.
- **Clavier 0x55 → saisie texte** : **une seule op ajoutée** au HAL :

  ```c
  /* Lit la prochaine touche du clavier I2C (ESP32-C3 @0x55).
   * Retourne le code ASCII de la touche pressée, ou 0 si aucune.
   * NULL sur les cartes sans clavier. */
  esp_err_t (*keyboard_read)(uint8_t *out_ascii);
  ```

  Sa **consommation UI** (champ texte du wizard fondateur) est réalisée au **Palier D**,
  pas ici. En Palier 0, le smoke test prouve seulement que `keyboard_read` renvoie le bon
  ASCII.
- **Trackball : différé** (le tactile couvre la navigation).

---

## 5. Réutilisation (gros gain vs une carte from scratch)

| Élément | Statut |
|---|---|
| Tactile GT911 — décodage trame | **réutilisé** (`..._gt911_decode`, déjà testé ; à renommer agnostique) |
| Tactile GT911 — transport I2C | **nouveau** (lecture trame sur bus T-Deck SDA 18 / SCL 8) |
| LoRa SX1262 | **réutilisé** (`sx126x_hal` + core1262, **pins 100 % Kconfig** : `MESHPAY_LORA_C1262_PIN_*`, RXEN/TXEN = -1) + TCXO 1.8 V + AUX_CS |
| Arbitrage CS bus SPI partagé | **réutilisé** (pattern AUX_CS H752) |
| ESP-NOW | **réutilisé** (natif S3) |
| Écran ST7789 SPI | **nouveau** (proche du Waveshare) |
| `keyboard_read` (0x55) | **nouveau** (op + décodage) |
| Kconfig board + sdkconfig.defaults | **nouveau** |

---

## 6. Tests (TDD + banc) — conforme CLAUDE.md (test unitaire pour toute fonction)

### 6.1 Unitaire (mock HAL, sans matériel)
- **Décodage clavier** : la fonction qui transforme l'octet brut 0x55 en événement
  (touche / vide) est **testable en pur** → tests Unity (`components/device_hal/test/`),
  avec le backend mock (`device_hal_mock.c`) qui simule des octets clavier.
- **Décodage tactile GT911** : déjà couvert par `test_device_hal.c` ; le rename agnostique
  ne doit pas casser ces tests (les garder verts).
- **Sélection de carte** : vérifier que `MESHPAY_BOARD_LILYGO_TDECK` câble bien les bonnes
  ops (display/touch/keyboard non nuls, etc.).

### 6.2 Banc (matériel — `hardware_smoke`)
Nouveau scénario `build-tdeck` dans le manifeste `hardware_smoke`, prouvant successivement :
1. boot + `KB_POWERON` → log « firmware boot ready ».
2. écran : motif de test visible.
3. tactile : un appui renvoie des coordonnées plausibles (log).
4. clavier : une touche renvoie le bon ASCII (log).
5. LoRa : un TX/RX de boucle ou un announce reçu par un pair.
6. ESP-NOW : annonce reçue par un pair.

### 6.3 Critères d'acceptation
Tous les points 6.2 passent sur le matériel + les tests unitaires 6.1 compilent et passent.
Aucun `TEST_IGNORE`, aucune désactivation de test (CLAUDE.md).

---

## 7. Risques / à vérifier au banc

1. **`KB_POWERON = 10` HIGH avant** d'initialiser clavier / tactile / écran (piège n°1 du
   T-Deck — sinon les périphériques ne répondent pas).
2. **BUSY LoRa = 13** à confirmer (divergence LILYGO / Meshtastic, cf. §2).
3. Le **C3 du clavier doit avoir son firmware factory** (sinon I2C 0x55 muet) — vérifier.
4. **SPI partagé** écran / LoRa / SD → CS inactifs tenus HAUT (logique H752 réutilisée).
5. **TCXO 1.8 V** (≠ 2.4 V du H752) → bien poser le défaut Kconfig.
6. **Orientation / init exacte ST7789** (offset colonnes/lignes, ordre RGB/BGR, miroir) — à
   ajuster visuellement au banc.

---

## 8. Hors-périmètre (paliers suivants)

- Wizard fondateur (saisie nom au clavier, réglage des règles) → **Palier D**.
- Descripteur signé + persistance + durcissement MINT → **Palier A**.
- Diffusion descripteur + code d'invitation + adoption membre → **Palier B**.
- Vouchers / crédit initial → **Palier C**.
- Trackball, audio, GPS, carte SD (au-delà du CS) → ultérieur si besoin.

---

## 9. Décisions ouvertes (non bloquantes pour le plan, à lever au banc)

- Valeur exacte du pin **BUSY** LoRa (13 probable).
- Init ST7789 exacte (orientation, offsets, RGB/BGR).
- Présence/firmware du C3 clavier.
- Bus/port I2C exact pour GT911 (partagé avec le clavier sur SDA 18 / SCL 8).
