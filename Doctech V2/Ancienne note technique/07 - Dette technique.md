---
tags:
  - meshpay
  - meshpay
  - meshpay/dette-technique
  - todo
Projets:
  - Mesh Pay
Topics:
  - Dette technique
  - Améliorations
Date: 2026-05-11
---

# Dette technique

> [!abstract] À quoi sert cette note
> **Centraliser** tout ce qui est connu comme incomplet, bancal, ou reporté volontairement. Chaque item a un nom, un statut, une justification, et quand possible un plan.

## Légende des statuts

- 🟢 **Reporté consciemment** — pas urgent, noté pour plus tard
- 🟡 **Limité mais acceptable** — fonctionne pour le prototype, à améliorer avant prod
- 🔴 **Bloquant pour déploiement terrain** — doit être résolu avant vrai usage

---

## 🔴 Clé de signature Secure Boot dans Dropbox (Audit Sonnet item 1)

**Statut** : **reporté consciemment** par décision utilisateur (mai 2026), mais bloquant pour production

**Problème** : `secure_boot_signing_key.pem` (clé RSA privée qui signe le firmware) est présente à la racine du projet, dans un dossier synchronisé Dropbox. Même si `.gitignore` la couvre côté git (fix [[#I6 I7 secrets à la racine du repo|I6/I7]]), elle reste dans Dropbox. Quiconque accède au Dropbox peut signer un firmware accepté par Secure Boot V2 sur tous les appareils flashes avec la clé correspondante.

**Risque** : signature d'un firmware backdooré → déploiement sur l'intégralité du parc.

**Décision** : risque assumé tant qu'on reste en phase prototype. À traiter avant déploiement production.

**À faire (quand) :**
1. Vérifier l'historique git (`git log --all -- secure_boot_signing_key.pem`) — si elle a jamais été committée, considérer tous les appareils flashes compromis et **régénérer la clé**.
2. Sortir la clé du Dropbox et du projet.
3. Stocker hors arbre source sur support chiffré (HSM, coffre, support physique distinct).
4. Référencer via `CONFIG_SECURE_BOOT_SIGNING_KEY` en absolu.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Secure Boot V2]] et [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Reports conscients audit Sonnet]].

---

## 🔴 Flash Encryption en mode DEVELOPMENT (Audit Sonnet item 5)

**Statut** : **reporté consciemment** par décision utilisateur (mai 2026), bloquant pour production

**Problème** : `sdkconfig.defaults` contient `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y`. Le port UART reste actif pour reflasher, et le JTAG accessible — un attaquant avec accès physique peut lire la RAM (clés privées) ou extraire des données via le port série.

**Décision** : pertinent en phase prototype (debug + reflash facile). À basculer **avant production**.

**À faire (quand)** :
1. Créer `sdkconfig.defaults.release` avec `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y` et `CONFIG_SECURE_UART_ROM_DL_MODE_DISABLED=y`.
2. Documenter la procédure de flash production (point de non-retour sur le chip).
3. Bloquer le build CI en mode release sans cette config.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Secure Boot V2]] et [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Reports conscients audit Sonnet]].

---

## 🟢 ~~Conflit GPIO 48 LCD BL / Touch INT~~ → caduc après Lot E.5

L'item d'origine décrivait un conflit imaginaire (GPIO 48 supposé être à la fois LCD BL et Touch IRQ). En réalité, GPIO 48 est **uniquement** le Touch IRQ. Le LCD BL est sur GPIO 46 (jamais en conflit). L'item est donc caduc et a été résolu via le Lot E.5 (BL restauré à 46). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Fix Lot E.5 HAL Display Waveshare vraie cause post-mortem E 3 E 4]].

---

## 🟡 Support multi-SKU Waveshare (31199 vs 31202) via Kconfig

**Statut** : **non bloquant**, à faire si on veut un jour supporter aussi le SKU 31199 sans touch.

