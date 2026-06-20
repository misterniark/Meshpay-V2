# Plan de développement MeshPayV2 - Reticulum Edition

## Résumé

Créer un nouveau projet ESP-IDF v5.4+ dans `MeshPayV2`, en partant d'un arbre propre. Le premier chantier est un port C interopérable par paliers de Reticulum : wire format, identité, destination, announce, paquets chiffrés, transport ESP-NOW/LoRa, puis Link/Request/Resource pour la synchronisation DAG.

Chaque composant livré doit avoir au moins un test Unity dans `components/<composant>/test/`, plus des fixtures de compatibilité générées depuis Reticulum Python pour les composants Reticulum.

## État actuel

- ✅ `project_skeleton` : squelette ESP-IDF, `main/`, `components/`, `test_app/`, wrapper `scripts/idf.sh`, build ESP32-S3 validé.
- ✅ `rns_fixtures` : schéma de fixtures, manifeste bootstrap, générateur Python, composant C et test Unity compilé.
- ✅ `rns_crypto` : wrappers SHA-256/SHA-512/HMAC-SHA256/PBKDF2/AES-256-CBC via mbedTLS, Ed25519/X25519 via Monocypher vendoré, RNG injectable, tests Unity compilés.
- ✅ X25519 défensif : `rns_crypto` rejette les clés X25519 entièrement nulles et les secrets partagés nuls; test Unity compilé.
- ✅ `rns_identity` : identité Reticulum 64 octets privé/public, hash 16 octets, signature/vérification, secret partagé X25519, tests Unity compilés.
- ✅ Identités défensives : `rns_identity` rejette les clés privées/publiques entièrement nulles ou avec moitié X25519/Ed25519 nulle avant dérivation ou chargement; test Unity compilé.
- ✅ `rns_destination` : noms dotted Reticulum, name hash 80 bits, Destination hash 16 octets Single/Plain/Link, helper `meshpay.wallet`, tests Unity compilés.
- ✅ `rns_packet` : pack/unpack wire Reticulum MTU 500, header type 1/2, contexte, hop count, ordre type 2 `transport_id` puis `destination_hash`, tests Unity compilés.
- ✅ Contextes Reticulum officiels : `rns_packet` aligne `REQUEST=0x09`, `RESPONSE=0x0a`, `LINKPROOF/LRRTT/LRPROOF=0xfd/0xfe/0xff` et les contextes Resource manquants sur Reticulum Python 1.2.9; test Unity de garde compilé.
- ✅ MDU Reticulum officiel : `rns_packet` expose `RNS_PACKET_MDU=464` et limite `RNS_PACKET_MAX_DATA_SIZE` à cette valeur pour rester aligné avec Reticulum Python; test Unity de garde compilé.
- ✅ `rns_packet_crypto` : token DATA Single Reticulum X25519 éphémère, HKDF-SHA256, AES-256-CBC/PKCS7, HMAC-SHA256, limite plaintext 383 octets, fixture déterministe compilée.
- ✅ `rns_announce` : encode/decode/verify announce Reticulum sans ratchet, alias `app_data`, validation signature/destination hash, table `known_destinations`, tests Unity compilés.
- ✅ Announce défensif : rejet des annonces émises ou vérifiées avec `random_hash` entièrement nul; test Unity compilé.
- ✅ `rns_transport_core` : cache anti-rejeu 48 IDs, destinations locales, callbacks RX/forward, table de chemins depuis announces valides, hop increment au forward, tests Unity compilés.
- ✅ Transport défensif : `rns_transport_core` refuse l'enregistrement d'une destination locale entièrement nulle; test Unity compilé.
- ✅ `rns_iface_espnow` : fragmentation L2 ESP-NOW paramétrable, frame max 320 octets, réassemblage hors ordre, rejet doublons contradictoires, tests Unity compilés.
- ✅ `rns_iface_lora` : fragmentation LoRa 255 octets, réassemblage hors ordre, init demi-duplex avec retries et timeout, tests Unity compilés.
- ✅ `rns_link_request` : LINKREQUEST avec signalling MTU/mode, `link_id` Reticulum, proof LRPROOF signé par l'identité destination, handshake A/B simulé, tests Unity compilés.
- ✅ Link Request défensif : rejet des LINKREQUEST non-Single et des signalling MTU/mode hors support local avant handshake actif; test Unity compilé.
- ✅ `rns_request_response` : requêtes ciblées sur Link, `path_hash`, `request_id` par hash paquet, réponse corrélée, rejet mauvaise corrélation, timeout logique, tests Unity compilés.
- ✅ Request/Response défensif : les requêtes refusent un Link actif sans `link_id` non nul; test Unity compilé.
- ✅ Timeouts monotones robustes : `rns_request_response` ignore un `now_ms` inférieur au timestamp d'envoi au lieu de déclencher une expiration par underflow; test Unity compilé.
- ✅ `rns_resource` : Resource minimal pour batch DAG, fragments `RESOURCE`, ID/checksum SHA-256, réassemblage hors ordre, corruption rejetée, tests Unity compilés.
- ✅ Resource défensif : la création de fragments refuse un Link actif sans `link_id` non nul; test Unity compilé.
- ✅ `storage` : backend mock NVS-like, identité/alias/PIN hash/seq/checkpoint, checksum SHA-256 checkpoint, save/load/erase idempotent, tests Unity compilés.
- ✅ Persistance défensive : `storage` rejette les records persistés dont les flags identité/PIN/checkpoint sont incohérents avant save/load, ainsi que les identités nulles ou partielles et les hash PIN nuls; test Unity compilé.
- ✅ `meshpay_tx` : transactions `TRANSFER`/`MINT`, CBOR compact à clés numériques, hash signable SHA-256, signature Ed25519 du hash, encode/decode/verify, tests Unity compilés.
- ✅ `dag` : fenêtre 250 TX, merge validé, doublons, conflits `(from, seq)`, parents manquants, tips triés, seuil checkpoint 200, tests Unity compilés.
- ✅ DAG défensif : `dag` refuse les transactions dont la signature est entièrement nulle avant insertion; test Unity compilé.
- ✅ `currency` : config monnaie, autorités de MINT, validation supply/fee/solde, fees vers `mint_authorities[0]`, fonte BPS, tests Unity compilés.
- ✅ Configuration monnaie idempotente : réajouter une autorité de mint existante reste `ESP_OK` même lorsque la table est pleine; test Unity compilé.
- ✅ `wallet` : solde disponible via `currency`, verrou local 30 s, libération par TX id, `next_seq`, hash PIN, verrouillage après 3 échecs, tests Unity compilés.
- ✅ Timeout wallet monotone : le verrou local reste actif si l'horloge observée recule sous `lock_started_ms`, évitant une libération prématurée; test Unity compilé.
- ✅ `payment_engine` : création paiement, TX CBOR signée, paquet Reticulum DATA, réception/validation, ACK par TX id, finalisation et déverrouillage, tests Unity compilés.
- ✅ Timeout paiement : `payment_engine` expire maintenant un pending après le timeout wallet de 30 s, libère le lock sans réutiliser l'ancienne séquence, puis autorise un nouveau paiement; tests Unity compilés et build sécurisé validé.
- ✅ Paiement chiffré : `payment_engine` peut créer un paiement DATA chiffré via `rns_packet_crypto` avec l'identité publique du destinataire, et la réception tente le déchiffrement local avant validation; test Unity compilé.
- ✅ Réception paiement chiffré runtime : `app_main` chiffre automatiquement les paiements quand l'annonce destination est connue, le chemin RX applicatif accepte DATA plaintext ou chiffré, réémet l'ACK et conserve le montant déchiffré pour le feedback UI; tests Unity compilés.
- ✅ Vérification sender connu : `payment_engine` vérifie la signature de transaction quand le sender est connu via ANNOUNCE, et rejette un montant altéré après signature; tests Unity compilés et build sécurisé validé.
- ✅ `dag_sync` : summary broadcast, request par compteur connu, batch DAG via Resource Reticulum, application des TX manquantes, test de rattrapage compilé.
- ✅ DAG Sync défensif : rejet des source/peer entièrement nuls dans les summary/request pour éviter un routage ambigu; test Unity compilé.
- ✅ `dag_sync` runtime : `app_main` traite SUMMARY/REQUEST/RESOURCE, émet une requête avec source demandeur, renvoie un batch Resource vers ce demandeur, réassemble et merge les transactions manquantes; test d'intégration runtime compilé.
- ✅ Broadcast DAG réel : `rns_node` livre désormais les DATA `Plain` broadcast au callback applicatif tout en les forwardant, et `app_main` sait émettre un `DAG_SUMMARY` depuis la queue core; tests Unity compilés.
- ✅ Requêtes DAG Reticulum : les `DAG_REQUEST` utilisent maintenant le contexte Reticulum `REQUEST`, et le firmware branche le callback request de `rns_node` vers le runtime; tests Unity compilés.
- ✅ Request/Response DAG : les `DAG_REQUEST` sont maintenant créées via `rns_request_response` sur pseudo-link Reticulum, avec parsing compatible ancien payload MeshPay et réponse batch via `Resource`; builds root/test/sécurisé validés.
- ✅ `device_hal` : contrats display/touch/storage/LoRa/ESP-NOW/power, boards CYD/Waveshare/LilyGo, backend mock et tests Unity compilés.
- ✅ `ui` : logique Setup PIN/Home/Pay/Receive/History/Network/Locked, feedback paiement/PIN, état solde/réseau, tests Unity compilés.
- ✅ UI utilisateur portable : modèle de vue présentable par écran, actions principales, saisie PIN masquée, saisie montant paiement, activation confirmation et libellés utilisateur testés sans LVGL; `test_app` et firmware racine compilés.
- ✅ `app_main` : orchestration simulée boot/announce/paiement, constantes `ui_task`/`reticulum_task`/`core_task`, compteurs de queues, test d'intégration simulé compilé.
- ✅ Intégration Reticulum mémoire : test Unity reliant deux `rns_node` par callbacks TX/RX, annonces réelles, paiement chiffré via announce, ACK retour et feedback UI sans matériel; `test_app` compilé.
- ✅ `hardware_smoke` : manifeste testable des scénarios build/flash/monitor/announce/paiement/sync, script de banc avec garde-fou flash, procédure manuelle documentée, tests Unity compilés.
- ✅ Premier jalon simulé : deux noeuds peuvent s'annoncer, échanger une transaction signée/chiffrée via paquet Reticulum DATA, confirmer l'ACK, puis rattraper un batch DAG dans les tests d'intégration.
- ✅ `rns_node` : façade haute niveau ajoutée après le jalon initial (`rns_node_init`, announce, send, poll wire, callbacks RX/proof/request, stats), tests Unity compilés.
- ✅ Découverte announce runtime : les ANNOUNCE distants sont livrés au callback RX applicatif après mise à jour transport, et `app_main` synchronise `ui.network_peers` depuis `known_destinations`; tests Unity compilés.
- ✅ Firmware root : `main/app_main.c` initialise une identité, une destination `meshpay.wallet`, un `rns_node`, émet un announce local vers callback de log, build ESP32-S3 validé.
- ✅ Firmware radio ESP-NOW : `app_main` tente l'init ESP-NOW, branche `rns_node` sur `rns_radio`, démarre `radio_task` de polling RX et conserve un fallback log/local si la radio échoue au boot.
- ✅ Runtime applicatif : mutex FreeRTOS non récursif, queues `ui`/`reticulum`/`core`, tâches nommées, traitement annonce/UI/paiement entrant-sortant, tests Unity compilés et firmware root raccordé.
- ✅ ACK/PROOF runtime : le firmware branche le callback Reticulum `proof` vers le runtime, et les ACK de paiement sont acceptés en paquets DATA ou PROOF; tests Unity compilés et build sécurisé validé.
- ✅ Persistance runtime `next_seq` : après création d'un paiement, `app_main` sauvegarde la séquence wallet dans le record storage avant émission; si la sauvegarde échoue, le pending est annulé et la séquence est restaurée; tests Unity compilés.
- ✅ Séquences wallet robustes : `payment_engine` restaure `next_seq` quand une création de paiement est rejetée avant émission, évitant les trous et réutilisations ambiguës; test Unity compilé.
- ✅ Hook TX applicatif : les paquets produits par `core_task` sont remis à `reticulum_task`, puis envoyés via callback injectable; le firmware racine branche ce callback sur `rns_node_send_packet`, test Unity compilé.
- ✅ Hook RX applicatif : les paquets locaux livrés par `rns_node` alimentent la queue Reticulum du runtime; les paiements entrants extraient le montant CBOR et les ACK entrants finalisent/déverrouillent le paiement, tests Unity compilés.
- ✅ ACK paiement runtime : un paiement entrant validé réémet maintenant l'ACK produit par `payment_engine` via le hook TX Reticulum; test Unity ajouté pour garantir l'émission de l'ACK.
- ✅ Persistance boot : backend NVS pour `storage`, bootstrap identité load-or-create, alias/next_seq sauvegardés, fallback identité volatile si NVS indisponible, tests mock compilés et firmware root raccordé.
- ✅ PIN persistant : le wallet peut recharger un hash PIN stocké, et le boot firmware initialise l'UI en mode configuré quand le record NVS contient `has_pin_hash`; tests Unity compilés.
- ✅ Robustesse NVS : `meshpay_storage_nvs_init` récupère `NO_FREE_PAGES`/`NEW_VERSION_FOUND` par erase + reinit, tout en laissant `nvs_flash_init()` gérer l'encryption NVS ESP-IDF; tests injectés compilés et build sécurisé validé.
- ✅ Pont radio Reticulum : composant `rns_radio`, fragmentation TX ESP-NOW/LoRa via HAL, réassemblage RX vers `rns_node_poll`, timeout idle, tests Unity compilés.
- ✅ Adaptateur node/radio : `rns_radio_bind_node` connecte le TX de `rns_node` au pont radio tout en préservant les callbacks applicatifs TX/RX/proof/request, test Unity compilé.
- ✅ HAL ESP-NOW réel : driver `device_hal` pour ESP-NOW broadcast par défaut, init Wi-Fi STA, peer ESP-NOW, queue RX FreeRTOS, send/recv via contrat HAL, test de configuration sûr compilé.
- ✅ HAL LoRa UART réel : driver `device_hal` raw/framed UART pour modules LoRa série, framing `MP` + longueur + CRC16-CCITT, send/recv via contrat HAL, tests de configuration et roundtrip/corruption de trame compilés.
- ✅ HAL Waveshare display/touch : driver `device_hal` pour `JD9853` SPI + `AXS5106L` I2C, pins Lot E.5/E.6, init manufacturer, offset `Y=34`, MADCTL paysage `0x60`, conversion RGB565 big-endian, décodage tactile `0x63/0x01` et tests Unity hors hardware compilés.
- ✅ HAL LilyGo H752 display/touch : driver `device_hal` pour `ED047TC1` 960x540 via bus parallèle I80/`esp_lcd` + driver LilyGo, framebuffer e-paper 4 bpp, TPS65185 `0x6B` VCOM -2.0 V, GT911 `0x5D/0x14` + fallback CST `0x5A`, transform tactile `x=raw_y`/`y=540-raw_x`, tests Unity hors hardware et build H752 compilés.
- ✅ Configuration radio : Kconfig `MESHPAY_RADIO_ESPNOW`/`MESHPAY_RADIO_LORA_UART`/`MESHPAY_RADIO_DISABLED`, choix board, canal ESP-NOW et paramètres UART LoRa; `app_main` sélectionne et logge le backend au boot.
- ✅ Profils cible : defaults ESP32-S3 orientés Waveshare + ESP-NOW, defaults H752 orientés LilyGo e-paper sans radio matérielle, defaults ESP32 orientés CYD + LoRa UART, builds smoke frais validés pour les profils sans flash.
- ✅ Fixtures Reticulum étendues : catalogue versionné schema/name hash/destination hash/packet raw/announce/token chiffré, générateur Python avec fallback local-port déterministe, test Unity compilé.
- ✅ Génération fixtures : `scripts/generate_rns_fixtures.py` régénère `fixtures/rns/manifest.json` et fournit un mode `--check` pour verrouiller les vecteurs versionnés.
- ✅ Générateur fixtures canonique : `tools/rns_fixtures/generate.py` devient la source unique, le wrapper `scripts/generate_rns_fixtures.py` conserve `--output/--check`, et `--verify-reticulum` valide les constantes de paquet contre le paquet officiel quand il est disponible.
- ✅ Profil smoke sécurisé : partition `nvs_key` chiffrée, flash encryption development, NVS encryption, build ESP32-S3 sécurisé validé, `sdkconfig` de banc isolé du `sdkconfig` racine.
- ✅ Couverture radio callbacks : l'adaptateur `rns_radio_bind_node` a maintenant un test explicite garantissant la traversée des callbacks `proof` et `request`, en plus du TX/RX.
- ✅ Validation hors matériel : `test_app`, firmware racine, fixtures Reticulum `--check/--verify-reticulum`, builds smoke S3, CYD et S3 sécurisé recompilés sans flash après les derniers durcissements.

