# Chantier — Erreurs UI invisibles (fin des actions muettes)

> Statut : **VALIDÉ — banc 315/0 + non-régression réelle 4 devices le 2026-07-15 (§ 7)**.
> Décisions utilisateur : **Option A (feedbacks typés)**, **fenêtre de rejointe 60 s**.
> Créé le 2026-07-15 (dette UX relevée au Palier E, aggravée
> par la revue du chantier migration NVS).
> Priorité : **P2 UX** — mais contient un **correctif P1** : le feedback M4
> « stockage HS » s'affiche À TORT pour « déjà membre » (collision de code
> d'erreur, voir § 3).

## 1. Problème

Le M4 du chantier migration NVS a rendu visibles les échecs d'écriture dus au
stockage. Mais le mal générique demeure : **une action utilisateur qui échoue
pour toute autre raison ne produit RIEN à l'écran** — le motif part en série
(`ESP_LOGW`), invisible sans câble. L'utilisateur tape, rien ne se passe, il
retape, conclut au bug matériel. C'est le « OK ne fait rien » sous toutes ses
formes restantes.

Les feedbacks existants (footer transient, `ui.c:feedback_text`) couvrent :
paiement (LOCKED/SENT/RECEIVED/CONFIRMED/REJECTED), PIN (PIN_ERROR), stockage
(STORAGE_ERROR, M4). Tout le reste est muet.

## 2. Inventaire des chemins muets (main/app_main.c, 2026-07-15)

| Chemin (`wallet_run_deferred`) | Échec réel possible | Aujourd'hui |
| --- | --- | --- |
| `WALLET_DEFER_CREATE` (~1215) | nom vide, crédit > offre, **déjà membre**, storage HS | log W, l'UI **reste sur le wizard** sans un mot — le cas majeur |
| `WALLET_DEFER_JOIN` (~1239) | **code d'invitation malformé** (1 faute de frappe sur 22 caractères !), déjà membre | log W ; M4 n'affiche que le cas storage — et il se déclenche à tort (§ 3) |
| `WALLET_DEFER_ARM_DISCOVERY` (~1285) | armement refusé (déjà membre) | log W, l'écran affiche « Recherche... » **fantôme** qui ne trouvera jamais rien |
| `WALLET_DEFER_JOIN_DISCOVERED` (~1299) | index périmé, liste désarmée, import refusé | log W ; M4 ne couvre que INVALID_STATE |
| `WALLET_DEFER_SHOW_CODE` | — | ✓ déjà correct (« Indisponible » affiché) |

État mensonger apparenté : **la fenêtre d'armement de la rejointe n'expire
jamais** — `join_armed_until_ms` est posé (`app_main_logic.c:715`) mais le
désarmement différé n'a jamais été implémenté (commentaire ~1914). Si aucun
OFFER ne répond (fondateur éteint, mauvais code plausible), le menu affiche
« Rejointe en cours » **à vie**.

