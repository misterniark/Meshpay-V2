# Chantier — Palier D : UI création & rejointe de monnaie

> Statut : **décomposition proposée, à valider** · Créé le 2026-07-14
> Dépend de : Paliers A (descripteur), B (rejointe), C (crédit initial) — TERMINÉS.
> Dernière brique de la feature « création de monnaie ».

---

## 1. Objectif

Rendre la feature utilisable **sans banc** : un humain crée une monnaie ou en rejoint une
depuis l'écran, saisit les champs au clavier, voit le code d'invitation et son solde. Toute
la logique headless (A/B/C) existe ; il manque **les écrans + le rendu + la saisie** et
**l'API runtime de création** (un fondateur ne peut pas encore créer une monnaie à
l'exécution — `meshpay_currency_descriptor_sign` n'est appelé que par les tests).

## 2. Décisions verrouillées

- **Wizard de création COMPLET** (6 champs : nom, symbole, max_supply, frais de transfert,
  crédit initial, demurrage on/bps) **mais avec des VALEURS PAR DÉFAUT pré-remplies** dans
  l'UI : le fondateur peut tout accepter d'un coup ou éditer champ par champ.
- **Deux cartes** : T-Deck (vrai clavier matériel) **et** Waveshare S3 (tactile → **clavier
  alphanumérique à l'écran** pour saisir le nom et le code d'invitation base32).
- **Logique UI portable, sans LVGL** : on étend `meshpay_ui_state_t` + `build_view` (comme
  l'existant), testée en Unity headless. Le rendu réel est board-spécifique dans `main/`.
- **Saisie texte générique** : nouveau `meshpay_ui_input_char(ui, char)` (généralise
  `input_digit`), alimenté par le clavier T-Deck OU le clavier tactile Waveshare — la
  logique d'écran ne connaît pas la source.
- **Mono-monnaie strict** (repris de B) : créer/rejoindre n'est proposé que si
  `has_descriptor == false`. Une fois membre/fondateur : l'écran bascule sur « infos monnaie
  + code d'invitation (si fondateur) + solde ».
- **Entrée du flux** : action « Monnaie » depuis Home/Réseau → sous-menu **Créer / Rejoindre**
  (visible tant que non-membre). Pas d'écran bloquant au 1er boot (le repli legacy garde le
  wallet utilisable).

## 3. Décomposition

Deux couches : **D-logique** (headless, validée au banc Unity comme B/C) et **D-matériel**
(rendu + saisie, validés à l'œil sur carte).

| Sous-palier | Contenu | Composant | Validation |
| --- | --- | --- | --- |
| **D1** | API runtime `meshpay_app_runtime_create_currency(runtime, params, now_ms)` : construit le corps du descripteur depuis les params fondateur, signe avec l'identité locale, dérive genesis/currency_id, persiste le blob (storage), applique la config EN PLACE (devient fondateur-membre, `has_descriptor=true`, autorité MINT), puis **auto-CLAIM** du crédit initial (réutilise C4). Mono-monnaie strict (refuse si déjà membre). | `app_main` | Unity `[app_main][d1]` |
| **D2** | Saisie texte portable (`meshpay_ui_input_char`) + écrans « membre/fondateur » : **Rejointe** (saisie du code d'invitation), **Code fondateur** (affiche `invite_code`), **infos monnaie + solde/crédit** sur Home. Exposition de `join_state` à l'UI. Branches `build_view` + nav. | `ui` | Unity `[ui][d2]` |
| **D3** | Écran **Wizard création** : les 6 champs pré-remplis de défauts, navigation champ→champ, édition (texte/nombre/toggle), validation → produit les `params` pour D1. | `ui` | Unity `[ui][d3]` |
| **D4** | **Rendu + saisie T-Deck** : `render_tdeck_view()` (ST7789 RGB565, à créer intégralement) + tâche clavier→UI (`meshpay_hal_keyboard_read` → `input_char`, sur le modèle de `waveshare_touch_task`). | `main` (+`device_hal`) | œil sur T-Deck |
| **D5** | **Rendu + saisie Waveshare** : **clavier alphanumérique tactile** à l'écran (layout base32/QWERTY) alimentant `input_char` + extension de `render_waveshare_view()` aux nouveaux écrans. | `main` | œil sur Waveshare S3 |
| **D6** | **Câblage actions → runtime** sur les deux cartes : action « Monnaie » (Créer/Rejoindre), Créer → `create_currency`, Rejoindre → `arm_join` (la `join_request_task` prend le relais en `ARMED`), Code fondateur → `invite_code`. Rendu du sous-menu. | `main` | œil sur les deux cartes |

## 4. Flux de bout en bout

**Fondateur** : Home → « Monnaie » → « Créer » → wizard (6 champs pré-remplis, éditer,
valider) → D1 `create_currency` (signe + persiste + devient fondateur + auto-crédite) →
écran « Code fondateur » affiche le code d'invitation à dicter.

**Membre** : Home → « Monnaie » → « Rejoindre » → saisie du code au clavier → `arm_join` →
`join_request_task` émet la REQUEST → réception de l'OFFER (B4) → import → auto-CLAIM (C4) →
Home affiche le solde crédité.

## 5. Hors périmètre D

- Le **trackball** T-Deck (non driverisé) : navigation au clavier/tactile uniquement.
- La **validation d'ingestion sync** (P0 sécurité) : chantier séparé
  `chantier_durcissement_ingestion.md`.
- **Multi-monnaie / changement de monnaie / révocation** : hors v1 (mono-monnaie strict).
- **Édition d'une monnaie après création** : les règles sont figées à la création (le
  descripteur est signé une fois).

## 6. Ordre d'implémentation proposé

D1 (create_currency, TDD banc) → D2 (UI membre/fondateur, TDD banc) → D3 (UI wizard, TDD
banc) → D4 (rendu+clavier T-Deck) → D5 (rendu+clavier tactile Waveshare) → D6 (câblage
actions). Les sous-paliers headless (D1-D3) sont committés/validés au banc façon A/B/C ; les
sous-paliers matériels (D4-D6) sont validés à l'œil sur carte (captures/log), chacun committé
séparément.