## Interfaces clés

- `rns_crypto` : Ed25519/X25519 via Monocypher, SHA/AES/HMAC/PBKDF2 via mbedTLS.
- `rns_identity` : génération, sérialisation, chargement, signature, vérification.
- `rns_destination` : hash 16 octets Reticulum pour `meshpay.wallet`, types Single/Plain/Link.
- `rns_packet` : pack/unpack wire format `[HEADER][ADDR][CONTEXT][DATA]`, MTU 500, contextes Reticulum.
- `rns_node` : API haute niveau `rns_node_init`, `rns_announce`, `rns_send`, `rns_poll`, callbacks RX/proof/request.
- `meshpay_tx` : transactions CBOR signées, champs numériques, `TRANSFER`/`MINT`.
- `payment_engine` : verrouillage local, émission, validation, ACK/proof, feedback UI.
- `dag_sync` : `DAG_SUMMARY`, `DAG_REQUEST`, `DAG_TX_BATCH` via Request/Response/Resource.

## Ordre de développement par composant

| Ordre | Composant | Travail | Test unitaire obligatoire |
|---:|---|---|---|
| 0 | `project_skeleton` | Créer projet ESP-IDF, `components/`, `main/`, `test_app/`, CMake, sdkconfig par cible. | Build minimal + test Unity "bootstraps test runner". |
| 1 | `rns_fixtures` | Script Python générant vecteurs Reticulum depuis l'implémentation officielle : destination hash, packet raw, announce, token chiffré. | Vérifier que les fixtures sont lisibles et versionnées. |
| 2 | `rns_crypto` | Porter les primitives nécessaires : hash, HMAC, AES-CBC, Ed25519, X25519, RNG injectable. | Vecteurs crypto fixes + roundtrip encrypt/decrypt. |
| 3 | `rns_identity` | Keyset Reticulum 64 octets, identité persistable, signature/proof, public key concaténée. | Génération, sérialisation, signature valide/invalide. |
| 4 | `rns_destination` | Dotted aspects, hash SHA-256 tronqué 128 bits, Single destination `meshpay.wallet`. | Hash conforme aux fixtures Python. |
| 5 | `rns_packet` | Constantes DATA/ANNOUNCE/PROOF, header type 1/2, context flag, hop count, pack/unpack MTU 500. | Pack/unpack bit-exact contre fixtures. |
| 6 | `rns_packet_crypto` | Chiffrement des DATA Single : clé éphémère X25519, AES-256-CBC, HMAC-SHA256. | Déchiffrage local + fixture Python. |
| 7 | `rns_announce` | Encode/decode/verify announce, `app_data` alias, table `known_destinations`. | Announce signé accepté, altéré rejeté. |
| 8 | `rns_transport_core` | Tables de chemins, cache anti-rejeu 48 IDs, callbacks RX, routage local/broadcast. | Duplicate drop, path update, hop increment. |
| 9 | `rns_iface_espnow` | Interface ESP-NOW Reticulum, fragmentation L2 si paquet > payload ESP-NOW utile. | Fragmentation/réassemblage hors hardware. |
| 10 | `rns_iface_lora` | Interface LoRa Core1262/Wio-E5, fragmentation 255 octets, demi-duplex, retries init. | Fragmentation/réassemblage + erreur timeout. |
| 11 | `rns_link_request` | Link minimal et packet proof nécessaires aux Request/Response. | Handshake simulé A/B. |
| 12 | `rns_request_response` | Requêtes ciblées, réponses corrélées, timeout. | Request reçoit bonne response, mauvaise corrélation rejetée. |
| 13 | `rns_resource` | Resource minimal pour batch DAG : découpe, séquence, checksum, reassembly. | Batch > MTU transféré et reconstruit bit-identique. |
| 14 | `storage` | NVS chiffrée : identité, alias, PIN hash, seq, checkpoints. | Save/load idempotent avec backend mock. |
| 15 | `meshpay_tx` | Struct transaction, CBOR compact, hash signable, signature Ed25519. | Encode/decode, signature valide, champ altéré rejeté. |
| 16 | `dag` | Insertion, tips, conflits `(from, seq)`, fenêtre 250 TX, seuil checkpoint 200. | Merge valide, duplicate, conflit, checkpoint-needed. |
| 17 | `currency` | Config monnaie, réserve pré-minée, fees vers `mint_authorities[0]`, fonte checkpoint. | Solde, fee, demurrage, MINT non autorisé rejeté. |
| 18 | `wallet` | Solde local, `wallet_lock`, `next_seq`, timeout 30 s, PIN policy. | Lock/unlock, double dépense locale, PIN failures. |
| 19 | `payment_engine` | Création paiement, envoi Reticulum DATA, preuve/ACK, validation réception, feedback UI. | Paiement A/B en mémoire avec ACK/proof. |
| 20 | `dag_sync` | Summary broadcast, request ciblée, batch Resource, intégration transactions manquantes. | Noeud en retard rattrape batch manquant. |
| 21 | `device_hal` | HAL display/touch/storage/lora/espnow/power par cible CYD, Waveshare et LilyGo H752. | Tests mocks des contrats HAL, hardware smoke séparé. |
| 22 | `ui` | LVGL Setup, PIN, Accueil, Payer, Recevoir, Historique minimal, état réseau. | Tests logique UI sans rendu : transitions et feedback. |
| 23 | `app_main` | Tâches FreeRTOS `ui_task`, `reticulum_task`, `core_task`, mutex non récursif, queues. | Test d'intégration simulé : boot, announce, paiement. |
| 24 | `hardware_smoke` | Scripts build/flash/monitor chiffrés, tests banc Waveshare/CYD/H752. | Scénarios manuels documentés : boot, announce, paiement, sync. |

