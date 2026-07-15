# Chantier — Phase B : checkpoint élagueur de la DAG (> 200 TX)

> Statut : **CODE VALIDÉ AU BANC (323/0) le 2026-07-15 — validation réelle fenêtre réduite en attente (§ 12)**
> (décisions § 11 : checkpoint signé fondateur,
> annuaire embarqué, coupe totale refondatrice, seuils Kconfig) · Créé le 2026-06-29
> Pré-requis livré : **Phase A** (persistance DAG sur flash, composant `dag_store`,
> validée banc 2026-06-29 — cf. `protocole_test_dag.md` §11.x et la mémoire
> `dag-persistance-flash-phase-a`).
> Suite : **Phase C** (durcissement — re-vérif signatures au load, cadence anti-usure).

⚠️ **Avertissement de portée** : contrairement à la Phase A (purement locale, sans
impact réseau), la Phase B **touche la couche consensus** (la synchro DAG). Le risque
est de **déstabiliser la convergence** durement acquise (cf. §11.4 « fork RÉSOLU »).
À planifier finement et à valider au banc avant de considérer le sujet clos.

---

## 1. Pourquoi ce chantier (l'enjeu)

La fenêtre DAG est **bornée à 250 TX** en dur :

```c
// components/dag/include/meshpay/dag.h
#define MESHPAY_DAG_MAX_TRANSACTIONS 250
#define MESHPAY_DAG_CHECKPOINT_THRESHOLD 200
```

```c
// components/dag/dag.c:112-113  (meshpay_dag_merge_tx)
if (dag->count >= MESHPAY_DAG_MAX_TRANSACTIONS) {
    return MESHPAY_DAG_MERGE_FULL;     // <-- plafond dur
}
```

**Conséquence en production : à la 250ᵉ transaction, le portefeuille se bloque
définitivement** — plus aucune TX (locale ou entrante) n'est acceptée. Il n'y a
**aucun mécanisme d'élagage** : le seuil 200 existe mais son détecteur est un **stub
jamais appelé** :

```c
// components/dag/dag.c:69-71
bool meshpay_dag_needs_checkpoint(const meshpay_dag_t *dag)
{
    return dag != NULL && dag->count >= MESHPAY_DAG_CHECKPOINT_THRESHOLD;
}
```

> Historique : la fonctionnalité existait en V1 (`../Mesh Pay/main/persistence/
> ledger_store.*`) mais n'a pas été reportée à la réécriture V2. La Phase A a rétabli
> la **persistance** ; la Phase B doit rétablir l'**élagage** pour borner la fenêtre
> ET la partition `dagstore` (256 Ko).

---

## 2. Les trois problèmes à résoudre

### 2.1 Conservation des soldes (snapshot)

Le solde se calcule en **parcourant TOUTE la DAG** :

```c
// components/currency/currency.c:88 (meshpay_currency_get_balance)
for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
    const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
    // crédits MINT, crédits/débits TRANSFER, fee -> mint_authorities[0]
}
```

Si on élague d'anciennes TX, leur effet sur les soldes **disparaît** et tous les
soldes deviennent faux. Il faut donc, au moment de l'élagage, **figer un snapshot de
soldes par compte** au point de coupe, et rendre `get_balance` = `snapshot[compte] +
contribution de la fenêtre restante`.

### 2.2 Intégrité des parents (élagage du graphe)

Chaque TX référence jusqu'à 2 parents :

```c
// components/meshpay_tx/include/meshpay/meshpay_tx.h
#define MESHPAY_TX_MAX_PARENTS 2
uint8_t parents[MESHPAY_TX_MAX_PARENTS][MESHPAY_TX_PARENT_ID_SIZE];
```

Une TX entrante dont un parent est absent est rejetée `merge=MISSING_PARENT`.
Élaguer une TX qui est encore parent d'une TX conservée **orphelinerait** cette
dernière. Il faut une **frontière d'élagage** : n'élaguer qu'une TX assez ancienne
pour n'être **parent d'aucune TX de la fenêtre conservée** (et jamais un tip).