**Problème** : le HAL `hal_display_jd9853.c` est verrouillé sur le pinout SKU 31202 (`ESP32-S3-Touch-LCD-1.47`). Le SKU 31199 (`ESP32-S3-LCD-1.47` sans touch) a un pinout LCD **totalement différent** (MOSI=45 vs 39, SCK=40 vs 38, CS=42 vs 21, DC=41 vs 45, RST=39 vs 40) — voir tableau au [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Lot E 4 annule pinout LCD complet bascule vers SKU 31199|Lot E.4 annulé]] pour la comparaison.

**À faire (Lot futur)** :
1. Introduire `CONFIG_BOARD_WS_ESP32S3_LCD_1_47` vs `CONFIG_BOARD_WS_ESP32S3_TOUCH_LCD_1_47` dans `components/device_hal/Kconfig`.
2. Encapsuler tous les `#define JD9853_PIN_*` dans des `#if CONFIG_BOARD_WS_ESP32S3_TOUCH_LCD_1_47 ... #else ... #endif`.
3. Conditionner `i2c_init()` + tout le bloc touch sur `CONFIG_BOARD_WS_ESP32S3_TOUCH_LCD_1_47`.
4. Vérifier au cas par cas la séquence d'init JD9853 (peut différer entre variantes du panneau).

Aucune urgence : le matériel détenu est du SKU 31202.

---

## 🟢 ~~Fiche matériel Obsidian incorrecte~~ → corrigée Lot E.5

La fiche [[Waveshare ESP32-S3 1.47 Touch Display]] a été reprise et corrigée le 2026-05-12 à partir du code source CircuitPython officiel (source testée runtime). Voir l'historique en bas de cette fiche.

---

## 🟢 ~~PSA Ed25519 non compilé dans mbedTLS IDF v5.4.3~~ → résolu par vendor Monocypher (Lot E.2)

**Statut** : **résolu le 12 mai 2026** par le Lot E.2 — refactor du composant crypto vers Monocypher 4.0.2 vendoré.

Item conservé pour traçabilité : le diagnostic initial proposait d'activer un flag PSA mbedTLS, mais l'investigation a montré qu'aucune version de mbedTLS livrée par ESP-IDF (3.6.4 en v5.4.3, 4.0.0 en v6.0) **ne fournit d'implémentation Ed25519** du tout. Les constantes `PSA_ALG_PURE_EDDSA` sont des stubs d'API forward-compat. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Fix Lot E.2 PSA Ed25519 absent de mbedTLS vendor Monocypher APPLIQUÉ]].

**Reste à faire** (transverse, à tracer ailleurs si nécessaire) :
- Valider le fix sur CYD/ESP32 — non testé hardware à ce jour mais le code crypto est partagé entre cibles, aucune raison de divergence
- Ajouter des test vectors RFC 8032 (Annexe A.1) dans `test_crypto.c` pour s'assurer de l'interop avec d'autres implémentations Ed25519 standard (dépend du test runner Unity)

---

## 🔴 Aucun smoke test hardware dans le workflow post-Sonnet

**Statut** : **dette de process critique** révélée par le smoke test 2026-05-12