Hors périmètre (défendu ailleurs) : erreurs de fond radio/sync (alertes du
monitor DAG), échecs de save runtime avec rollback propagés aux actions
(couverts par les feedbacks d'action), feedbacks paiement (déjà en place).

## 3. Correctif P1 embarqué — collision INVALID_STATE (régression douce M4)

`meshpay_app_runtime_arm_join` retourne `ESP_ERR_INVALID_STATE` pour **« déjà
membre »** (mono-monnaie strict, `app_main_logic.c:709-710`) — PAS pour un
problème de stockage (l'armement n'écrit rien ; l'import se fait à l'OFFER).
Le hook M4 de `WALLET_DEFER_JOIN` mappe `INVALID_STATE` → « Echec: stockage
HS » : **un utilisateur déjà membre qui re-saisit un code verrait un
mensonge**. Règle du chantier : le feedback storage ne se déclenche que si
`storage_status ≠ OK` (l'état poussé au boot fait foi), sinon le motif réel
s'affiche.

## 4. Mécanisme d'affichage — options

### Option A — Feedbacks typés par cause (RECOMMANDÉE)

Étendre `meshpay_ui_feedback_t` d'une entrée par cause utilisateur
(BAD_INVITE_CODE, ALREADY_MEMBER, CREATE_REFUSED, JOIN_EXPIRED,
DISCOVERY_REFUSED…) + libellés centralisés dans `feedback_text`.

- ✅ Cohérent avec l'existant (PIN_ERROR/STORAGE_ERROR) ; libellés en UN seul
  endroit (testables, traduisibles) ; zéro buffer texte dans l'état UI ; le
  firmware ne fait que mapper esp_err → cause.
- ❌ Une entrée d'enum par cause (le jour où il y en a 30, lourdeur — il y en
  a ~6 aujourd'hui).

### Option B — Feedback générique + texte libre poussé par le firmware

Un seul `FEEDBACK_ACTION_FAILED` + champ `error_text[48]` dans l'état UI.

- ✅ Extensible sans toucher l'enum.
- ❌ Libellés éparpillés dans main/app_main.c (dupliqués T-Deck/Waveshare si
  divergence), buffer de plus dans l'état, moins testable unitairement.

**Recommandation : Option A** — le vocabulaire des causes est petit et
stable, et la centralisation des libellés dans ui.c est exactement ce qui a
rendu M4 testable au banc.

## 5. Décomposition en paliers

### U1 — UI : causes typées + libellés + tests

- `meshpay_ui_feedback_t` + BAD_INVITE_CODE / ALREADY_MEMBER /
  CREATE_REFUSED / DISCOVERY_REFUSED / JOIN_EXPIRED (+ libellés courts
  affichables sur 1 ligne de footer Waveshare).
- `meshpay_ui_on_action_failed(ui, feedback)` (générique, pose le transient).
- Tests : chaque cause → footer attendu ; le transient s'efface à la nav ;
  priorité sur l'alerte storage persistante (règle M4 conservée).

### U2 — Firmware : câblage des 4 chemins muets + fix collision

- `WALLET_DEFER_CREATE` : mapper le motif (params invalides → CREATE_REFUSED ;
  INVALID_STATE → ALREADY_MEMBER ou STORAGE selon `storage_status`).
- `WALLET_DEFER_JOIN` : decode KO → BAD_INVITE_CODE ; INVALID_STATE →
  ALREADY_MEMBER (fix § 3) ; le cas storage passe par l'état storage.
- `WALLET_DEFER_ARM_DISCOVERY` : échec → DISCOVERY_REFUSED + retour au menu
  monnaie (pas de « Recherche... » fantôme).
- `WALLET_DEFER_JOIN_DISCOVERED` : NOT_FOUND/désarmé → DISCOVERY_REFUSED ;
  INVALID_STATE → selon `storage_status` (même règle).
- Tests logic : mapping esp_err → feedback pour chaque chemin (la logique de
  mapping vit dans app_main_logic pour être testable, le firmware l'appelle).

### U3 — Fenêtre de rejointe : expiration réelle + retour visible

- Implémenter le désarmement différé : `join_armed_until_ms = now + FENÊTRE`
  (constante, ~60 s ?) ; le tick runtime désarme à l'échéance → JOIN_EXPIRED
  poussé à l'UI, join_state repasse IDLE (le menu cesse de mentir).
- Décision à trancher : durée de la fenêtre (60 s ? 120 s ?).
- Tests : armement → tick avant échéance (rien) → tick après (désarmé +
  feedback) ; l'OFFER qui arrive à temps annule l'échéance.

### U4 — Banc complet + validation réelle

- Banc Unity 0 échec.
- Réel : saisir un code FAUX à l'écran → « Code invalide » ; re-saisir un
  code en étant membre → « Deja membre » ; wizard avec nom vide → refus
  visible ; armer une rejointe sans fondateur allumé → expiration visible
  après la fenêtre. Aucun de ces gestes ne doit laisser un écran muet ou
  mensonger.

## 6. Critères de validation

- Plus AUCUN chemin de `wallet_run_deferred` ne retourne sur échec sans
  feedback à l'écran (grep : chaque `return` d'échec est précédé d'un
  `meshpay_ui_on_*`).
- « Déjà membre » n'affiche plus jamais « stockage HS » (fix § 3 testé).
- Une rejointe armée sans réponse expire et le dit ; « Rejointe en cours »
  n'est plus un état pouvant mentir indéfiniment.
- Libellés bornés : lisibles sur le footer Waveshare (1 ligne, ~26 caractères
  utiles — le renderer n'affiche pas les detail_lines, leçon M5).
- Banc complet 0 échec + les 4 gestes réels du U4 vérifiés à l'écran.

## 7. Validation (2026-07-15)

### Banc on-device (T-Deck, test_app)

**315 tests, 0 échec** (309 + 6 nouveaux) :
- U1 : les 6 causes typées → libellé attendu au footer, transient effacé à la
  nav, priorité sur l'alerte storage persistante, valeurs de succès refusées ;
- U2 : mapping exhaustif esp_err → feedback, dont le fix de la collision M4
  (INVALID_STATE + storage sain → « Deja membre », + storage HS → « stockage
  HS ») ;
- U3 : armement → expiration à 60 s pile (désarmé + « Rejointe expiree »),
  no-op avant l'échéance et après désarmement, OFFER à temps n'expire rien.

### Réel (4 devices)

Non-régression validée : boots v3 directs, identités/alias conservés,
convergence stricte tx=10 conv=1, zéro crash, écrans sains.

**Gestes d'échec à l'écran : NON exécutés au réel** — découverte du chantier :
le menu monnaie d'un MEMBRE n'offre ni « Creer » ni « Rejoindre » (l'UI
prévient ces refus en amont), donc les 4 gestes du § 5-U4 exigent un device
NON membre → sacrifier l'identité d'un Waveshare (erase NVS) avec pollution
comptable à la re-rejointe (ancien compte orphelin + nouvelle CLAIM de 8).
Les chemins restent atteignables hors nominal (état ARMED persistant,
désynchronisations, T-Deck clavier) : les feedbacks servent de défense en
profondeur, intégralement couverte par le banc. Décision utilisateur (2026-07-15) : CLORE sur le banc + la non-régression —
la monnaie de test reste propre (pas de compte orphelin sacrifié) ; le
premier device neuf qui rejoindra exercera ces écrans naturellement.
