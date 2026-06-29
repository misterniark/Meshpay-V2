# Chantier — Phase B : checkpoint élagueur de la DAG (> 200 TX)

> Statut : **PROCHAIN CHANTIER — non démarré** · Créé le 2026-06-29
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
