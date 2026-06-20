---
tags:
  - meshpay
  - meshpay
  - meshpay/décision
  - meshpay/architecture
  - tech/crypto
  - tech/esp32
Projets:
  - Mesh Pay
Topics:
  - Choix techniques
  - Embarqué
Date: 2026-04-18
---

# Décisions techniques

> [!abstract] À quoi sert cette note
> Les **grands choix techniques** validés avec leur justification. Pour le "pourquoi vs alternatives", voir les callouts `example`. Pour les détails d'implémentation, voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]].

## Registre : [[DAG]] plutôt que blockchain

| Aspect | Blockchain | DAG (choix MeshPay) |
|---|---|---|
| Structure | Chaîne linéaire | Graphe orienté acyclique |
| Parallélisme | ❌ Une TX à la fois | ✅ Plusieurs TX coexistent |
| Empreinte RAM | Lourde | Légère (250 TX × ~233 o = ~60 KB) |
| Consensus | PoW / PoS | Pas nécessaire (signatures + verrouillage) |
| Adapté embarqué | ❌ | ✅ |
| Consommation énergie | Énorme | Minime |

> [!example] Scénario qui justifie le DAG
> Alice paie Bob à 12h00:00 au stand A. Charlie paie David à 12h00:00 au stand B. Les deux opèrent simultanément, hors ligne, et pourtant les deux TX peuvent coexister parfaitement dans le DAG — chacune référence son propre parent local, et les deux branches se rejoignent lors de la prochaine sync LoRa.

Implémentation dans `components/core/dag/`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/10 - Glossaire et concepts#DAG]].

## Communication radio hybride

**Deux couches complémentaires** :

| Couche | Portée | Débit | Latence | Usage |
|---|---|---|---|---|
| **[[ESP-NOW]]** | ~200 m | Wi-Fi-like | 1-5 ms | Paiement instantané direct, ACK, découverte |
| **[[LoRa]]** | ~2 km | kbps | secondes | Synchronisation globale, broadcasts, time sync, attestations |

> [!tip] Pourquoi les deux
> ESP-NOW seul : on perd les devices hors portée → pas de convergence du ledger. LoRa seul : latence incompatible avec un flow "paiement < 1 seconde". Les deux ensemble = le meilleur des deux mondes.

Implémentation : `components/comm/espnow/` et `components/comm/lora_sync/`.

## Cryptographie : Ed25519 + SHA-256

| Primitive | Usage | Implémentation |
|---|---|---|
| **Ed25519** | Signatures TX, ACK, PONG, broadcast, attestation | PSA Crypto (mbedTLS) |
| **SHA-256** | Hash d'identification des TX (`tx.id`) | PSA Crypto |
| **PBKDF2-HMAC-SHA256** | Hash du PIN (10 000 itérations) | mbedTLS |
| **AES-XTS** | Chiffrement NVS (flash encryption native) | HAL ESP32 |

**Pourquoi Ed25519 ?**
- Rapide (signature < 1 ms sur ESP32)
- Compact (clé 32 o, signature 64 o)
- Sécurité moderne (128 bits, resist side-channel raisonnablement)
- Pas de paramètres dangereux à configurer (contrairement à ECDSA)

## Sérialisation : CBOR avec clés numériques

