---
tags:
  - meshpay
  - meshpay
  - meshpay/sécurité
  - tech/crypto
Projets:
  - Mesh Pay
Topics:
  - Sécurité
  - Durcissement
Date: 2026-05-11
---

# Sécurité et durcissement

> [!abstract] À quoi sert cette note
> Cartographier les **couches de sécurité** du firmware. MeshPay manipule de la valeur économique (même modeste) — la sécurité n'est donc pas optionnelle.

## Modèle de menace

| Attaquant | Capacité | Défense |
|---|---|---|
| **Observateur passif (sniffing radio)** | Écoute [[ESP-NOW]] / [[LoRa]] | [[#Authentification cryptographique\|Signatures Mesh Pay]] + intégrité |
| **Attaquant actif (injection radio)** | Forge des messages | Signatures + [[#Anti-rejeu\|anti-rejeu]] + [[#Rate-limiting\|rate-limit]] |
| **Voleur physique (device perdu)** | Accès à la flash | [[#Chiffrement du stockage\|NVS chiffré]] + [[#PIN et anti brute-force\|PIN]] |
| **Utilisateur malveillant avec PIN** | Tente tous les PIN | [[#PIN et anti brute-force\|Blocage progressif + définitif]] |
| **Émetteur malhonnête** | Double-dépense volontaire | [[#Protection contre la double dépense\|Lock source + nonce]] |
| **Tiers relai malveillant** | Altère des TX en transit | Signatures Mesh Pay non altérables |
| **Maître compromis** | Crée de la monnaie sans droit | Vérification clé dans `mint_authorities` |

## Authentification cryptographique

### Profil de signature Mesh Pay

Tous les messages réseau sont signés par leur émetteur avec le profil de signature Mesh Pay.

Décision au 17 mai 2026 :
- profil wire `meshpay-monocypher-4.0.2-ed25519-closed` ;
- provider embarqué : Monocypher 4.0.2 `crypto_ed25519_*` ;
- clés publiques 32 octets, signatures 64 octets ;
- **compatibilité Ed25519/RFC8032 externe non garantie**.

Ce choix est acceptable pour le prototype fermé si tous les devices utilisent exactement le même firmware/lib. Pour un système ouvert ou durable, il faudra migrer vers une implémentation Ed25519 validée contre les vecteurs RFC8032 officiels et versionner le profil wire.

Voir aussi : `docs/superpowers/specs/2026-05-17-crypto-signature-profile.md`.

Cas particulièrement important : **les MINT sont vérifiées** contre la liste `mint_authorities` (via `tx_validate_master`). Un attaquant forgeant une clé aléatoire ne peut pas créer de crédits.

### Vérification à réception

Dans `dag_merge_transaction()` :
1. `tx_validate_structure()` — invariants de base (amount > 0, parents, etc.)
2. `tx_validate_signature()` — hash recalculé + signature Mesh Pay
3. `tx_validate_master()` — pour MINT uniquement, `tx.from` doit être dans `mint_authorities`

Les TX invalides sont rejetées avec `DAG_MERGE_REJECTED`.

### Désérialisation CBOR durcie

- Le champ `status` est **ignoré** à la réception (non signé, forgeable) et forcé à `LOCKED` (TRANSFER) ou `CONFIRMED` (MINT). Sans ça, un attaquant pourrait court-circuiter le mécanisme d'ACK en envoyant une TX directement "CONFIRMED".
- Les champs `amount` et `currency_id` sont vérifiés contre `UINT32_MAX` avant troncature uint64→uint32 (anti-overflow).
- Les clés CBOR inconnues sont ignorées silencieusement (extensibilité).

**Voir aussi** [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Sérialisation CBOR avec clés numériques|Sérialisation CBOR]].

### Attestation signée pour la finalité LoRa (I2)

Depuis avril 2026, le status `CONFIRMED` n'est plus propagé via la TX elle-même (non fiable) mais via un message **`COMM_MSG_LORA_ATTESTATION`** dédié, signé par le destinataire de la TX. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I2 attestation signée LoRa]].

## Protection contre la double dépense

**Trois couches** complémentaires :

### 1. Lock source (principal)

Dès qu'une TX est initiée, le montant est **immédiatement bloqué** sur l'émetteur (`wallet_lock`). Impossible d'émettre une autre TX avec les mêmes fonds tant que le lock est actif.

- Timeout 30 s si aucun ACK → lock libéré + TX annulée
- Implémentation : `components/core/wallet/wallet_lock.*`

### 2. Nonce monotone par émetteur (I3)

Chaque TX porte un `seq` unique croissant chez son émetteur. Deux TX avec même `(from, seq)` mais id différent = **conflit détecté** (`DAG_MERGE_CONFLICT`). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone]].

### 3. Validation du solde côté récepteur (C2, défense en profondeur)

`wallet_get_balance_for()` calcule le solde du `from` depuis l'état local. Si insuffisant → rejet. Limite documentée : on n'a pas forcément vu toutes les TX du `from`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 Validation du solde émetteur C2]].

## PIN et anti brute-force

### Hash du PIN : PBKDF2-HMAC-SHA256

- **Sel aléatoire** généré au setup (stocké à côté du hash)
- **10 000 itérations** (ralentit massivement les attaques offline)
- Paramétrage standard NIST
- Implémenté via mbedTLS `mbedtls_pkcs5_pbkdf2_hmac_ext`

### Blacklist de PIN faibles

`ui_pin.c` refuse au setup :
- Tous identiques (0000, 1111, 9999)
- Séquences évidentes (0123, 4321)
- Patterns répétés
- etc.

### Blocage progressif

| Échecs consécutifs | Sanction |
|---|---|
| 3 | Attente 30 s |
| 5 | Attente 5 min |
| 10 | **Device bloqué définitivement** (factory reset nécessaire) |

Le compteur d'échecs est persisté en NVS chiffré — il survit donc aux reboots. Un attaquant qui redémarre le device ne repart pas à zéro.

**Trade-off connu** : 10 000 itérations PBKDF2 sur 4 chiffres reste faible cryptographiquement. La vraie protection vient du **blocage progressif** et du **chiffrement NVS**. Il faut physiquement voler ET le device ET le PIN correct en <10 essais.

## Chiffrement du stockage

### NVS chiffré (AES-XTS)

- Partition NVS dédiée + partition `nvs_keys` (4 KB, flag `encrypted`)
- Clé de chiffrement AES-XTS dérivée de l'**eFuse du chip** (unique, non extractible)
- Une attaque par dessoudage de la puce flash ne permet pas de lire les données sans aussi extraire l'eFuse (très difficile)

### Isolation de la clé privée

- Stockée uniquement dans NVS chiffré, chargée en RAM au boot
- **Jamais** passée à l'UI (qui ne reçoit que `own_pubkey`)
- Les buffers temporaires sont zeroisés après usage (`memset` explicite)

### Secure Boot V2

> [!warning] Pas activé par défaut
> Nécessite RSA-3072 + eFuses irréversibles. ESP32 requiert ECO3+, ESP32-S3 supporte nativement. Activation = **point de non-retour** sur le chip.

Documenté dans `specs.md`. Procédure en production :
1. Générer la clé RSA-3072 **hors de l'arbre source** (jamais dans le repo)
2. Stocker la clé dans un gestionnaire de secrets / HSM
3. Référencer via `CONFIG_SECURE_BOOT_SIGNING_KEY`
4. Si la clé du repo a été publiée, la considérer compromise et régénérer

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I6 I7 secrets à la racine du repo|I6/I7]].

## Anti-rejeu

### Nonce 32 bits par message signé

Chaque ACK, broadcast, etc. embarque un nonce aléatoire 32 bits. L'appareil récepteur maintient un **cache circulaire FIFO** (48 derniers nonces depuis le Lot B mai 2026 — voir [[#Limites résiduelles audit Sonnet|limites résiduelles]]).

- Si nonce déjà vu → rejeu détecté → message rejeté
- Probabilité de collision aléatoire sur 32 bits : négligeable pour un trafic local
- Fenêtre de mémoire : ~1 s au rate-limit max (50 msg/s global)

**Correction Lot B (mai 2026)** : avant cette session, le cache avait 32 entrées initialisées à zéro via `memset`. Conséquences corrigées :
- Le nonce `0` était systématiquement rejeté dès l'init (faux-positif).
- Eviction documentée à tort comme "LRU" — en réalité FIFO.

Maintenant un compteur `filled` distinct des entrées valides ferme le faux-positif. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 2 anti rejeu nonce ESP NOW]].

Implémentation : `components/comm/espnow/nonce_cache.*`, protégée par `portMUX_TYPE` (ISR-safe).

### Vérification de signature LoRa avant queue (Lot B)

Avant le Lot B, plusieurs types de messages LoRa étaient désérialisés et postés directement dans `evt_queue` **sans vérification de signature** : `LORA_TX`, `LORA_FRAG` (réassemblé), `LORA_BROADCAST`, `LORA_SET_ALIAS`, `LORA_SET_BENEFICIARY`. La signature était vérifiée uniquement par `core_task` ; un attaquant pouvait inonder la queue avec des messages forgés non signés.

Depuis le Lot B, `lora_sync.c` appelle systématiquement avant `xQueueSend` :
- `tx_validate_signature()` pour `LORA_TX` et fragments réassemblés
- Trois nouvelles fonctions `comm_msg_verify_broadcast()`, `comm_msg_verify_set_alias()`, `comm_msg_verify_set_beneficiary()` dans `comm_protocol` (symétrie des `pack_*`)

`core_task` continue d'effectuer ses vérifications d'**autorité** (clé dans `mint_authorities`) et de **cible** (`target_key == ma cle`) — la couche LoRa ne fait que la vérif crypto pure.

### Vérification de l'ACK destinataire dans la couche comm (Lot B)

Avant le Lot B, `espnow.c` ne vérifiait pas que `ack.sender_key == tx.to`. La vérif était déléguée à `core_task` (cf. fix C4 du journal avril). Si `core_task` oubliait ou échouait silencieusement, un tiers connaissant un `tx_id` en circulation pouvait signer son propre ACK et faire confirmer le paiement vers une fausse cible.

Depuis le Lot B, une table `s_pending_tx_table` dans `espnow.c` enregistre les TX envoyées avec leur `expected_signer = tx.to`. À réception d'un ACK, vérification **dans la couche comm** que le `tx_id` est attendu ET que `sender_key == expected_signer`, avant `xQueueSend`. Défense en profondeur : `core_task` continue sa propre vérification via `lock_table`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 3 ACK destinataire verifie dans la couche comm]].

## Rate-limiting

### Par MAC source + global (I5)

**Ajout avril 2026**. Deux niveaux empilés :

| Niveau | Limite | Raison |
|---|---|---|
| Par MAC | 10 msg/s par pair | Un pair bruyant ne bloque plus les autres |
| Global | 50 msg/s total | Filet contre un flood massif (botnet local) |

Table LRU de 8 pairs avec éviction du plus ancien. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I5 rate limit par source]].