**Problème** : entre le fix C6 (audit Sonnet, mai 2026) et le 12 mai 2026, **aucun flash hardware** n'a été effectué. Les builds passaient ("Project build complete" répété), mais le firmware ne bootait pas correctement runtime. Deux bugs runtime bloquants sont restés latents :
1. `crypto_init()` jamais appelée → `ESP_ERR_INVALID_STATE` à `[3/12]` ([[#Lot E.0 fix crypto_init non appelé|fix Lot E.0]])
2. PSA Ed25519 non compilé → `PSA_ERROR_NOT_SUPPORTED` à `[3/12]` (voir item PSA Ed25519 ci-dessus)

**À faire** :
- Inscrire dans le workflow Mesh Pay : **après chaque lot d'audit, flash hardware obligatoire** sur au moins une cible (CYD ou Waveshare).
- Lier à la mise en place du test runner Unity ([[#🟡 Tests embarqués non intégrés au runner]]).

---

## 🔴 Console debug / verification DAG sans reset

**Statut** : **bloquant pour debug terrain**, revele par le smoke test Waveshare + Core1262 du 2026-05-17

**Problème** : les commandes debug (`dump_all`, `dump_dag`, checks runtime) existent cote firmware, mais le transport serie n'est pas fiable pour inspecter un device vivant. L'ouverture brute du port USB Serial/JTAG depuis l'environnement de test a declenche `USB_UART_CHIP_RESET`, donc reboot des deux cartes et perte des deux transactions RAM que l'utilisateur venait de creer.

**Risque** : impossible de prouver l'integrite du DAG apres un scenario reel sans modifier l'etat observe. On peut verifier le DAG au boot, mais pas encore l'etat runtime post-transaction sans risque de reset.

**À faire** :
1. Choisir un canal debug qui n'arme pas le reset automatique : UART physique separe, USB CDC configure explicitement, ou canal applicatif admin signe via ESP-NOW/LoRa.
2. Ajouter un audit DAG automatique apres chaque mutation sensible : creation TX, reception TX, ACK, CANCEL, merge LoRa, checkpoint.
3. Conserver un snapshot minimal en NVS ou journal circulaire pour post-mortem apres reset.

Voir [[14 - Audit runtime Waveshare S3 Core1262]].

---

## 🟡 Init LoRa Core1262 fragile apres reset rapide

**Statut** : **a surveiller**, non reproduit comme panne permanente

**Problème** : apres les resets involontaires causes par l'ouverture du port serie, un device a loggue `sx126x_hal: Timeout BUSY (100 ms)` puis `apply_radio_config echoue (rc=-7)`. Avant cela, les cycles LoRa avaient fonctionne sur les deux cartes, donc ce point ressemble davantage a une fragilite d'initialisation apres reset rapide/alimentation qu'a un SX1262 mort.

**À faire** :
1. Ajouter un retry d'init SX1262 avec reset radio explicite si BUSY reste haut.
2. Logger l'etat BUSY/DIO1/NSS/RST autour de `apply_radio_config`.
3. Re-tester apres stabilisation du canal debug non-reset.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/14 - Driver LoRa Core1262 (design)]] et [[14 - Audit runtime Waveshare S3 Core1262]].

---

## 🟡 Refactor `main.c` god object (Audit Sonnet item 6 = Lot D)

**Statut** : **en cours depuis le 2026-05-13**, decompose en 8 lots. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/12 - Refactoring main.c (Lot D)]] pour le plan complet.

**Avancement** :
- ✅ Lot D.1 (etat global + utilitaires) — 2026-05-13. `main.c` 3521 → 2966 lignes.
- ⏳ Lots D.2 a D.8 — persistance NVS, facades transport, handlers, ops, core_task, debug console, app_init.

Le statut redescendra a 🟢 quand la cible (`main.c` ~150 lignes, ~5 `#if` residuels) sera atteinte.

**Probleme historique** : `main/main.c` faisait 106 Ko (3521 lignes) et concentrait :
- Le moteur de paiement (`initiate_payment`, `handle_tx_received`, `handle_ack_received`, `handle_attestation_received`)
- La persistance NVS (`nvs_checkpoint_save/load`, `load_or_generate_keypair`, `load_or_generate_alias`)
- Le protocole applicatif réseau (handlers broadcast/ping/pong/set_alias/set_beneficiary/forward + senders)
- Des wrappers métier (calcul de fonte, helpers Lamport)
- Le point d'entrée FreeRTOS et toute l'orchestration

Risque : chaque évolution du moteur de paiement peut introduire une faille silencieuse, et les invariants (notamment les annotations `[C1-fix]` aux lignes 1969, 2008, 2040, 2053) sont des conventions de mutex fragile non assertées.

**Décision (mai 2026)** : ne pas mélanger ce refactor avec les correctifs de sécurité (lots A/B/C). Une session dédiée extraira `main.c` en trois nouveaux composants :
- **`payment_engine`** : `initiate_payment`, `handle_tx_received`, `handle_ack_received`, `handle_attestation_received`, `check_lock_expirations`, `attempt_beneficiary_forward`
- **`app_storage`** (ou dans `wallet`) : `nvs_checkpoint_save/load`, `load_or_generate_keypair`, `load_or_generate_alias`
- **`mesh_protocol`** (ou dans `comm_protocol`) : tous les handlers `handle_*_received` + senders `broadcast_text_send`, `ping_send`, `set_alias_send`, `set_beneficiary_send`

Préalable : convertir les annotations `[C1-fix]` en assertions `configASSERT(xSemaphoreGetMutexHolder(s_state_mutex) == xTaskGetCurrentTaskHandle())` (debug build) avant l'extraction.

**Effort estimé** : 1 journée complète, par extractions successives validées par `idf.py build` entre chacune.

---

## 🟡 `NONCE_CACHE_SIZE` réduit par contrainte RAM (Audit Sonnet — Lot B)

**Statut** : implémenté à **48 entrées** (au lieu de 128 souhaité)

**Problème** : l'audit recommandait un cache anti-rejeu beaucoup plus large que les 32 entrées initiales. Plan initial à 128, dégradé à 64 puis 48 par contraintes successives de `dram0_seg`.

**Conséquence** : cache couvre ~1 s de mémoire au rate-limit max (50 msg/s) au lieu des ~2.5 s visés. Mitigation **x1.5 l'original** (et non x4 prévu).

**Pour remonter à 128** : libérer ~512 octets de DRAM ailleurs. Pistes : sdkconfig (tailles de stacks IDF, plafonds heap), buffers LVGL, partition layout.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Anti-rejeu]] et [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 2 anti rejeu nonce ESP NOW]].

