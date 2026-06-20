Voici une spécification complète et structurée pour le projet **Mesh Pay intégré nativement avec Reticulum**

Les notes contenu dans le dossier "Ancienne note technique" sont ancienne, les très techniques orientées réseau (**04, 14, 16, 17**) sont obsolètes mais montrent comment le problème était résolu avant pour t'inspirer sur les concepts, mais tu dois ignorer cette implémentation réseau et utiliser nativement les API de Reticulum à la place.


# Spécifications Techniques : Mesh Pay (Reticulum Edition)

## 1. Vision et Objectifs
Mesh Pay est un système de micropaiement hors-ligne, décentralisé, fonctionnant sur des microcontrôleurs ESP32 sur batterie [1, 2]. Il ne dépend d'aucune infrastructure centrale (ni serveur, ni internet, ni banque) [3]. 
La nouveauté de cette architecture "from scratch" est le remplacement des protocoles radio "maison" par le **Reticulum Network Stack (Wire Format)** natif en C [4, 5]. Cela permet un routage multi-sauts résilient, un chiffrement de bout en bout, et une découverte automatique des pairs sur un réseau maillé hybride (ESP-NOW + LoRa) [6-8].

## 2. Matériel et Cibles (Hardware)
Le firmware est construit via **ESP-IDF v5.4+** en C [9, 10].
*   **Cible 1 : "CYD" (Cheap Yellow Display)**. ESP32 classique, écran tactile 2.8" (ILI9341 + XPT2046) [11]. Fait office de terminal complet et de "Transport Node" Reticulum longue portée via module LoRa externe Wio-E5 (UART) ou Core1262 (SPI) [12, 13].
*   **Cible 2 : "Waveshare" ESP32-S3 1.47"**. Écran tactile (JD9853 + AXS5106L) [11, 14, 15]. Fait office de portefeuille de poche autonome (pair) [11, 16].

## 3. Cryptographie et Identités (Base Reticulum)
Le projet utilise strictement les primitives imposées par Reticulum [17, 18] :
*   **Ed25519** pour les signatures [17, 18].
*   **X25519 (ECDH)** pour l'échange de clés éphémères (chiffrement des paquets) [18, 19].
*   **SHA-256** et **SHA-512** pour les hashs et la dérivation [20].
*   **AES-256-CBC** et **HMAC-SHA256** pour l'authentification et le chiffrement des tokens [18].
*   **Bibliothèques C** : Les algorithmes Ed25519 et X25519 seront sourcés via `Monocypher` (vendoré) [21, 22], et AES/SHA via `mbedTLS` natif d'ESP-IDF [17].

### 3.1 Identité et Adressage Reticulum
*   Au premier démarrage, le device génère une **Identity** (clés Ed25519/X25519) persistée en NVS chiffrée [17, 23].
*   Contrairement à l'ancienne architecture, l'adresse réseau n'est plus la clé publique brute. C'est une **Destination Reticulum (16 octets)**, obtenue en tronquant le hash SHA-256 des aspects de l'application et de la clé publique [24].
*   Nom de l'application : `meshpay.wallet`.
*   Alias utilisateur : Généré aléatoirement (ex: "Brave-Loup") [25] et partagé en clair dans la variable `app_data` d'une trame `Announce` Reticulum [26].

## 4. Couche Réseau (Le Wire Format Reticulum)
La stack réseau doit implémenter en C la spécification du format binaire Reticulum (Wire Format) pour permettre au réseau de converger en Mesh [5].

### 4.1 Encapsulation des Paquets
*   Le MTU du réseau est de **500 octets** (header inclus) [27].
*   Structure du paquet binaire (Wire Format) : `[HEADER 2 bytes] [ADDRESSES 16/32 bytes] [CONTEXT 1 byte] [DATA 0-465 bytes]` [5].
*   **ESP-NOW** : Le plafond de 320 octets (V2) [27] doit être géré en fragmentant les trames Reticulum de 500 octets en L2 si nécessaire.
*   **LoRa** : Plafond physique de 255 octets [27]. Les gros paquets (ex: Batchs de transactions) doivent être gérés via l'abstraction `Resource` de Reticulum pour un transfert fiable multi-paquets [28, 29].

### 4.2 Découverte et Paiement
*   **Découverte (Announce)** : Le portefeuille diffuse périodiquement et au boot un paquet `Announce` Reticulum avec son alias dans le payload [26]. Les pairs voisins l'ajoutent à leur UI de paiement.
*   **Paiement Direct (Single Destination)** : Un paiement est un paquet `DATA` ciblant le hash de la `Destination` du receveur. Il est **chiffré asymétriquement en X25519** (secret parfait), contrairement aux vieux "Gossip" en clair [30].

## 5. Le Registre DAG (Ledger)
Mesh Pay n'utilise pas de blockchain, mais un DAG (Graphe Orienté Acyclique) léger optimisé pour la DRAM [31].

