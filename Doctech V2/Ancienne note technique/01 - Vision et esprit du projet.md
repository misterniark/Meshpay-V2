---
tags:
  - meshpay
  - meshpay/vision
  - philosophie
  - souveraineté-monétaire
Projets:
  - Mesh Pay
Topics:
  - Monnaie locale
  - Décentralisation
  - Résilience
Date: 2026-04-18
---

# Vision et esprit du projet

> [!quote] Le postulat fondateur
> Il est possible d'échanger de la valeur **sans dépendre d'un tiers** — ni banque, ni serveur, ni internet, ni opérateur télécom, ni même électricité abondante.

## L'intention

MeshPay n'est pas "un système de paiement en plus". C'est une **affirmation politique et technique** : la monnaie est un outil, pas un privilège. Les communautés qui veulent échanger doivent pouvoir le faire avec leurs propres règles, dans leurs propres conditions matérielles.

Trois axes motivent le projet :

1. **Souveraineté** — "comment est créée ma monnaie ? quelles règles suit-elle ? comment la gagne-t-on ? dans quoi peut-on la dépenser ?" Ces questions sont reprises en main par ceux qui utilisent la monnaie.
2. **Résilience** — quand les infrastructures s'effondrent (coupures, censure, catastrophes, pannes de réseau), l'échange continue.
3. **Frugalité** — des boîtiers à ~15-25 € qui tiennent des jours ou des semaines sur batterie, pas des data centers énergivores.

## Les publics ciblés

| Public | Besoin | Apport MeshPay |
|---|---|---|
| **Communautés locales** | Créer leur propre monnaie d'échange (marché, village, coopérative) | Monnaie entièrement [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage\|configurable]] avec règles adaptées |
| **Organisateurs de festivals** | Paiement interne sans dépendre de 4G / CB | Fonctionne hors-ligne, jetons numériques autonomes |
| **Zones isolées** | Pas ou peu d'infra bancaire/télécom fiable | Pas de prérequis réseau — [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale\|mesh ESP-NOW + LoRa]] |
| **Situations d'urgence** | Réseaux classiques HS | Redondance radio, batterie longue durée |
| **Collectifs qui veulent s'émanciper du capitalisme financier** | Outil d'échange indépendant | Pas de point central à saisir, censurer ou surveiller |

## Les valeurs non négociables

> [!success] Ces principes guident toutes les décisions techniques
> - **Pas d'infrastructure centralisée** : le réseau doit fonctionner même s'il n'en reste que deux devices
> - **Pas de surveillance** : les transactions ne transitent pas par un tiers
> - **Consommation minimale** : tenir plusieurs jours sur batterie
> - **Pas de barrière d'entrée** : pas besoin de carte bancaire, de smartphone, d'abonnement
> - **Open hardware** : composants standard, documentation ouverte

## Le fonctionnement en une phrase

Chaque boîtier est à la fois **portefeuille, émetteur et relais**. Les paiements passent en direct par ESP-NOW (~200 m, <1 seconde). La synchronisation globale se fait en LoRa (~2 km) par diffusion toutes les 2 minutes — comme une rumeur qui se propage de proche en proche dans un village.

## La monnaie, un paramètre (pas un dogme)

Un point essentiel de l'esprit MeshPay : **la monnaie n'est pas codée en dur**. Chaque réseau choisit ses règles :
- Nom, symbole, décimales
- Plafond de masse monétaire, facultatif.
- Frais par transfert (commission reversée à l'organisateur ou brûlée), facultatif.
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#La fonte demurrage|Fonte / demurrage]] pour encourager la circulation, facultatif.
- Date d'expiration programmée, facultatif.
- Liste des maîtres autorisés à créer de la monnaie

Voir les détails dans [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage]].

## Ce que MeshPay n'est PAS

> [!warning] Lire attentivement avant tout déploiement
> - Ce n'est **pas** une cryptomonnaie spéculative — pas de marché, pas de staking, pas de mineurs
> - Ce n'est **pas** un système anonyme au sens crypto — les clés publiques sont identifiables dans le réseau local
> - Ce n'est **pas** un remplacement des monnaies nationales — c'est un **complément** adapté à des usages communautaires
> - Ce n'est **pas** un produit bancaire — pas de KYC, pas de garantie de dépôt, pas d'assurance

## Inspirations

- Les **SEL** (Systèmes d'Échange Locaux) et leurs monnaies fondantes
- Les **festivals** qui utilisent déjà des jetons mais dépendent d'une caisse centrale
- Les travaux de Silvio Gesell sur la [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage#La fonte demurrage|monnaie fondante]]
- Les réseaux mesh historiques (B.A.T.M.A.N., cjdns) pour la topologie
- L'architecture DAG pour le registre (inspiration IOTA, SPECTRE) mais dans une version minimaliste

## Liens

- Vers la réalité technique : [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale]]
- Vers les choix pratiques : [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/03 - Décisions d'usage]]
- Vers la dette et les limites connues : [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique]]

## Notes liées

- [[Mesh Pay (MOOC)]] — hub du projet
- [[Mesh Pay Expliqué]] — vulgarisation grand public
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]] — index documentation technique
- Concepts connexes : [[Mesh]], [[ESP-NOW]], [[LoRa]], [[DAG]]