---

## 🟡 `PENDING_TX_TABLE_SIZE = 1` par contrainte RAM (Audit Sonnet — Lot B)

**Statut** : implémenté à **1 entrée** (au lieu de 8 souhaité)

**Problème** : la table de TX en attente d'ACK dans la couche comm (défense en profondeur pour vérifier que `ack.sender_key == tx.to`) devait initialement contenir 8 entrées. Réduite à 1 pour rester dans la marge DRAM.

**Conséquence** : si un auto-forward déclenche un paiement pendant qu'un paiement manuel est en attente d'ACK, le 1er sera silencieusement écrasé. L'utilisateur le voit en timeout côté UI au bout de 30 s. **Pas une faille de sécurité — juste une perte d'UX**.

**Pour remonter à 4 ou 8** : libérer ~216 / ~504 octets de DRAM ailleurs (cf. dette ci-dessus).

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 3 ACK destinataire verifie dans la couche comm]].

---

## 🟡 DRAM `dram0_seg` saturée

**Statut** : ~340 octets de marge avant les ajouts du Lot B, désormais ~60 octets

**Problème** : la combinaison `lvgl` + `wifi/espnow` + `mbedtls/psa-crypto` + état applicatif (DAG ~116 Ko, wallet, lock_table, time_manager, currency, etc.) sature la DRAM. Chaque nouveau buffer statique de plusieurs centaines d'octets risque de faire déborder le segment.

**À faire** : audit des buffers BSS de chaque composant et de la config IDF. Pistes connues :
- `mbedtls/psa-crypto` peut être configuré plus serré (cf. `mbedtls/esp_config.h`)
- Stacks IDF des tâches Wi-Fi peuvent être réduites
- Le pool LVGL peut être déplacé en PSRAM sur ESP32-S3

**Pourquoi c'est documenté ici** : sans marge, les futurs renforcements de sécurité (cache nonce plus large, table pending TX plus large, monitoring supplémentaire) sont bloqués.

---

## 🔴 Manifeste de monnaie signé (C5)

**Statut** : reporté consciemment, TODO détaillé dans `specs.md`

**Problème** : actuellement `init_currency_config()` est hardcodé en C, et chaque device se désigne lui-même comme seul `mint_authority` au premier boot. Résultat : **pas de réseau partagé possible** — deux devices ne reconnaissent pas la même monnaie.

