# Chantier — Palier C : crédit initial (voucher réflexif)

> Statut : **décomposition validée, prête pour implémentation** · Créé le 2026-07-01
> Dépend de : Palier A (descripteur signé + `initial_credit`) et Palier B (rejointe) — TERMINÉS.
> Suite logique : Palier D (UI wizard + écran rejointe).

---

## 1. Objectif

Un nouveau membre démarre à **0** (le self-mint / boot-credit historique a disparu). Le
descripteur signé du fondateur porte un champ **`initial_credit`** : le montant que
**tout nouveau membre** reçoit **une seule fois** en rejoignant. Le Palier C rend ce
crédit **effectif** : le membre s'auto-crédite `initial_credit`, l'événement est unique
par membre, il est propagé aux pairs et compté dans l'offre (plafond).

Contrainte forte du modèle : le fondateur est **hors-ligne** au moment de la rejointe
(octroi hors-ligne). Le crédit ne peut donc **pas** être signé par le fondateur à chaque
réclamation — il est **pré-autorisé** par le montant `initial_credit` figé dans le corps
signé du descripteur (chaîne de confiance déjà ancrée par le code d'invitation, Palier B).

## 2. Décisions verrouillées (brainstorm 2026-07-01)

- **TX auto-crédit réflexive** : nouveau type **`CLAIM = 3`**, `from == to == membre`,
  `fee == 0`, `seq == 0` (réservé), **signée par le membre lui-même**.
- **Montant** : `amount == descriptor.initial_credit` en **égalité STRICTE** (pas `≤`) —
  un membre ne peut pas s'auto-créditer un montant arbitraire.
- **Autorisation universelle, bornée par le plafond** : tout membre ayant importé le
  descripteur est en droit de réclamer `initial_credit` une fois ; il n'y a **pas
  d'allowlist**. Le garde-fou anti-inflation est **`max_supply`** (la CLAIM est comptée
  dans `total_minted`). Propriété assumée : le risque Sybil est **borné par
  `max_supply`** — le fondateur règle `initial_credit`/`max_supply` en conséquence.
- **Unicité « une réclamation par membre »** = **gratuite** via le **conflit DAG existant
  `(from, seq)`** : `seq == 0` réservé ⇒ une 2ᵉ CLAIM du même membre = même `(from, 0)` ⇒
  `MERGE_CONFLICT` ⇒ rejetée. **Aucun champ `voucher_nonce` ajouté au wire** (ce qui
  casserait le format CBOR et tous les fixtures).
- **Garde « déjà réclamé » = le DAG lui-même** : « ai-je réclamé ? » = « une CLAIM
  `from == moi` existe-t-elle dans mon DAG ? ». Le DAG est **persisté sur flash** (Phase A,
  `dag_store`) et **aucun élagage n'existe encore** → la CLAIM survit au reboot ; le
  retry-boot est **idempotent** (re-émission rejetée en DUPLICATE/CONFLICT). **Pas de flag
  `initial_credit_claimed` en storage** (pas de bump de version) : la garde durable
  « post-éviction » relève de la **Phase B (checkpoint)**, pas de C — voir §5.
- **Émission automatique à la rejointe** : dès que le membre devient membre (import du
  descripteur, B4) — ou au boot s'il est membre et n'a pas encore de CLAIM — le runtime
  émet la CLAIM, la **committe localement (commit-on-send)** et la **propage via DAG sync**.
  `initial_credit == 0` ⇒ on ne réclame rien.
