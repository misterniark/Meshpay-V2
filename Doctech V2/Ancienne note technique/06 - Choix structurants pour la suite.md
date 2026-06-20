---
tags:
  - meshpay
  - meshpay
  - meshpay/décision
  - stratégie
Projets:
  - Mesh Pay
Topics:
  - Roadmap
  - Architecture
Date: 2026-04-18
---

# Choix structurants pour la suite

> [!abstract] Pourquoi cette note
> Documenter les **décisions qui engagent** le projet sur la durée. Certaines sont déjà prises, d'autres pendent. Les reconnaître permet d'éviter des reworks massifs.

## Ce qui est engagé définitivement

### Le [[DAG]] comme structure de registre

Revenir à une blockchain (ou Merkle tree linéaire) signifierait réécrire `core/dag/`, `dag_merge`, `dag_prune`, `wallet`, la sérialisation CBOR, les checkpoints, et modifier toute la logique de finalité. [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Registre DAG plutôt que blockchain|Le DAG est une décision architecturale structurante]] — on ne revient pas dessus.

### Le double protocole [[ESP-NOW]] + [[LoRa]]

Les deux radios sont utilisées par des couches différentes. Supprimer ESP-NOW = perdre la latence sub-seconde. Supprimer LoRa = perdre la convergence du réseau. [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Communication radio hybride|Décision validée]], conservée.

### Ed25519 + SHA-256

Tout le protocole wire intègre les signatures et hashes à ces tailles précises (32 + 64 + 32 octets). Changer signifie réécrire tous les packers/unpackers et casser la compat.

### Format CBOR de la transaction

- Les **clés numériques** (1, 2, 3…) sont en place
- Ajout récent du champ `seq` (clé 12) — [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I3 nonce monotone|voir I3]]
- Toute évolution du format doit rester rétrocompatible (CBOR ignore les clés inconnues)

### Réserve pré-minée comme modèle produit

La création monétaire cible n'est plus "chaque paiement peut minter". La masse est créée dans une réserve initiale, puis distribuée par `TRANSFER`. Cela garde l'offline simple : un device admin agit comme caisse, sans autorité centrale nécessaire pendant chaque paiement.

Le self-mint Waveshare est conservé, mais seulement comme **mode banc** (`MESHPAY_BENCH_SELF_MASTER` + `MESHPAY_TEST_DEVICE_SEED`) tant que le CYD maître n'est pas disponible.

Pour rendre les tests plus proches du modèle cible, le banc expose désormais `Admin > Init monnaie` : la réserve est choisie depuis l'UI, persistée en NVS, créée une seule fois, puis propagée comme TX de réserve après reboot. Cela prépare le futur flux de manifeste signé sans bloquer les tests sur Waveshare.

## Ce qui engage mais reste ajustable

### Fenêtre RAM de 250 TX

> [!note] Plafond calibré sur DRAM ESP32
> Chaque TX fait ~233 octets. 250 × 233 = ~58 KB. Sur les 520 KB de DRAM, l'UI LVGL consomme déjà beaucoup. 250 est un **compromis**. On peut :
> - ✅ Réduire (ex: 128) si on a besoin de RAM pour autre chose
> - ✅ Augmenter (ex: 500) sur ESP32-S3 avec PSRAM
> - Le checkpoint s'adapte automatiquement (seuil = 80% de DAG_MAX_TRANSACTIONS)

### Seuil de checkpoint à 80%

Basé sur `dag_needs_checkpoint()`. Ajustable dans `dag_prune.c`. Attention : trop bas = checkpoints trop fréquents (coût NVS) ; trop haut = pas de marge pour de nouvelles insertions pendant le checkpoint.

### Rate-limit 10 msg/s/MAC + 50 global

Valeurs empiriques. Peuvent être ajustées en fonction des retours terrain (fréquence de sync, taille du réseau). Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Rate-limiting ESP-NOW]].

### Période LoRa sync de 2 minutes

Trade-off conso batterie ↔ réactivité. Un réseau très actif pourrait descendre à 1 min. Un réseau en veille pourrait passer à 5 min. C'est configurable dans `lora_sync.c`.