**CBOR** (Concise Binary Object Representation, RFC 7049) a été choisi contre JSON/Protobuf/MsgPack pour :
- Format compact et **binaire** (essentiel face au plafond de taille de paquet : 320 octets en ESP-NOW V2, 255 en LoRa)
- Auto-descriptif (permet d'ignorer des champs inconnus → extensibilité)
- Librairies C légères (tinycbor)
- Éprouvé en IoT (standardisé par l'IETF)

> [!info] Clés CBOR numériques
> Pour gagner de la place, les champs utilisent des clés entières (1, 2, 3…) au lieu de chaînes ("type", "from", "to"). Voir `CBOR_KEY_*` dans `tx_types.h`.

Taille de la struct `transaction_t` en mémoire : ~233 octets. Sérialisée en CBOR, une TX complète peut atteindre **`TX_CBOR_MAX_SIZE` = 320 octets** (cas d'une TRANSFER à 2 parents, ~282 octets) — le projet est passé à **ESP-NOW V2** (plafond 320 o, contre 250 o en V1, Lot E.1bis) pour l'accommoder. Côté **LoRa**, le paquet reste plafonné à 255 octets : une TX dont le CBOR dépasse ~254 octets est donc **fragmentée** en messages `LORA_FRAG` à l'émission (fix 2026-05-14 — voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]]).

Implémentation : `components/core/transaction/src/tx_serialize.c`.

## Anti double-dépense

Trois couches défensives :

### 1. Verrouillage source (principal)

Dès qu'une TX est initiée, le montant est **immédiatement bloqué** sur l'appareil émetteur (`wallet_lock`). Impossible de réutiliser ces fonds tant qu'ils sont lockés — même avant confirmation.

- Timeout 30 s : si aucun ACK, le lock est libéré
- Implémenté dans `components/core/wallet/` (lock_table)

### 2. Nonce monotone par émetteur (I3)

**Ajout récent (avril 2026)** — champ `seq` dans chaque transaction.
- Chaque émetteur incrémente son propre compteur à chaque TX
- Deux TX avec même `(from, seq)` mais id différent → `DAG_MERGE_CONFLICT`
- Persistance NVS pour survivre aux reboots

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone]].

### 3. Validation réseau du solde (défense en profondeur)

Lors de la réception d'une TX, le récepteur calcule le solde de l'émetteur depuis son état local (`wallet_get_balance_for`). Si le solde est insuffisant → rejet.

> [!warning] Garantie limitée
> Le récepteur n'a pas forcément tout le DAG de l'émetteur. Donc cette vérif peut laisser passer une TX en double-dépense si on n'a pas vu les TX précédentes. Ce n'est qu'une **défense en profondeur** — les garanties fortes sont le lock source + le nonce. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#C2 validation du solde émetteur]].

## Persistance : NVS chiffré

- Partition NVS dédiée pour les données sensibles (clés, checkpoints, alias, next_seq)
- Chiffrement AES-XTS via `nvs_sec_provider` (schéma `flash-encryption`)
- Clé de chiffrement dérivée de l'eFuse du chip (unique, non extractible)

**Secure Boot V2** : documenté mais **non activé par défaut** (nécessite RSA-3072 + eFuses irréversibles). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Secure Boot V2]].

## Anti-rejeu (nonce cache)

Chaque message signé contient un **nonce aléatoire 32 bits**. L'appareil récepteur maintient un cache circulaire (16 derniers nonces). Si un nonce est déjà vu → rejet (rejeu).

Implémentation : `components/comm/espnow/nonce_cache.*`.

## Rate-limiting ESP-NOW

**Deux niveaux** (ajout récent I5) :

- **Par MAC source** : max 10 msg/s par pair (table LRU de 8 pairs)
- **Plafond global** : max 50 msg/s total (anti-flood massif)

Avant ce fix, un seul pair bruyant pouvait bloquer les messages de tous les autres. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I5 rate limit par source]].

## Checkpoints + élagage DAG

| Aspect | Valeur | Raison |
|---|---|---|
| Fenêtre RAM | 250 TX max | Tenir dans la DRAM ESP32 avec l'UI |
| Seuil checkpoint | 80% (200 TX) | Laisser de la marge pendant la consolidation |
| Callback save/load | Injectables | Découple le métier du backend (NVS en prod, mémoire en test) |
| Élagage | `dag_prune_before(checkpoint.timestamp)` | Supprime les TX consolidées du DAG |

Déclenchement automatique : quand `dag_needs_checkpoint()` retourne `true` → `checkpoint_create()` → fonte appliquée → save → `dag_prune_before`.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Le registre DAG]].

## Fonte globale au checkpoint