**Solution prévue** :
- Struct `currency_manifest_t` = `currency_config_t` + `manifest_version` + `manifest_signature` (signée par une "root key" de fondation)
- Persistance NVS d'un blob signé, chargé au boot
- Si absent → nouveau flow UI "setup réseau" (créer ou rejoindre)
- Création : génération d'une root key locale + signature du manifeste
- Rejoindre : réception LoRa d'un manifeste + vérification contre la root (scannée en QR code ou saisie)
- Nouveau message `COMM_MSG_LORA_MANIFEST` pour diffusion aux nouveaux pairs
- Gestion des mises à jour (re-signature + version++)

**Impact estimé** : ~1-2 semaines de dev (struct, serialisation, persistance, UI, message LoRa, tests).

**Pourquoi c'est reporté maintenant** : l'utilisateur a choisi de valider le reste du système d'abord (paiement, UI, batterie, portée LoRa) avant d'ajouter la complexité du manifeste. Décision conservatrice raisonnable.

**Quand l'implémenter** : avant tout déploiement terrain avec plus de 2-3 devices, ou dès qu'on veut un vrai test multi-pair.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite#Manifeste de monnaie signé C5]].

---

## 🟡 Validation du solde émetteur (C2)

**Statut** : implémenté avec limite documentée

**Problème** : quand un device reçoit une TX réseau, il valide le solde de l'émetteur (`rx_tx->from`) à partir de **son propre état local** (checkpoint + DAG). Si l'émetteur a émis d'autres TX qu'on n'a pas encore reçues, on peut sous-estimer ses dépenses et accepter à tort.

**Ce qui a été fait** : `wallet_get_balance_for(pubkey)` calcule le solde correctement depuis checkpoint + DAG post-checkpoint. Utilisé dans `handle_tx_received`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#C2 validation du solde émetteur]].

**Pourquoi ça reste une limite** : on n'a pas le DAG complet du from. La défense en profondeur ne donne qu'une garantie partielle.

**Garanties fortes à la place** :
- **Lock source** côté émetteur (`wallet_lock`) — empêche l'émetteur lui-même de doubler
- **Nonce monotone** (`tx.seq`) — [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone|I3]] détecte deux TX avec même `(from, seq)`

**Amélioration future possible** : protocole de requête de ledger complet d'une pubkey (sur demande). Coûteux en LoRa mais utile pour audit.

---

## 🟡 Tests embarqués non intégrés au runner

**Statut** : fichiers présents, pas compilés automatiquement

**Problème** : les fichiers `test_*.c` existent dans chaque composant (`components/core/*/test/`, `components/comm/*/test/`) mais il n'y a pas de `CMakeLists.txt` dans les sous-dossiers `test/` pour les inclure dans le build.

**Conséquence** : le build principal ne compile pas les tests. Un runner Unity distinct (test_app ESP-IDF) est nécessaire pour les exécuter.

**Ce qui existe déjà** :
- ~53 TEST_CASE dans `test_wallet.c`, `test_transaction.c`, `test_dag.c`, `test_comm_msg_*.c`
- Nouveaux tests I2 : `test_comm_msg_attestation.c` (7 cas)
- Nouveaux tests I3 : 2 TEST_CASE dans `test_dag.c`
- Les tests ont été mis à jour pour la signature de `tx_create_*` avec `seq`

**À faire** :
- Créer un test_app ESP-IDF minimal
- Ajouter les composants test via `idf_component_register` dans chaque CMakeLists
- Documenter la commande de lancement (`idf.py qemu` ou flash sur device réel)

---

## 🟡 Absence d'OTA

**Statut** : prévu, pas commencé

**Problème** : pas de flux de mise à jour du firmware. Un bug critique déployé = flash physique sur chaque device.