## Plan de tests et critères d'acceptation

- Aucun composant n'est considéré terminé sans son test Unity dans `components/<composant>/test/`.
- Les composants Reticulum doivent passer deux niveaux : tests C purs + compatibilité contre fixtures Python officielles.
- Les tests d'intégration simulés doivent couvrir : découverte par announce, paiement direct chiffré, ACK/proof, double dépense rejetée, rattrapage DAG.
- Les tests hardware arrivent seulement après les tests unitaires : build S3, build CYD, flash chiffré, lecture logs, paiement entre deux devices.
- Le premier jalon utilisable est atteint quand deux noeuds simulés peuvent s'annoncer, échanger une transaction signée/chiffrée, confirmer, puis synchroniser un batch DAG.

## Hypothèses et références

- Décision verrouillée : nouveau projet dans `MeshPayV2`, pas migration directe de `../Mesh Pay`.
- Décision verrouillée : Reticulum est porté par paliers interopérables, pas port complet monolithique.
- Le port C doit être clean-room autant que possible : s'appuyer sur le manuel, les fixtures et les comportements observés, avec revue licence avant toute traduction directe du code Python.
- Références utilisées : manuel Reticulum, référence officielle GitHub Reticulum, spécification locale `Doctech V2/Spécifications Mesh Pay - Réseau Reticulum pour ESP32 bare-metal.md`.