> Atout : les nouvelles TX sélectionnent comme parents les **tips courants** (les plus
> récents), donc elles ne référencent jamais les plus anciennes. L'élagage des plus
> vieilles ne casse donc pas les TX entrantes — le risque est uniquement vis-à-vis des
> TX **déjà dans la fenêtre** qui pointeraient vers une TX en cours d'élagage.

### 2.3 Le piège MAJEUR — élaguer une DAG **répliquée** vs la synchro

C'est le point dur, propre à un registre distribué. La DAG sync **diffuse toute la
fenêtre** (choix retenu pour résoudre le fork, §11.4 : `known=0`, batch depuis le
début). Scénario du thrashing :

1. Le nœud **A** élague les TX 1-50 (il est à 200, il coupe).
2. Le nœud **B** n'a pas encore élagué (il est à 180).
3. À la prochaine sync, A constate qu'il « manque » 1-50 par rapport à B → il les
   **re-demande** → B les **re-livre** → A les **re-merge** → **élagage annulé**.
4. A regrimpe à 250 → ré-élague → re-reçoit… **boucle infinie**.

→ **La Phase B impose de rendre la synchro « checkpoint-aware »** : les pairs doivent
s'accorder sur un **point de coupe commun** (« horizon ») en-deçà duquel on ne
re-livre rien. C'est une modification de la **couche consensus**, pas un simple ajout
local.

---

## 3. Décomposition proposée (à valider avant de coder)

| # | Sous-tâche | Composant(s) | Risque |
|---|---|---|---|
| B1 | **Format de snapshot de soldes** `{account_hash[16] → balance(u32)}`, sérialisé/désérialisé, borné. | `currency` (+ nouveau type) | moyen |
| B2 | **`get_balance` checkpoint-aware** : part du snapshot + ne parcourt que la fenêtre restante. | `currency` (API change, nombreux appelants) | moyen |
| B3 | **Élagage du graphe** : calcul de la frontière (TX élaguables sans orpheliner ni perdre un tip), repli de leur effet dans le snapshot, compactage de `transactions[]`. | `dag` | **élevé** |
| B4 | **Persistance du checkpoint** : étendre `dag_store` (ou un slot dédié) pour stocker `{snapshot, horizon, digest}` à côté de la fenêtre. Les 512 o du record NVS (`MESHPAY_STORAGE_CHECKPOINT_MAX`) sont **insuffisants** (ils ne tiennent aujourd'hui que le MINT de boot). | `dag_store` / `storage` | moyen |
| B5 | **Sync checkpoint-aware** : porter l'`horizon`/`checkpoint_seq` dans le `SUMMARY` ; un pair ne re-livre pas en-deçà de l'horizon de l'autre ; réconciliation des horizons (prendre le plus avancé sur lequel tous s'accordent). | `dag_sync`, `main/app_main.c` | **élevé (consensus)** |
| B6 | **Câbler `needs_checkpoint`** : au seuil 200, déclencher B3→B4, puis sauver. | `app_main` (runtime) | faible |

---

## 4. Décisions de conception à trancher (AVANT de coder)

1. **Quand élaguer** — au seuil 200 (marge de 50 avant le plafond 250) : OK ? Combien
   de TX couper d'un coup (p. ex. ramener à 100 pour éviter d'élaguer à chaque TX) ?
2. **Frontière d'élagage** — par profondeur de graphe (n'élaguer que ce qui n'est
   parent d'aucune TX conservée) ou par `seq`/ordre d'arrivée ? La première est sûre
   mais plus coûteuse à calculer.
3. **Coordination de l'horizon** — qui décide du point de coupe commun ? Proposition :
   l'horizon = un point que **toutes les cartes en ligne** ont dépassé (élaguer
   seulement ce qui est « profondément finalisé » et observé partout via le SUMMARY).
   C'est le cœur du risque consensus.