### Côté LoRa

Pas de rate-limit explicite côté LoRa (débit naturellement faible par le média). Surveillance possible par les logs.

## Anti-boucle relay

Pour les messages relayés (broadcasts, PING) :
- Cache `seen_broadcasts` de 16 signatures vues récemment
- Si un broadcast avec signature déjà vue → on ne relaie pas
- Délai aléatoire (200-1000 ms) avant relay pour éviter les collisions radio

## Liste des attaques connues mitigées

| # | Attaque | Défense |
|---|---|---|
| 1 | Sniff + répétition (rejeu) | Nonce 32 bits + cache 48 entrées FIFO ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 2 anti rejeu nonce ESP NOW\|Lot B]]) |
| 2 | Forge d'une TX | Signature Ed25519, désormais vérifiée en couche LoRa avant queue ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 4 verification de signature des messages LoRa avant queue\|Lot B]]) |
| 3 | Forge d'une MINT | Vérification `mint_authorities` |
| 4 | Forge d'un ACK | Signature + `sender_key == tx.to` ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#C4 ACK non lié au destinataire\|C4]] + défense en profondeur en couche comm [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 3 ACK destinataire verifie dans la couche comm\|Lot B]]) |
| 5 | Forge d'un PONG | Signature Ed25519 du PONG (I4) |
| 6 | Double-dépense par même émetteur | Lock source + nonce `seq` (I3) |
| 7 | Modification du status en transit | Status ignoré / forcé à la réception |
| 8 | Modification du fee en transit | Fee dans champs signables (couvert par signature) |
| 9 | Flood ESP-NOW | Rate-limit par MAC + global (I5) |
| 10 | Brute-force PIN | PBKDF2 + blocage progressif + chiffrement NVS |
| 11 | Vol physique + dump flash | Chiffrement NVS (AES-XTS par eFuse) |
| 12 | ACK par un tiers | Vérification `sender_key == tx.to` à 2 niveaux (couche comm + core_task) |
| 13 | Overflow amount uint64→uint32 | Check `val > UINT32_MAX` avant troncature |
| 14 | Usurpation d'identité dans scan/rename | PONG signé (I4) |
| 15 | Flood de messages LoRa non signés vers la queue applicative | Vérif signature en couche LoRa avant queue ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#Item 4 verification de signature des messages LoRa avant queue\|Lot B item 4]]) |
| 16 | Cooldown PIN bloqué indéfiniment après reboot | Guard `now < last_fail` traité comme cooldown expiré ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#B4 Cooldown PIN bloque indefiniment apres reboot\|Lot A]]) |