- **Parents de la CLAIM** : les tips courants du DAG — **0 parent** si le DAG est vide
  (cas nominal d'un membre frais) ⇒ la CLAIM est le **genesis local du membre**. L'unicité
  ne dépend pas des parents (elle vient de `(from, seq==0)`). À vérifier en C1 : `seq==0`
  n'est jamais alloué par le wallet (`next_seq` démarre à 1), donc réservable sans collision.

## 3. Décomposition

| Sous-palier | Contenu | Composant | Livrable / test |
| --- | --- | --- | --- |
| **C1** | Type **`CLAIM = 3`** : `meshpay_tx_create_claim` (`from==to`, `fee==0`, signé membre) ; `validate_common` accepte CLAIM + règle `from==to` ; décodeur accepte le type 3. | `meshpay_tx` | tests Unity : create/sign/verify, `from!=to` rejeté, `fee!=0` rejeté, decode round-trip, encode borné |
| **C2** | `initial_credit` copié dans `meshpay_currency_config_t` (par `config_from_descriptor`) ; chemin de validation CLAIM dans `validate_tx` : `from==to`, `amount == config.initial_credit`, signé par le membre (`from`), **PAS** de vérif `founder_public`, compté dans `total_minted`, borné par `max_supply` ; `get_balance` crédite `to`. | `currency` | tests : CLAIM correct crédite ; `amount != initial_credit` rejeté ; CLAIM > `max_supply` rejeté ; CLAIM ne passe pas la vérif fondateur ; solde reflète le crédit |
| **C3** | Vérifier que le conflit `(from, seq==0)` couvre bien la CLAIM ; durcir `tx_shape_valid` (CLAIM ⇒ `from==to`, `fee==0`, `amount!=0`). | `dag` | tests : 2 CLAIM même membre → 2ᵉ `CONFLICT`, `count` inchangé ; CLAIM de membres différents coexistent ; convergence digest (A→B == B→A) |
| **C4** | Auto-émission : après import réussi (B4 `handle_join_offer`) OU au boot si membre & pas de CLAIM `from==moi` dans le DAG → construire CLAIM (`amount=initial_credit`, `seq=0`), commit local + propagation (DAG sync). `initial_credit==0` ⇒ skip. Fonction runtime testable + câblage `main/`. | `app_main` + `main/` | tests **bridged** : B rejoint A → B s'auto-crédite → `balance(B)==initial_credit` ; A voit le solde de B après sync ; idempotence (pas de 2ᵉ CLAIM) ; `initial_credit==0` → aucune CLAIM |

## 4. Flux de bout en bout

1. Rejointe (Palier B) : le membre importe le descripteur → devient membre.
2. C4 vérifie « CLAIM `from==moi` dans mon DAG ? » → non.
3. Construit la CLAIM (`type=CLAIM`, `from=to=moi`, `amount=initial_credit`, `seq=0`,
   `fee=0`), la signe, la **committe localement** (le solde local passe à `initial_credit`).
4. **Propagation** via DAG sync (la CLAIM est une TX ordinaire du DAG).
5. Les pairs reçoivent la CLAIM, la valident (C2 : montant exact, signature membre, plafond,
   unicité `(from,0)`) et créditent le solde du membre chez eux.
6. Reboot : le DAG persisté contient la CLAIM → étape 2 renvoie « oui » → pas de
   re-émission ; toute re-émission accidentelle est rejetée (DUPLICATE/CONFLICT).

## 5. Hors périmètre C (rappels & dépendances)

- **Élagage / checkpoint (Phase B)** = chantier séparé **non démarré**
  (`chantier_phase_b_checkpoint.md`). Tant qu'il n'existe pas, aucune TX n'est évincée : la
  CLAIM reste dans le DAG et l'unicité `(from, seq)` tient. **Dette connue** : quand la
  Phase B introduira l'élagage, le **set des membres ayant réclamé devra survivre au
  checkpoint** (sinon re-réclamation possible après éviction). C'est une **exigence de la
  Phase B**, à câbler avec elle — pas dans C.
- **UI** (affichage du solde/crédit, wizard) = Palier D. C fournit la logique headless.
- **Politique multi-monnaie / révocation de membre / vouchers nominatifs** = hors v1
  (l'octroi est universel et borné par `max_supply`).

## 6. Ordre d'implémentation proposé

C1 (type CLAIM, logique pure, TDD) → C2 (validation currency) → C3 (unicité DAG) →
C4 (runtime auto-émission + bridged test). Validation banc on-device au fil de l'eau ;
chaque sous-palier committé séparément (façon A/B). Tests ESP-NOW seulement.