**Solution prévue** (dans spec) :
- Image signée par une clé de signature firmware (distincte de la root monétaire)
- Propagation mesh (un device reçoit, vérifie, l'installe, puis le relaie)
- Rollback automatique si nouveau firmware ne boot pas

**Pourquoi pas fait** : non prioritaire tant qu'on itère sur des prototypes.

---

## 🟡 Réconciliation multi-maîtres

**Statut** : non implémenté

**Problème** : deux maîtres peuvent émettre en parallèle et dépasser collectivement `max_supply` sans que le protocole le détecte.

**Plusieurs options** documentées dans [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite#Modèle de réconciliation multi-maîtres]].

**Pourquoi pas fait** : décision architecturale dépendante de C5 (manifeste).

---

## 🟡 Fin de monnaie (prolongation / arrêt anticipé)

**Statut** : `valid_until` dans la struct mais pas de mécanisme de mise à jour

**Problème** :
- Prolonger une monnaie = changer `valid_until` = changer le manifeste = dépend de [[#🔴 Manifeste de monnaie signé C5|C5]]
- Arrêter avant la date = idem, ou nouveau message `COMM_MSG_LORA_CURRENCY_END` signé multi-sig

**Reporté** — dépend de C5.

---

## 🟡 Propagation DAG LoRa fragile — filigrane unique, ni relais ni anti-entropie

**Statut** : limité mais fonctionnel pour un petit réseau ; à durcir avant un test terrain à plusieurs sauts.

**Problème** : la convergence du DAG via LoRa repose entièrement sur la re-diffusion périodique (`lora_sync_do_cycle`, toutes les 2 min). Quatre faiblesses identifiées en lisant `components/comm/lora_sync/src/lora_sync.c` et `main/transport/transport_lora.c` :

1. **Aucun relais explicite.** Un `LORA_TX` reçu est validé puis posté à `core_task` — il n'est jamais ré-émis. Pas de cache `seen` ni de TTL pour les TX (contrairement aux broadcasts et PING). La propagation multi-saut n'existe que comme effet de bord de la re-diffusion périodique.
2. **Filigrane temporel unique.** `lora_collect_confirmed_txs` ne renvoie que les TX `CONFIRMED` dont `timestamp > last_sync_ts`, et `last_sync_ts` ne fait qu'avancer. Une TX qui arrive « en retard » (timestamp ≤ filigrane courant) n'est **jamais re-diffusée** par ce device — fréquent en mode Lamport où le timestamp est un compteur logique. La TX meurt sur ce nœud.
3. **Troncature silencieuse à 32 TX/cycle.** `SYNC_MAX_TX_PER_CYCLE = 32` : si plus de 32 TX dépassent le filigrane, seules 32 partent et le filigrane avance quand même → les TX non envoyées peuvent être définitivement exclues de la sync.
4. **Contexte de réassemblage unique côté récepteur.** Le récepteur n'a qu'un seul `lora_frag_ctx`. Si plusieurs TX fragmentées sont émises dans le même cycle, seule la dernière est réassemblée : l'arrivée des fragments d'une nouvelle séquence (`seq_id` différent) fait abandonner le réassemblage en cours. La fragmentation à l'émission étant désormais active (fix 2026-05-14), ce cas est atteignable en production dès qu'un cycle contient plus d'une TX TRANSFER à 2 parents.

**Conséquence** : sur un réseau de 2-3 devices en visibilité directe, ça converge. Au-delà (multi-saut, paquets perdus, trafic dense), la convergence n'est pas garantie. Aucun mécanisme d'anti-entropie (« envoie-moi ce qui me manque »).

**Pistes** (chantier de conception dédié, pas un simple fix — à brainstormer) :
- Relais des `LORA_TX` avec cache de signatures vues + délai aléatoire, sur le modèle des broadcasts.
- Remplacer le filigrane unique par un suivi par-TX (set de `tx_id` déjà diffusés) ou un vrai protocole d'anti-entropie (échange de résumés de DAG).
- Corriger la troncature : n'avancer le filigrane que jusqu'au plus ancien non envoyé.
- Plusieurs contextes de réassemblage côté récepteur (ou réassemblage indexé par `seq_id`), pour tolérer plusieurs TX fragmentées par cycle.

**Pourquoi pas fait maintenant** : la fragmentation à l'émission (prérequis pour que les grosses TX circulent tout court) a été traitée d'abord (fix 2026-05-14) ; le durcissement du gossip est un chantier à part entière.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] (fix fragmentation LoRa).

---

## 🟢 ESP32-S3 sans LoRa

**Statut** : choix actuel, évolutif

> **Historique** : Avant le 13 mai 2026, la Waveshare ESP32-S3 était aussi privée d'**ESP-NOW** (`#if CONFIG_IDF_TARGET_ESP32` autour de toute la stack). Deux Waveshare ne pouvaient pas se voir pour faire un paiement direct. Corrigé par le commit `60776d8` : ESP-NOW est désormais activé sur les deux cibles, gate `MP_HAS_ESPNOW` (ESP32 + ESP32-S3) ; le canal Wi-Fi est forcé à 1 dans le HAL pour garantir un canal commun entre devices. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#ESP-NOW activé sur Waveshare S3]].

Reste à faire : le Waveshare ESP32-S3 n'a toujours pas de module LoRa connecté. Conséquence :
- Il ne peut pas émettre d'attestation de confirmation en LoRa
- Il ne peut pas recevoir les TX d'autres pairs en LoRa (sync longue portée)
- Les devices ESP32 (CYD) autour de lui prennent le relais pour la propagation réseau
- Entre deux Waveshare seules, le DAG ne converge qu'à travers les TX échangées en ESP-NOW direct

Pour corriger, il faudra brancher un module LoRa (Wio-E5 ou autre) sur les pins disponibles et activer la gate `MP_HAS_LORA` pour la cible S3 (actuellement définie uniquement pour ESP32).

**Pas urgent** : le cas d'usage principal (portefeuille individuel simple, paiement de proximité Waveshare ↔ Waveshare) marche sans LoRa.

---

## 🟢 Lots de fees non répartis

**Statut** : "winner takes all" actuel

Si plusieurs maîtres existent, tous les fees vont **au premier** (`mint_authorities[0]`). Pas de répartition.

**Amélioration future** : mécanisme de répartition proportionnelle ou par round-robin. Dépend d'un consensus entre maîtres (complexe sans infra centralisée).

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Table des décisions validées résumé]] et `wallet_get_balance_for`.

---

## 🟢 PONG non relayé

**Statut** : décision consciente de conception

Les PONG ne sont PAS relayés (contrairement aux PING). Conséquence : un PONG hors portée du maître peut être perdu.

**Pas un bug** : c'est un choix pour éviter la pollution réseau (si chaque device relaie chaque PONG de chaque autre, ça explose).

**Amélioration possible** : relay des PONG avec TTL (time-to-live), comme certains protocoles mesh.

---

## 🟢 Alias avec faible entropie

**Statut** : choix conscient

16 adjectifs × 16 animaux = 256 combinaisons. Dans un réseau > 16 devices, il y aura des collisions d'alias.

**Pas un bug de sécurité** (l'identité cryptographique reste la pubkey). Juste un problème UX si beaucoup de "Brave-Loup" dans le même village.

**Amélioration** : allonger les listes OU ajouter un suffixe numérique en cas de collision détectée.

---

## 🟢 Logs verbeux en production

**Statut** : ESP_LOGI partout

Tous les handlers, réceptions, envois et events sont loggés en INFO. Utile en debug, consomme inutilement en production (batterie + bruit console).

**À faire** : passer certains logs en `ESP_LOGD` et configurer le niveau global à WARN en release.

---

## 🟢 Pas de gestion d'erreur côté UI pour LoRa

**Statut** : silencieux

Si `s_lora_hal.send()` retourne une erreur (ex: module LoRa déconnecté), l'utilisateur ne voit rien dans l'UI. Les logs sont là mais invisibles à l'écran.

**À faire** : feedback UI (icône état LoRa dans la status bar).

---

## 🟢 Pas de backup / restore du wallet

**Statut** : inexistant

Perdre son device = perdre sa clé privée = perdre son solde.

**Amélioration** :
- Export de la clé privée chiffrée par un mot de passe (papier QR / NFC / fichier)
- Import au setup d'un nouveau device

**Sensible** : il faut bien penser à la sécurité du flow (un mauvais backup compromet tout).

---

## Résumé par priorité

| Priorité | Item | Bloquant |
|---|---|---|
| 🟢 | ~~PSA Ed25519~~ → résolu Lot E.2 (Monocypher) | ~~Runtime~~ — désormais opérationnel |
| 🟢 | ~~LCD reste noir Waveshare~~ → résolu Lot E.5 (RST + init + offset + swap) | ~~Affichage~~ — UI désormais visible |
| 🟢 | ~~Touch ne répond pas Waveshare~~ → résolu Lot E.6 (addr 0x63 + reg 0x01 + RST + 100 kHz) | ~~Interaction~~ — tactile opérationnel |
| 🟢 | ~~Reboot toutes les 30 s~~ → résolu Lot E.6 (stack stkmon 2048→4096 + skip auto-monitoring) | ~~Stabilité~~ — firmware stable |
| 🔴 | [[#🔴 Aucun smoke test hardware dans le workflow post Sonnet\|Pas de smoke test hardware]] | Process — laisse passer des régressions runtime |
| 🟡 | [[#🟡 Support multi-SKU Waveshare 31199 vs 31202 via Kconfig\|Support multi-SKU Waveshare]] | Évolution future, non bloquant |
| 🔴 | [[#🔴 Clé de signature Secure Boot dans Dropbox Audit Sonnet item 1\|Clé Secure Boot Dropbox]] | Production |
| 🔴 | [[#🔴 Flash Encryption en mode DEVELOPMENT Audit Sonnet item 5\|Flash Encryption mode DEV]] | Production |
| 🟡 | [[#🟡 Refactor main c god object Audit Sonnet item 6 Lot D\|Refactor main.c (Lot D)]] | En cours (Lot D.1 fait) |
| 🔴 | [[#🔴 Manifeste de monnaie signé C5\|C5 manifeste]] | Déploiement multi-device |
| 🟡 | [[#🟡 NONCE CACHE SIZE réduit par contrainte RAM Audit Sonnet Lot B\|Cache nonce x1.5 au lieu de x4]] | Mitigation rejeu plus faible que prévu |
| 🟡 | [[#🟡 PENDING TX TABLE SIZE 1 par contrainte RAM Audit Sonnet Lot B\|Pending TX = 1]] | UX en cas concurrent rare |
| 🟡 | [[#🟡 DRAM dram0 seg saturée\|DRAM saturée]] | Bloque les futurs renforcements |
| 🟡 | [[#🟡 Validation du solde émetteur C2\|C2 validation réseau]] | Non bloquant (défense supplémentaire) |
| 🟡 | [[#🟡 Tests embarqués non intégrés au runner\|Tests embarqués]] | Qualité, pas fonctionnel |
| 🟡 | [[#🟡 Absence d'OTA\|OTA]] | Déploiement grande échelle |
| 🟡 | [[#🟡 Réconciliation multi-maîtres\|Réconciliation]] | Réseau multi-maîtres en prod |
| 🟡 | [[#🟡 Fin de monnaie prolongation arrêt anticipé\|Fin de monnaie]] | Dépend de C5 |
| 🟡 | [[#🟡 Propagation DAG LoRa fragile filigrane unique ni relais ni anti-entropie\|Gossip LoRa fragile]] | Convergence multi-saut non garantie |
| 🟢 | [[#🟢 ESP32-S3 sans LoRa\|ESP32-S3 sans LoRa]] | Sync longue portée absente |
| 🟢 | [[#🟢 Lots de fees non répartis\|Fees non répartis]] | Amélioration UX |
| 🟢 | [[#🟢 PONG non relayé\|PONG non relayé]] | Cas d'usage rare |
| 🟢 | [[#🟢 Alias avec faible entropie\|Alias collision]] | UX |
| 🟢 | [[#🟢 Logs verbeux en production\|Logs]] | Propreté, conso |
| 🟢 | [[#🟢 Pas de gestion d'erreur côté UI pour LoRa\|UI erreur LoRa]] | UX |
| 🟢 | [[#🟢 Pas de backup restore du wallet\|Backup wallet]] | UX / résilience |

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite]] — décisions pendantes
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] — ce qui a été fixé récemment
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]]

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[auditchatgtp]] — audit code à l'origine de la cartographie de dette
- [[auditusagechatgtp]] — audit UX (manques bloquants UI)
- Concepts cités : [[DAG]], [[LoRa]]