## Limites résiduelles (audit Sonnet, mai 2026)

> [!danger] Reports conscients
> - **Clé de signature Secure Boot dans Dropbox** ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Clé de signature Secure Boot dans Dropbox Audit Sonnet item 1\|item 1 de l'audit Sonnet]]) — quiconque accède au Dropbox peut signer un firmware. À régler **avant production**.
> - **Flash Encryption mode DEVELOPMENT** ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Flash Encryption en mode DEVELOPMENT Audit Sonnet item 5\|item 5]]) — UART/JTAG ouverts. À basculer en RELEASE avant production.

> [!warning] Mitigations partielles (audit Sonnet — contraintes RAM)
> - **Cache nonce à 48 entrées** au lieu des 128 visés. Mitigation x1.5 l'original au lieu de x4. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 NONCE CACHE SIZE réduit par contrainte RAM Audit Sonnet Lot B]].
> - **Table pending TX à 1 entrée** au lieu de 8. Un seul paiement actif à la fois — auto-forward concurrent écrase silencieusement le paiement manuel (UX seulement, pas une faille). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 PENDING TX TABLE SIZE 1 par contrainte RAM Audit Sonnet Lot B]].
> - **DRAM saturée** ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 DRAM dram0 seg saturée]]) — bloque les futurs renforcements de sécurité.

> [!danger] Limites architecturales (préexistantes)
> - Pas de **Secure Boot V2** activé (facile à contourner par reflash si accès physique)
> - Pas d'**OTA signé** (inexistant — voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 Absence d'OTA]])
> - **[[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Manifeste de monnaie signé C5\|Pas de manifeste signé]]** — chaque device est son propre maître par défaut
> - Pas de **multi-sig pour les MINT** (un seul maître suffit à signer)
> - Pas de **révocation de clé** (si un maître est compromis, on ne peut pas le retirer dynamiquement — nécessite C5)
> - **`main.c` god object** ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Refactor main c god object Audit Sonnet item 6 Lot D\|Lot D pending]]) — invariants de sécurité éparpillés sur 3000+ lignes, risque d'introduire des failles silencieuses à chaque évolution.

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques]] — choix cryptographiques
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]] — ce qui reste à sécuriser
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] — derniers renforcements

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[auditchatgtp]] — audit code (origine des fixes sécurité C4, I2, I3, I4, I5)
- [[Mesh Pay specs]] — spec technique 36 sections
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]] — index documentation technique
- Concepts radio cités : [[ESP-NOW]], [[LoRa]]