## Ce qui reste à trancher (décisions structurantes futures)

### Manifeste de monnaie signé (C5)

> [!danger] Décision structurante majeure en attente
> Le self-master est désormais explicite et limité au banc Waveshare. **Impossible de faire un vrai réseau partagé sans manifeste signé.** Voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#Manifeste de monnaie signé C5|la dette C5]] pour le plan détaillé.

Cette décision impacte :
- Structure `currency_config_t` (ajout de `root_key`, `manifest_version`, `manifest_signature`)
- Persistance NVS
- Nouveau flow UI de setup (créer / rejoindre)
- Nouveau message LoRa `COMM_MSG_LORA_MANIFEST`
- Politique de mise à jour du manifeste (ajout/retrait d'un maître)

**Quand la prendre** : avant tout déploiement terrain avec 3+ devices.

### Modèle de réconciliation multi-maîtres / multi-réserves

Que se passe-t-il si deux maîtres créent/rechargent indépendamment des réserves et dépassent collectivement le `max_supply` ? Options :

| Option | Principe | Complexité |
|---|---|---|
| Pas de réconciliation | On laisse passer, responsabilité des maîtres | Zéro |
| Plafond par maître | Chaque maître a sa propre quote-part | Modéré |
| Multi-sig pour MINT | 2/N maîtres doivent co-signer | Important (protocolaire) |
| Réconciliation async | Détection + annulation rétroactive | Lourde |

**Décision reportée** — voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#Réconciliation multi-maîtres]].

### Fin de monnaie (prolongation / arrêt anticipé)

`valid_until` dans `currency_config_t` permet une expiration programmée. Mais :
- Comment la **prolonger** si le festival dure plus longtemps que prévu ?
- Comment l'**arrêter** avant la date ? (ex: catastrophe, consensus de fin)

Options :
- Nouveau message `COMM_MSG_LORA_CURRENCY_END` signé multi-sig
- Nouveau manifeste (version++) avec `valid_until` différent

**Décision reportée** — implique le manifeste (C5).

### OTA (Over-The-Air update)

Pas de flux OTA applicatif actuellement. Quand il arrivera, il devra :
- Être signé par une clé de signature de firmware (distincte de la root monétaire)
- Être vérifié côté device avant installation
- Permettre le rollback en cas d'échec
- Idéalement se propager en mesh (le firmware reçu par un device se propage aux voisins)

**Décision reportée** — pas urgent avant les premiers prototypes terrain.

### Politique de rétention des checkpoints

Combien de checkpoints historiques garder en Flash ? Un seul (actuel) simplifie mais empêche le rollback. Plusieurs permettent d'auditer l'historique.

**Décision reportée** — pas critique pour le prototype.

## Dépendances hardware à figer

### Module LoRa : aujourd'hui Wio-E5 (UART AT)

- Protocole AT simple, débogage facile
- Limité à LoRa/LoRaWAN (pas LoRaWAN utilisé)
- **À figer** : pinout UART définitif sur la carte finale

### Choix d'un **board de production**

CYD est parfait pour prototyper mais pas idéal en production (hardware non optimisé, qualité variable, sécurité physique zéro). Une carte custom sera nécessaire pour :
- Encastrement LoRa natif (pas de module externe)
- Anti-tamper basique
- Form factor adapté (poche / comptoir / poignet)
- Certification radio (CE, FCC)

**Décision reportée** — après validation du prototype.

## Les points de non-retour identifiés

Ce qui serait très douloureux à changer plus tard :

1. **Taille de la clé de chiffrement NVS** — liée à l'eFuse, one-shot en production
2. **Format wire des messages** — tout changement casse la compat entre versions de firmware
3. **Format de la TX CBOR** — idem (migrations possibles mais coûteuses)
4. **Politique d'attribution des `mint_authorities`** — une fois un maître légitimé dans le manifeste, le retirer demande un nouveau manifeste signé

Tout ajout doit idéalement passer par une **version de manifeste** pour rester compatible.

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]] — ce qui est conscient et reporté
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques]] — fondations techniques
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]] — la voie parcourue

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[Mesh Pay specs]] — spec technique 36 sections
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]] — index documentation technique
