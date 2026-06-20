---
tags:
  - meshpay
  - meshpay
  - glossaire
  - référence
Projets:
  - Mesh Pay
Topics:
  - Concepts
  - Lexique
Date: 2026-04-18
---

# Glossaire et concepts

> [!info] Usage
> Ce lexique est pointé depuis toutes les autres notes. Il définit les termes spécifiques au projet et les concepts techniques non triviaux.

## A

### ACK (Acknowledgement)
Accusé de réception signé envoyé par le destinataire d'une TX TRANSFER via ESP-NOW pour confirmer qu'il a bien reçu et validé la transaction. Format : `[0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Authentification cryptographique|C4 fix]].

### Alias
Nom lisible humain d'un device, généré automatiquement (`<Adjectif>-<Animal>`) ou défini par l'utilisateur. Utilisé dans l'UI à la place de la clé publique. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/05 - Décisions UI#Format de l'alias]].

### Attestation
Nouveau message LoRa (`0x18`) ajouté en avril 2026. Le destinataire d'une TX signe son `tx_id` et diffuse cette preuve pour que le reste du réseau promeuve la TX à `CONFIRMED`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I2 attestation signée LoRa]].

## B

### BPS (basis points)
Unité = 1/10000. Utilisée pour la [[#Fonte demurrage|fonte]] en mode proportionnel. Exemple : `melt_bps = 100` = 1% par tick.

### Broadcast
Message texte signé diffusé par un maître à tous les devices à portée LoRa. Max 157 caractères (signature incluse dans la limite 250 octets LoRa). Usage : "Fermeture à 18h", "Rechargement au stand B".

## C

### CBOR (Concise Binary Object Representation)
RFC 7049, format de sérialisation binaire compact utilisé pour les TX sur le wire. Clés numériques (1, 2, 3…) pour économiser de la place. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Sérialisation CBOR avec clés numériques]].

### Checkpoint
Snapshot des soldes de tous les comptes à un instant T, sauvé en Flash. Permet d'élaguer le DAG sans perdre l'historique des soldes. Structure : `checkpoint_t { accounts[64], timestamp, last_melt_timestamp, ... }`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Checkpoints élagage DAG]].

### Client-only (mode, obsolète)
Ancien mode du Waveshare ESP32-S3 (avant le 13 mai 2026) : UI + stockage local, sans ESP-NOW ni LoRa — le device ne pouvait ni émettre ni recevoir de paiements en direct. **Supprimé** : la S3 a désormais ESP-NOW (gate `MP_HAS_ESPNOW`). Reste seul l'absence de LoRa onboard. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟢 ESP32-S3 sans LoRa]] et [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#ESP-NOW activé sur Waveshare S3]].

### Conflit (DAG_MERGE_CONFLICT)
Situation détectée par `dag_merge_transaction` quand deux TX ont même `from` et même `seq` mais des `id` différents → tentative de double-dépense. La nouvelle TX est rejetée. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone]].

### Currency (currency_config_t)
Struct qui encapsule toutes les règles d'une monnaie : nom, symbole, plafond, fonte, fees, expiration, liste des maîtres. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage]].

### CYD (Cheap Yellow Display)
Carte de développement bon marché (ESP32 + écran tactile 2.8" ILI9341 + touch XPT2046). Board de prototypage principal pour MeshPay. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#CYD Cheap Yellow Display ESP32 classique]].

## D

### DAG (Directed Acyclic Graph)
Graphe orienté acyclique. Structure du registre MeshPay : chaque transaction peut référencer 1 ou 2 parents. Permet le parallélisme (plusieurs TX coexistent) et le merge de sous-graphes (sync LoRa). Implémentation : `components/core/dag/`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Registre DAG plutôt que blockchain]] et la note de concept [[DAG]].

### Demurrage
Voir [[#Fonte demurrage|Fonte]].

### Double-dépense
Tentative d'émettre deux TX qui dépenseraient les mêmes fonds. Mitigée par : lock source, nonce monotone `seq`, validation solde réseau. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Protection contre la double dépense]].

## E

### Ed25519
Algorithme de signature cryptographique asymétrique. Clé 32 octets, signature 64 octets. Utilisé partout dans MeshPay. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Cryptographie Ed25519 SHA 256]].

### eFuse
Mémoire programmable non réinscriptible dans le chip ESP32. Utilisée pour stocker la clé de chiffrement NVS (unique par device, non extractible). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Chiffrement du stockage]].

### ESP-NOW
Protocole de communication sans fil intégré aux ESP32. Latence 1-5 ms, portée ~200 m. Pas besoin d'appairage Wi-Fi. Utilisé pour le paiement direct. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Communication radio hybride]] et la note de concept [[ESP-NOW]].

## F

### Fee (transfer_fee)
Frais de transfert figés au moment de la création de la TX. Stockés dans les champs signables (CBOR_KEY_FEE). Redirigés vers le premier `mint_authority` (non brûlés). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#Les frais de transfert]].

### Fenêtre RAM (sliding window)
250 TX max dans le DAG en RAM à tout instant. Au-delà, `dag_prune_before()` supprime les TX consolidées dans le checkpoint. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Le registre DAG]].

### Fonte (demurrage)
Perte de valeur progressive des soldes avec le temps, pour encourager la circulation. Deux modes : `MELT_MODE_BPS` (proportionnel composé) ou `MELT_MODE_FIXED` (linéaire). Appliquée au checkpoint. Inspiration : théories monétaires de Silvio Gesell. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#La fonte demurrage]].

### FreeRTOS
Système d'exploitation temps réel utilisé par ESP-IDF. MeshPay utilise 3 tâches (`espnow_task`, `core_task`, `lora_task`) + queues + mutex. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Les trois tâches FreeRTOS]].

## H

### HAL (Hardware Abstraction Layer)
Couche d'abstraction entre le hardware et le métier. MeshPay a des HAL pour storage (NVS), display (CYD + Waveshare), LoRa (Wio-E5 UART). Voir `components/device_hal/`.

### Hash
Empreinte numérique d'un contenu. MeshPay utilise SHA-256. Le hash d'une TX (`tx.id`) est calculé sur ses champs signables CBOR.

## L

### Lamport (timestamps)
Compteurs logiques incrémentaux préservant la causalité dans un système distribué sans horloge commune. Mode par défaut de `time_manager`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Timestamps Lamport Master Time optionnel]].

### Lock (wallet_lock)
Verrouillage de fonds côté émetteur dès qu'une TX est initiée, libéré par ACK ou timeout 30 s. Empêche l'émetteur lui-même de doubler. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Protection contre la double dépense]].

### LoRa
Technologie radio basse consommation longue portée. Portée 2 km+, débit faible. Utilisé pour la synchronisation globale. Module actuel : Wio-E5 en UART. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Communication radio hybride]] et la note de concept [[LoRa]].

### LVGL
Light and Versatile Graphics Library. Framework UI embarqué utilisé par MeshPay pour tous ses écrans. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/05 - Décisions UI#Choix de la lib graphique LVGL 9 2]].

## M

### Manifeste (de monnaie)
[[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Manifeste de monnaie signé C5|**Feature non implémentée**]]. Document signé par une "root key" de fondation, contenant tous les paramètres de la monnaie (nom, plafond, maîtres autorisés…) et diffusé aux devices qui rejoignent le réseau. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite#Manifeste de monnaie signé C5]].

### Master (mint_authority)
Device autorisé à créer de la monnaie (`TX_TYPE_MINT`). Sa clé publique figure dans `mint_authorities[]` de la `currency_config_t`. Plusieurs maîtres peuvent coexister. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#Multi maîtres indépendants]].

### Master Time
Mode optionnel où un device de référence diffuse un temps unix en LoRa. Permet l'application déterministe de la fonte et de l'expiration. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Timestamps Lamport Master Time optionnel]].

### Mesh (réseau maillé)
Topologie réseau où chaque nœud est à la fois émetteur, récepteur et relais. Pas de point central. Si un nœud tombe, les autres trouvent un autre chemin. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/01 - Vision et esprit du projet]] et la note de concept [[Mesh]].

### MINT
Type de transaction où un maître crée de nouveaux crédits. Auto-confirmée (`TX_STATUS_CONFIRMED` dès l'insertion). `tx.from` = clé publique du maître. Validée contre `mint_authorities[]`.

## N

### Nonce
1. **Anti-rejeu** : nombre aléatoire 32 bits dans les messages signés (ACK, etc.) pour détecter les messages rejoués. Cache circulaire de 16 nonces par device.
2. **Séquence (seq)** : compteur monotone par émetteur dans la TX, pour détecter les doubles-dépenses. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone]].

### NVS (Non-Volatile Storage)
Partition Flash ESP-IDF pour stocker des paires clé/valeur persistantes. Chiffrée par AES-XTS avec une clé eFuse. Utilisée pour keypair, checkpoint, alias, next_seq, beneficiary. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Chiffrement du stockage]].

## P

### PBKDF2
Password-Based Key Derivation Function 2. Utilisé pour hasher le PIN avec un sel et 10 000 itérations. Rend le brute-force offline lent. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#PIN et anti brute force]].

### Peer (pair)
Device voisin dans le réseau mesh. Les peers ESP-NOW sont découverts par broadcast. Table `s_peers[MAX_PEERS=10]`.

### PIN
Code 4 chiffres protégeant l'accès aux fonctions sensibles. Hashé en PBKDF2, avec blocage progressif. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/05 - Décisions UI#Principe du PIN]] et [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#PIN et anti brute force]].

### PING / PONG
Messages LoRa signés pour scanner le réseau. Un maître envoie un PING, les devices à portée répondent par un PONG (signé depuis I4). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I4 signature des PONG LoRa]].

### PSA Crypto
Platform Security Architecture Crypto API (ARM). Abstraction de primitives cryptographiques fournie par mbedTLS. Utilisée pour Ed25519 et SHA-256. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Cryptographie Ed25519 SHA 256]].

### Prune (dag_prune_before)
Élagage du DAG : suppression des TX avec `timestamp <= before_timestamp`. Appelé après un checkpoint pour libérer de la place. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Le registre DAG]].

## R

### Rate-limit
Limitation du débit de messages reçus. Deux niveaux : par MAC source (10 msg/s) + global (50 msg/s). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I5 rate limit par source]].

### Rejeu (replay)
Réinjection d'un message déjà envoyé. Mitigé par nonce + cache. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Anti rejeu]].

### Root key
[[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Manifeste de monnaie signé C5|**Feature future**]]. Clé privée de la fondation d'un réseau, utilisée pour signer le manifeste et ses mises à jour.

## S

### Secure Boot V2
Mécanisme ESP-IDF garantissant que seul un firmware signé par une clé donnée peut booter. Utilise RSA-3072 et des eFuses irréversibles. Pas activé par défaut. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Secure Boot V2]].

### Seq
Voir [[#Nonce]] (signification 2).

### Sub-graphe (sync LoRa)
Ensemble de TX récentes d'un device diffusées en LoRa toutes les 2 minutes. Les récepteurs les fusionnent dans leur DAG via `dag_merge_transaction`. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Les couches de communication]].

## T

### Tick (fonte)
Unité de temps pour la fonte. Définie par `melt_period_seconds` (défaut 86400 = 1 jour). Un tick écoulé = une application de la formule.

### Tips (du DAG)
Feuilles du DAG (TX non référencées par aucune autre TX). Une nouvelle TX doit référencer 1 ou 2 tips comme parents. Fonction : `dag_get_tips()`.

### TRANSFER
Type de transaction normal : un utilisateur envoie des crédits à un autre. Initial status : `LOCKED`. Passe à `CONFIRMED` après ACK.

### Transaction
Entité de base du DAG. Voir la struct `transaction_t` dans `components/core/transaction/include/transaction/tx_types.h`.

## W

### Waveshare ESP32-S3
Petit hardware 1.47" de MeshPay, format poignet. ESP32-S3 + JD9853 + AXS5106L + PSRAM. Mode **peer** depuis le 13 mai 2026 : paiement direct via ESP-NOW entre deux Waveshare validé. Pas de LoRa onboard, donc dépend des CYD voisins pour la sync réseau longue portée. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Waveshare ESP32-S3 1 47]].

### Wio-E5
Module LoRa / LoRaWAN de Seeed Studio, basé STM32WLE5JC. Interface UART avec protocole AT. Utilisé actuellement comme module LoRa externe. Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Communication radio hybride]].

## Liens vers les notes

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)|Index principal]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/01 - Vision et esprit du projet]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/05 - Décisions UI]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/06 - Choix structurants pour la suite]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement]]

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[Mesh Pay Expliqué]] — vulgarisation grand public
- [[Mesh Pay specs]] — spec technique 36 sections
- [[Mesh Coin]] — paramètres de monnaie
- Notes de concept transverses : [[Mesh]], [[ESP-NOW]], [[LoRa]], [[DAG]]