4. **Borne du snapshot** — nombre max de comptes ? Sur un petit mesh c'est petit ;
   en général borné par le nombre de comptes distincts vus. Définir une taille max et
   le comportement au dépassement.
5. **Format de stockage** — réutiliser le double-buffer `dag_store` (un 3ᵉ blob) ou
   une zone dédiée dans la partition `dagstore` (256 Ko, large) ?

---

## 5. Dépendance de validation : l'injecteur (§4.1)

Valider Phase B au banc exige de **dépasser 200 TX**, ce qui est **irréaliste à la
main** (200+ paiements tactiles). Cela nécessite l'**injecteur ESP-NOW** décrit au
`protocole_test_dag.md` §4.1 (5ᵉ carte « attaquant »), aujourd'hui **non construit**.

→ Conséquence : la **logique** de Phase B se valide en **TDD unitaire** (forçage du
seuil dans les tests), mais la **validation HW complète** est **bloquée par
l'injecteur**. L'injecteur débloque aussi la **Phase 2** (anti-corruption) et la
variante checkpoint de la **Phase 3**.

---

## 6. Plan d'attaque recommandé

Trois options, par ordre de préférence :

1. **Construire d'abord l'injecteur (§4.1)** — débloque la validation HW de Phase B
   *et* la Phase 2 *et* la variante Phase 3 ; puis Phase B en TDD + banc complet.
2. **Phase B en TDD pur** — logique testée unitairement (seuil forcé), validation HW
   différée jusqu'à l'injecteur. Permet d'avancer le code sans la 5ᵉ carte.
3. **Différer** — Phase A (livrée + prouvée) suffit pour la démo court terme ; Phase B
   en session dédiée quand le sujet consensus pourra être traité à froid.

> Recommandation : option 1 si on veut un résultat **prouvé au banc** ; option 2 si on
> veut avancer le code immédiatement en assumant une validation HW différée. **Ne pas**
> coder B5 (sync checkpoint-aware) sans avoir tranché les décisions §4.3 — c'est là que
> se joue la stabilité de la convergence.

---

## 7. Références code (points d'entrée du chantier)

- Plafond / stub : `components/dag/dag.c:69` (`needs_checkpoint`), `:112` (`MERGE_FULL`).
- Constantes : `components/dag/include/meshpay/dag.h:13-15` (250 / 200 / 32 tips).
- Calcul de solde à rendre checkpoint-aware : `components/currency/currency.c:88`.
- Persistance existante (Phase A) à étendre : `components/dag_store/` + câblage runtime
  `components/app_main/app_main_logic.c` (`runtime_dag_flush`) + boot `main/app_main.c`.
- Checkpoint NVS actuel (insuffisant, 512 o) : `components/storage/include/meshpay/storage.h:15,55-58`.
- Protocole de test & injecteur : `Doctech V2/protocole_test_dag.md` §4.1, §7, §11.
- Outillage test : `Doctech V2/plan_outillage_test_dag.md`.
- Checklist préprod liée : `Doctech V2/preprod.md` (« finaliser les checkpoints automatiques »).

---

## 8. RÉVISION 2026-07-15 — ce que les chantiers N/I/M/U ont changé

Le §§ 1-7 ci-dessus datent du 2026-06-29. Quatre chantiers sont passés depuis
et modifient substantiellement le problème :

### 8.1 Prémisses PÉRIMÉES

- **§ 2.2 (intégrité des parents) : caduc.** Depuis le chantier nettoyage
  currency legacy (0439883), un parent ABSENT est toléré au merge — référence
  pendante, jamais déréférencée ; `MISSING_PARENT` n'est plus jamais émis.
  L'élagage n'orpheline plus personne : la « frontière d'élagage » n'est plus
  une contrainte de graphe. **Le § 2.3 (thrashing de sync) reste LE problème
  central**, aggravé d'un détail : le digest porte sur la fenêtre ENTIÈRE —
  deux nœuds aux fenêtres décalées divergent en digest → `conv=0` → resync
  perpétuelle. L'horizon commun n'est pas une option, c'est la condition de
  convergence.