Pour être **déterministe sur tous les devices**, la fonte :
- S'applique uniquement en mode `TIME_MODE_MASTER` (un device de référence diffuse le temps)
- Utilise un seul `last_melt_timestamp` global stocké dans le checkpoint
- Tous les comptes du checkpoint sont fondus ensemble selon les ticks écoulés

Cette discipline garantit que **deux devices qui appliquent la même formule avec le même timestamp convergent vers le même solde**.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#La fonte demurrage]].

## Timestamps : Lamport + Master Time optionnel

Un réseau hors-ligne n'a pas d'horloge absolue. MeshPay supporte deux modes :

| Mode | Description | Cas d'usage |
|---|---|---|
| `TIME_MODE_LAMPORT` | Compteurs logiques incrémentaux, causalité préservée | Par défaut, pas de time master |
| `TIME_MODE_MASTER` | Un device de référence diffuse le temps unix via LoRa | Permet fonte + expiration |

Implémentation : `components/time_manager/`.

## Attestation de confirmation signée (I2)

**Ajout récent (avril 2026)** — nouveau message LoRa `COMM_MSG_LORA_ATTESTATION (0x18)`.

Avant, les TX reçues en LoRa étaient forcées à `TX_STATUS_LOCKED` par durcissement (le status n'étant pas signé, un attaquant pouvait le modifier). Conséquence : les pairs hors portée ESP-NOW ne pouvaient **jamais** voir une TX comme `CONFIRMED`.

Solution : le destinataire d'une TX signe une attestation contenant son tx_id, la diffuse en LoRa. Les récepteurs vérifient `attester_key == tx.to` puis promeuvent la TX à CONFIRMED.

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I2 attestation signée LoRa]].

## Signature Ed25519 pour tous les messages réseau

| Message | Signé par | Signature couvre |
|---|---|---|
| TX LOCKED / MINT | Émetteur | tx.id (hash CBOR des champs signables) |
| ACK ESP-NOW | Destinataire | [nonce:4][tx_id:32] |
| Broadcast maître | Maître | [text_len:1][text:N] |
| PING LoRa | Maître | [ping_id:2] |
| PONG LoRa **(I4)** | Device répondant | [ping_id:2][alias_len:1][alias:N] |
| Attestation **(I2)** | Destinataire de la TX | [tx_id:32] |
| Set alias / set beneficiary | Maître | [target_key][params] |

Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Authentification cryptographique]].

## Table des décisions validées (résumé)

| Décision | Choix |
|---|---|
| Ledger | DAG |
| Paiement | ESP-NOW (~200 m) |
| Sync | LoRa (~2 km, toutes les 2 min) |
| Sérialisation | CBOR (clés numériques) |
| Signatures | Ed25519 |
| Hash | SHA-256 |
| PIN | PBKDF2-HMAC-SHA256, 10 000 itérations |
| Stockage | NVS chiffré (AES-XTS) |
| Timestamps | Lamport par défaut, Master Time optionnel |
| Fenêtre RAM | 250 TX |
| Seuil checkpoint | 80% (200 TX) |
| Rate-limit ESP-NOW | 10 msg/s/MAC + 50 msg/s global |
| Découverte peers | Broadcast ESP-NOW |
| Fragmentation LoRa | Hybride : 1 paquet `LORA_TX` si CBOR ≤ 255 o, sinon découpe en `LORA_FRAG`. Émission **et** réception câblées depuis le fix 2026-05-14 |
| Fees | Redirigés au 1er mint_authority |
| Anti double-dépense | Lock source + nonce monotone + validation solde |
| Finalité LoRa | Attestation signée |

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]] — vue d'ensemble
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement]] — couches de protection
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] — les améliorations d'avril 2026

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[Mesh Pay specs]] — spec technique 36 sections
- [[auditchatgtp]] — audit code à l'origine des fixes C1-C6 / I1-I7
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]] — index documentation technique
- Concepts : [[Mesh]], [[ESP-NOW]], [[LoRa]], [[DAG]]
