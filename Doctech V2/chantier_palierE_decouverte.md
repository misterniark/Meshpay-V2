# Chantier Palier E — Rejointe par découverte radio (zéro saisie)

> **Statut : proposé** (2026-07-15). Suite du Palier D (UI création/rejointe).
> Décision utilisateur : option A — « Rejoindre » liste les monnaies à portée
> radio, on sélectionne, zéro saisie. Le code long (18 symboles) reste un repli.

## Constat qui motive le chantier

Le code d'invitation fait 22 caractères (ancre 10 o de genesis + checksum,
base32 Crockford). Or il ne **transporte** rien : le descripteur complet arrive
par radio (OFFER d'un membre à portée). Le code n'apporte que l'authentification
« c'est bien LA monnaie de X ». Comme la rejointe exige de toute façon la
proximité radio, la radio peut aussi porter la découverte — l'humain authentifie
visuellement (nom + fondateur + empreinte) au lieu de dicter 22 caractères.
Bénéfice collatéral : le Waveshare (sans clavier physique) n'a plus besoin d'un
clavier tactile base32 (l'ancien périmètre D5 se réduit au rendu + tap).

## Architecture (réutilisation du Palier B)

Protocole `descriptor_sync` existant : `REQUEST {type 0x33, currency_id, source}`
→ `OFFER {type 0x34, descripteur signé complet}` (PLAIN broadcast, répondeur
membre côté app_main). La découverte ajoute UN message :

- **DISCOVER `0x35`** `{type(1) + source(16)}` : « quiconque est membre d'une
  monnaie répond OFFER ». Pas de currency_id (c'est toute la différence avec
  REQUEST) — pas de « joker » ambigu sur currency_id=0.
- Le répondeur membre répond son OFFER habituel (descripteur complet signé).
  Anti-tempête : throttle par source (même cache de réponse que les announces).
- Côté demandeur, un mode **collecte** : les OFFER reçus pendant la fenêtre de
  découverte ne sont PAS importés ; ils sont vérifiés (signature fondateur +
  genesis → currency_id) puis stockés dédupliqués (≤ 4 monnaies).
- La sélection importe le descripteur choisi **déjà en main** (réutilise
  l'import B4 + CLAIM C4). L'ancre n'est plus nécessaire dans ce flux.

Sécurité (documentée, assumée) : l'authentification hors-bande du code est
remplacée par la confirmation visuelle : nom + symbole + **empreinte courte**
(4 octets hex du genesis) affichés avant de confirmer. Un usurpateur à portée
peut annoncer un même nom, mais pas la même empreinte. Durcissement ultérieur
possible (comparaison SAS type Bluetooth) hors périmètre E.

## Découpage

- **E1 — protocole (headless, Unity)** : `descriptor_sync` : build/parse
  DISCOVER 0x35 ; répondeur membre étendu (app_main) : OFFER en réponse à
  DISCOVER avec throttle. Tests [descriptor_sync][e1] + [app_main][e1].
- **E2 — runtime collecte (headless, Unity)** : `meshpay_app_runtime_arm_discovery
  (runtime, now)` (fenêtre 60 s), collecte OFFER en mode découverte : vérif +
  dédup par currency_id + stockage {nom, symbole, empreinte, blob descripteur}
  (≤ 4, en PSRAM ou pool statique) ; `discovered_count/get(i)` ;
  `meshpay_app_runtime_join_discovered(runtime, index, now)` → import + CLAIM ;
  émission DISCOVER périodique pendant la fenêtre (immédiate à l'armement puis
  ~3 s, portée par la tâche UI, hors verrou). Tests [app_main][e2].
- **E3 — UI portable (headless, Unity)** : `SCREEN_JOIN` devient la **liste**
  (pattern PAYEE existant) : primary = monnaie sélectionnée (nom + empreinte),
  detail_lines = autres, actions `OK / Suiv. / Code / Accueil` ; nouvel écran
  `SCREEN_JOIN_CODE` = l'ancienne saisie manuelle (repli). Setter
  `meshpay_ui_set_discovered(...)`. Tests [ui][e3].
- **E4 — T-Deck (œil)** : ouverture JOIN → arm_discovery + DISCOVER périodique ;
  liste rafraîchie en boucle de rendu ; OK → join_discovered ; Code → repli
  saisie clavier. Validation au banc à l'œil.
- **E5 — Waveshare (œil, absorbe l'ancien D5)** : rendu des écrans monnaie
  (menu / liste / code fondateur / wizard) + mapping tactile. **Décision** : pas
  de clavier base32 tactile — sur Waveshare le repli « saisie code » est absent
  (bouton Code masqué), la découverte est le seul flux de rejointe. Le T-Deck
  (clavier physique) garde le repli. La création (wizard, saisie du nom) reste
  possible au T-Deck seulement dans ce palier ; wizard tactile Waveshare =
  chantier ultérieur si besoin.

## Hors périmètre E

- Comparaison SAS / empreinte longue (durcissement usurpation).
- Wizard de création tactile Waveshare (la création reste sur T-Deck).
- Validation d'ingestion DAG (P0 différé, `chantier_durcissement_ingestion.md`).

## Critères de fin

1. Banc Unity : suites [e1][e2][e3] vertes on-device.
2. À l'œil : T-Deck A crée une monnaie ; Waveshare B « Rejoindre » → voit
   « <nom> <empreinte> » en ~5 s → OK → devient membre, solde = crédit initial ;
   T-Deck A voit le membre. Reboot des deux : états persistants.
3. Le code long reste fonctionnel au T-Deck (repli) : saisie manuelle → ARMED →
   MEMBER via OFFER ciblé.