- **Le « checkpoint » NVS du record storage** (512 o, `has_checkpoint`) n'a
  RIEN à voir : c'est le boot-credit MINT du repli sans descripteur (C4). La
  Phase B persiste dans la partition `dagstore` (256 Ko), pas dans la NVS.

### 8.2 Exigences NOUVELLES

- **L'annuaire des clés (chantier durcissement ingestion)** : les CLAIM
  portent `member_public` — élaguer la CLAIM d'un membre rend TOUTES ses tx
  futures invérifiables (`UNKNOWN_MEMBER` définitif : forge indistinguable).
  Le checkpoint DOIT transporter l'annuaire (hash compte → clé publique),
  sinon les CLAIM sont incompressibles et une fenêtre de 250 sature à ~250
  membres sans un seul paiement.
- **La vérifiabilité (même chantier)** : la sync actuelle ne livre QUE des tx
  individuellement signées, vérifiées par le gate. Un snapshot de soldes est
  un AGRÉGAT : personne ne l'a signé. L'échanger sans racine de confiance
  ouvre la forge de soldes — pire que le problème d'origine. Il faut une
  autorité de checkpoint (voir § 9).
- **L'anti-rejeu** : l'unicité (from,seq) est vérifiée contre la fenêtre.
  Élaguer les vieilles tx ré-ouvre le REJEU d'une tx d'avant-horizon (même
  (from,seq), re-livrée par un pair malveillant). Le checkpoint doit porter
  le **seq plancher par compte** (toute tx de seq ≤ plancher est refusée).

### 8.3 Opportunités NOUVELLES

- **Format CBOR tolérant éprouvé (chantier migration NVS)** : le blob
  checkpoint naît en CBOR à clés entières (préfixe magic+version), tolérant
  aux deux sens — jamais de breaking de schéma. Répond aussi à la question
  laissée au § 8 du chantier migration (le snapshot fenêtre de dag_store
  reste en struct brute : SON alignement reste hors scope ici).
- **Validation réelle SANS injecteur** : rendre la fenêtre et le seuil
  configurables (Kconfig, défauts 250/200) permet un banc RÉEL à fenêtre
  réduite (p. ex. 20/16) : une poignée de paiements à l'écran déclenche un
  vrai cycle checkpoint→élagage→convergence sur la flotte. L'injecteur
  (§ 5) reste utile pour la charge, plus bloquant pour la Phase B.

## 9. Décision d'architecture à trancher — l'autorité du checkpoint

### Option A — Checkpoint SIGNÉ PAR LE FONDATEUR (recommandée)