### 5.1 Fenêtre Glissante et Checkpoints
*   **RAM Limitée** : La DRAM maintient un maximum de **250 transactions** actives [32, 33].
*   **Checkpoints** : À 80% de remplissage (200 TX), le système calcule les soldes consolidés et sauvegarde un instantané chiffré dans la Flash (`storage` partition via LittleFS ou NVS) [32, 34, 35].
*   **Pruning** : Les transactions consolidées sont purgées du DAG en RAM [32, 36].

### 5.2 Structure de la Transaction (CBOR)
La transaction encodée en **CBOR** constitue la charge utile (DATA) d'un paquet Reticulum [5, 37]. L'utilisation de clés numériques CBOR (1, 2, 3...) compresse la taille [37].
*   `type` (uint8) : TRANSFER (1), MINT (2) [38, 39].
*   `from` & `to` (bytes) : Hashs Reticulum (16 octets) des cibles [24].
*   `amount` (uint32) : Montant brut [40].
*   `seq` (uint32) : Nonce monotone de l'émetteur (anti double-dépense) [41].
*   `fee` (uint32) : Frais de transfert [42].
*   `parents` (array) : Les IDs (hash) de 1 ou 2 transactions parentes (Tips) [43].
*   `signature` (bytes) : 64 octets Ed25519 signant le hash de tous les champs précédents [17].

## 6. Synchronisation et Rattrapage (Gossip v2)
Fini le broadcast aveugle. Mesh Pay s'appuie sur la couche `Request/Response` de Reticulum pour la convergence LoRa [44, 45].
1.  **DAG_SUMMARY** : Périodiquement, chaque noeud envoie en `Broadcast` Reticulum un résumé (Dernier timestamp, compte de TX, 2 tips) [44].
2.  **DAG_REQUEST** : Si un noeud A voit qu'il est en retard par rapport au résumé de B, il lui envoie un paquet `Request` ciblé [45, 46].
3.  **DAG_TX_BATCH** : B répond avec les transactions manquantes compressées via une instance `Resource` Reticulum [29, 46].

## 7. Modèle Monétaire
Chaque réseau gère sa propre monnaie, identifiée par un `currency_id` [47].
*   **Émission** : Modèle de réserve **pré-minée**. Un "Maitre" autorisé initie le réseau, s'alloue la réserve (TX MINT), puis la distribue (TRANSFER) [48, 49].
*   **Frais (Fees)** : Redirigés automatiquement vers le premier "Maitre" (`mint_authorities`) organisant le réseau [50].
*   **Fonte (Demurrage)** : Optionnellement, le solde perd de la valeur avec le temps (ex: -1% / mois). Déclenché lors du calcul local au moment du Checkpoint [51-53].

## 8. Interface Utilisateur (LVGL)
L'UI est écrite avec **LVGL 9.2** et gère dynamiquement les résolutions (320x240 vs 172x320) [54, 55]. L'utilisateur ne voit jamais les hashs cryptographiques, uniquement les Alias [56].
**Écrans principaux :**
*   **Setup** : Définition de l'Alias et du code PIN.
*   **PIN Lock** : Déverrouillage. 4 chiffres. Haché en PBKDF2 en interne. Bloqué 30s après 3 échecs, bloqué définitivement après 10 échecs [57].
*   **Accueil** : Solde actuel (calculé avec la fonte [58]), bouton "Payer", bouton "Recevoir" [59].
*   **Payer** : Scan des pairs Reticulum à proximité, saisie du montant. Feedback via variable atomique de statut (Succès, Échec) [25].

## 9. Multitâche FreeRTOS et Threads
L'architecture logicielle doit séparer le réseau de l'UI pour éviter les blocages.
*   **Mutex Global (`s_state_mutex`)** : Protège l'accès concurrent au DAG et au Wallet [60, 61]. **Attention : Mutex non récursif.**
*   `ui_task` (Prio 4) : Boucle principale LVGL [62].
*   `reticulum_task` (Prio 7) : Pilote les modems LoRa et les trames ESP-NOW, déchiffre les paquets entrants, gère le Wire Format [13, 62].
*   `core_task` (Prio 6) : Moteur de paiement. Valide les transactions reçues (vérification Ed25519, validation solde via `wallet_get_balance_for`, check monotonicité `seq`), ajoute au DAG [41, 62].

## 10. Mécanismes de Sécurité
*   **Double dépense locale** : Un `wallet_lock` gèle le montant d'un paiement dès l'émission. Relâché sur timeout (30s) ou `ACK` Reticulum [63].
*   **Double dépense réseau** : Rejet automatique si conflit sur le tuple `(from, seq)` [41, 64].
*   **Anti-Rejeu (Replay)** : Un cache FIFO interne à la couche réseau stocke les 48 derniers IDs de paquets (ou nonces) pour rejeter les trames rejouées par un attaquant [65, 66].
*   **Chiffrement Stockage** : La NVS doit être chiffrée (AES-XTS lié à un eFuse) pour protéger la clé privée Ed25519 en cas de vol du hardware [17, 67].
