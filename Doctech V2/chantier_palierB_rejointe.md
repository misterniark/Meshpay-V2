# Chantier — Palier B : protocole de rejointe (code d'invitation + descripteur radio)

> Statut : **décomposition validée, prête pour implémentation** · Créé le 2026-06-30
> Dépend de : Palier A (descripteur signé + persistance + dérivation config) — TERMINÉ.
> Suite logique : Palier C (vouchers/crédit initial), Palier D (UI wizard + écran rejointe).

---

## 1. Objectif

Permettre à un **nouveau device (membre)** de **rejoindre une monnaie** créée par le
fondateur : obtenir son **descripteur signé**, le vérifier, le persister, et dériver sa
config — sans que le membre ne fasse une confiance aveugle au premier descripteur entendu
sur la radio.

## 2. Décisions verrouillées

- **Modèle de confiance** : **code d'invitation COURT** (ancre, hors-bande, affiché par le
  fondateur et saisi par le membre) qui **ancre la confiance** sur un descripteur précis.
  Le **descripteur complet (~242 o)** voyage **par radio** ; le membre ne l'importe QUE
  s'il correspond à l'ancre saisie. (Décidé en session : aucune carte n'a de caméra → QR
  impossible ; ~242 o trop gros à taper → seul un code court est saisissable.)
- **Transport-agnostique** : ESP-NOW **et** LoRa selon la config (on ne teste qu'ESP-NOW
  pour l'instant), comme tout le reste de la stack.
- **Mono-monnaie** par device.
- **B = protocole/logique headless** (testable sans LVGL). L'UI (saisie du code au clavier
  T-Deck, écran de rejointe) est le **Palier D**.
- La **moitié « import » est déjà faite** (Palier A) : `meshpay_currency_descriptor_decode`
  → `..._verify` (genèse + signature fondateur) → `meshpay_storage_record_set_currency_descriptor`
  → `meshpay_app_currency_from_record`. B ajoute surtout l'**ancre** + la **diffusion radio**
  + la **machine à états de rejointe**.

## 3. Format de l'ancre (code d'invitation)

- L'ancre = **préfixe de la genèse** du descripteur. La genèse = `SHA-256(corps CBOR
  canonique)` (Palier A), unique et liante. `currency_id = genèse[0..3]` en est déjà un
  préfixe.
- **Code = N premiers octets de la genèse** (cible **10 o = 80 bits**, collision
  négligeable) **+ 1 octet de checksum** (détection de faute de frappe), encodés en
  **base32 sans ambiguïté** (alphabet sans 0/O/1/I/L) → **~18 caractères** groupés
  (`XXXX-XXXX-XXXX-XX`).
- Vérif côté membre : recompute `genèse(descripteur reçu)`, compare son préfixe à l'ancre
  saisie ; **rejet** si différent. Puis vérif signature fondateur (déjà en A).

## 4. Décomposition

| Sous-palier | Contenu | Livrable / test |
| --- | --- | --- |
| **B1** | **Format ancre/code d'invitation** : `meshpay_currency_invite_encode(signed, char* out)` → code base32+checksum ; `meshpay_currency_invite_decode(code, anchor_out, &len)` → octets d'ancre + validation checksum/longueur/alphabet. Pur, transport-agnostique. | composant (ou ext. `currency_descriptor`) + tests Unity (round-trip, checksum KO, caractères invalides, longueur, NULL) |
| **B2** | **Contrôle d'ancre** : `meshpay_currency_descriptor_matches_anchor(signed, anchor, len)` — recompute la genèse, compare le préfixe en temps constant. | tests (match, mismatch 1 bit, longueur partielle) |
| **B3** | **Diffusion/obtention du descripteur par radio** (transport-agnostique). **Sous-décision à trancher** : (i) requête/réponse ciblée (le membre demande le descripteur à un pair, comme `dag_sync` request/response + éventuellement Resource si fragmenté) — *recommandé* (économe en LoRa, à la demande) ; ou (ii) porté dans l'`app_data` de l'announce (push, mais coûteux à chaque announce). | encode/decode message « descriptor offer/request » + tests wire ; pas de logique métier ici |
| **B4** | **Machine à états de rejointe** dans `app_main` runtime : état « ancre en attente » (posée après saisie du code) → sur réception d'un descripteur **matchant l'ancre** → `verify` → persist (storage record) → **re-dérive la config** (chemin A5) → état « membre actif ». Ignorer/refuser tout descripteur non-matchant. | tests runtime (rejointe nominale, descripteur non-matchant rejeté, signature KO rejetée, idempotence si déjà membre) |
| **B5** | **Côté fondateur** : exposer/servir son propre descripteur (réponse à la requête B3, ou inclusion announce), + génération du code d'invitation à afficher (consommé par l'UI en D). | tests (le fondateur répond avec son descripteur ; le code généré décode vers la bonne ancre) |

## 5. Hors périmètre B (rappels)

- **Crédit initial / vouchers** = Palier C (s'appuie sur conflit DAG `voucher_nonce`).
- **UI** (saisie clavier du code, écran rejointe, affichage du code fondateur) = Palier D.
  B fournit les fonctions pures + le câblage runtime, pilotables par test sans écran.

## 6. Ordre d'implémentation proposé

B1 → B2 (logique pure, TDD, rapides) → B3 (wire) → B4 (runtime, le cœur) → B5 (fondateur).
Validation banc on-device au fil de l'eau (infra de banc PSRAM+pool désormais en place :
voir mémoire `banc-test-ondevice-psram-pool`). Tests ESP-NOW seulement pour l'instant ;
le format reste LoRa-compatible par construction.