Le fondateur — déjà racine de confiance du système (descripteur signé,
autorité MINT unique, collecteur des frais) — émet au seuil un CHECKPOINT
signé de sa clé d'identité : `{currency_id, horizon (count+digest), par
compte : hash, solde, seq plancher, clé publique (annuaire), génération}`.
Les membres le VÉRIFIENT contre la clé du descripteur (préimage déjà en
main), l'adoptent, élaguent le préfixe ≤ horizon. La sync devient
horizon-aware (SUMMARY porte la génération ; on ne re-livre rien sous
l'horizon adopté).

- ✅ Vérifiable par la racine de confiance EXISTANTE (zéro nouveau modèle) ;
  bootstrap d'un nouveau membre = checkpoint signé + fenêtre ; thrashing
  résolu par construction (l'horizon est un fait signé, pas une négociation) ;
  cohérent avec le produit (monnaie locale mono-fondateur).
- ❌ Fondateur durablement hors ligne ⇒ plus de nouveaux checkpoints : la
  fenêtre re-sature à 250 et le mesh repasse en lecture seule (comportement
  actuel) jusqu'à son retour. Limite ASSUMÉE du design mono-fondateur — même
  dépendance que les frais et le MINT.

### Option B — Horizon implicite (min des counts observés dans les SUMMARY)

Sans signature ni snapshot échangé : chaque nœud élague ce que TOUS les pairs
vus ont dépassé.

- ✅ Décentralisé, pas de nouveau wire.
- ❌ Invérifiable pour un nouveau venu (il doit croire les soldes implicites) ;
  « tous les pairs vus » est indécidable (pair éteint = horizon gelé, pair
  fantôme = fenêtre saturée) ; digests transitoirement divergents pendant les
  coupes = exactement le thrashing qu'on veut tuer.

### Option C — Différer (statu quo Phase A)

La fenêtre 250 suffit à la démo court terme ; blocage dur à la 250e tx.

**Recommandation : Option A.** C'est la seule qui préserve les acquis du
chantier durcissement (tout ce qui entre est vérifiable contre une racine de
confiance) sans inventer un consensus multi-parties hors de portée du
produit.

## 10. Décomposition révisée (remplace le § 3)

| # | Palier | Contenu | Risque |
|---|---|---|---|
| P1 | Format + signature | Type checkpoint (comptes bornés Kconfig), CBOR encode/decode (préfixe magic+version, clés entières), sign/verify fondateur, tests vecteurs + forge/altération | moyen |
| P2 | Application locale | `dag_apply_checkpoint` (élagage préfixe ≤ horizon + compactage + digest post-horizon) ; `get_balance` = solde checkpoint + fenêtre ; annuaire = checkpoint ∪ CLAIM fenêtre ; anti-rejeu = seq plancher ; unicité (from,seq) scopée post-horizon | **élevé** |
| P3 | Persistance | 3e blob CBOR « checkpoint » dans la partition dagstore (à côté du double-buffer fenêtre), restauration boot AVANT la sync | moyen |
| P4 | Émission + diffusion + sync | Fondateur : émission au seuil (checkpoint sur SA DAG) ; wire : message dédié + Resource ; membres : vérif + adoption ; SUMMARY porte la génération d'horizon ; REQUEST/BATCH ne servent plus rien sous l'horizon ; re-demande du checkpoint à la rejointe/au retard | **élevé (consensus)** |
| P5 | Validation | Banc TDD (seuil forcé) + Kconfig fenêtre réduite (20/16) + banc RÉEL : paiements à l'écran jusqu'au cycle complet checkpoint→élagage→convergence→nouveaux paiements ; comptabilité conservée au dump | moyen |

Décisions d'accompagnement : seuils Kconfig (`MESHPAY_DAG_WINDOW`,
`MESHPAY_DAG_CHECKPOINT_THRESHOLD`, borne de comptes par checkpoint) ;
génération monotone (u32) comme identité d'horizon ; le fondateur conserve
l'HISTORIQUE de son propre horizon ? Non — le checkpoint N+1 se calcule sur
{checkpoint N + fenêtre}, par récurrence (le fondateur n'a jamais besoin de
plus que sa DAG courante).

## 11. Décisions actées (2026-07-15) + sémantique de coupe

Décisions utilisateur : **Option A (checkpoint signé fondateur)**, **annuaire
dans le checkpoint**, **seuils Kconfig** (banc réel à fenêtre réduite).

### La coupe totale refondatrice (décision de conception)

Le digest DAG est indépendant de l'ordre local (tri par ID avant hash,
`dag.c:271`) : la coupe DOIT être désignée par CONTENU. La désignation
retenue : **un plancher de seq par compte** — une tx est sous l'horizon ssi
`tx.seq <= seq_floor(tx.from)`. Déterministe, indépendant de l'ordre local,
et c'est déjà l'anti-rejeu (toute tx re-livrée sous plancher est refusée).

Et la coupe est TOTALE : `seq_floor[c] = max seq observé de c` au moment de
l'émission — le checkpoint est un **re-genesis signé** : l'état complet
(soldes, planchers, annuaire) est refondé, la fenêtre repart VIDE avec 100 %
de marge. Aucune ambiguïté de frontière partielle ; les toutes premières tx
post-checkpoint référencent des tips élagués = parents pendants, TOLÉRÉS
depuis le chantier nettoyage (la décision N0 rend cette coupe possible).
Rien n'est perdu comptablement : tout ce qui disparaît de la fenêtre est
dans l'état signé.

Conséquences à câbler :
- gate d'ingestion : refuser toute tx `seq <= seq_floor(from)` (rejeu) ;
- auto-CLAIM C4 : « déjà claimé » = ma CLAIM dans la fenêtre OU mon compte à
  l'annuaire du checkpoint (sinon re-CLAIM à tort après élagage) ;
- unicité (from,seq) : scopée à la fenêtre post-horizon + plancher ;
- le fondateur n'apparaît à l'annuaire qu'avec une clé nulle (« voir
  descripteur ») — sa clé est déjà la racine de confiance ;
- balances : `solde = checkpoint[compte] + contribution de la fenêtre` ;
- membership (F2) : membre = annuaire du checkpoint ∪ CLAIM de la fenêtre.

## 12. Validation (2026-07-15) — banc complet ; réel à fenêtre réduite EN ATTENTE

**Banc on-device : 323 tests, 0 échec** (315 + 8 nouveaux) :
- P1 `[checkpoint]` : round-trip signé via wire, forge/altérations (solde,
  plancher, clé d'annuaire, imposteur) refusées, malformations (gén 0,
  doublons, magic/version/orphelins) refusées ;
- P2 `[currency][p2]` : cycle build→sign→adopt — coupe TOTALE (fenêtre vide),
  soldes/total_minted/membres/annuaire INVARIANTS à travers la coupe,
  ERR_REPLAY sur CLAIM et paiement rejoués, la vie continue post-horizon
  (seq au-dessus du plancher, parents pendants), génération 2 par récurrence ;
  adoption monotone (jamais de retour en arrière) ;
- P3 `[dag_store][p3]` : round-trip du blob signé en queue de partition,
  élection de la génération la plus haute, coexistence avec la fenêtre —
  le mock a imposé l'alignement 16 o de la flash CHIFFRÉE (une écriture du
  premier jet aurait échoué sur les Waveshare réels : buffer unique paddé) ;
- P4 `[app_main][p4]` : un summary de pair en RETARD DE GÉNÉRATION déclenche
  le push du re-genesis (rate-limité 5 s) ; l'adoption par fragments Resource
  (réassemblage → vérif contre la clé du descripteur → purge → solde conservé
  → digests convergents → rejeu no-op). BUG attrapé par le test : la
  génération se juge AVANT la convergence de digest (deux fenêtres vides sous
  des générations différentes ont des soldes différents — sans ce fix, un
  pair retardataire à fenêtre vide n'aurait JAMAIS reçu le checkpoint).

Firmwares T-Deck et Waveshare compilés avec les défauts 250/200 : le code
checkpoint est INERTE tant que la fenêtre réelle n'atteint pas le seuil — le
flash de flotte est sans risque de déclenchement intempestif.

Dette de harnais documentée en passant : les tests app_main ne détruisaient
JAMAIS leurs runtimes (queues fuitées) — `meshpay_app_runtime_destroy`
existait sans être appelé ; les 2 tests P4 l'appellent désormais, le tail de
la suite respire. Généraliser aux ~20 autres tests = chantier harnais.

**Reste pour clore : la validation réelle à fenêtre réduite** (protocole
§ 10-P5) : builds 20/16 des deux cibles (Kconfig), flash ×4 (la DAG de 10 tx
et le record v3 se rechargent tels quels), ~6 paiements à l'écran →
émission gen 1 chez le fondateur → adoption/purge sur la flotte → soldes
intacts au dump → paiements post-horizon → reboot (persistance du
checkpoint) → re-flash en 250/200 (le checkpoint adopté persiste, fenêtre
normale). Session interactive (paiements à l'écran).
