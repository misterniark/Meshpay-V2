# Chantier — Nettoyage des transactions legacy (currency_id étranger)

> Statut : **TERMINÉ — validé banc + réel le 2026-07-15** (286 tests/0 échec ;
> purge de 10 tx observée sur les 4 devices, `skipped_foreign=10` capté pendant
> la transition, mesh convergée à 16 tx en digest exact (`conv=1`), soldes
> inchangés 12/8/7/4 + orphelin 1 = 32 au re-dump). Créé le 2026-07-15 (audit
> comptable de la DAG « minimes »).
> Priorité : **P2 salubrité** (aucun impact sur les soldes — les calculs filtrent par
> `currency_id`). Couplé au chantier `chantier_durcissement_ingestion.md` (le filtre
> d'ingestion N2 en est la première marche concrète).

## 0. N0 — préalable découvert à l'implémentation : parents hors fenêtre

L'analyse des liens de parenté du dump (script `check_parents`) a montré que les
**10 tx legacy sont TOUTES des ancêtres de tx actives** (8 liens directs : les
CLAIM/paiements de « minimes » ont pris les MINT legacy comme parents — c'étaient
les tips du moment — et les parents font partie du contenu signé, irréversible).
Or `meshpay_dag_validate_merge` refusait toute tx au parent absent
(`MISSING_PARENT`) : purge impossible, et le filtre N2 seul aurait BLOQUÉ la
convergence des futurs membres vierges (8 tx actives refusées faute de parents).

**Décision utilisateur (2026-07-15) : les parents absents ne sont plus bloquants
au merge** — référence pendante tolérée, jamais déréférencée (tips et conflits ne
font que comparer des id). Ce n'était pas une barrière de sécurité (l'ingestion ne
vérifie pas les signatures, les tips sont publics dans les summaries) mais une
contrainte d'ordre d'application ; la Phase B (checkpoint fenêtre glissante)
l'aurait de toute façon levée. `MESHPAY_DAG_MERGE_MISSING_PARENT` n'est plus
jamais émis (enum conservé). Bénéfice collatéral : supprime un mode de rejet
résiduel des paiements directs (tx du payeur aux parents pas encore syncés,
course jumelle du Palier F1).

## 1. Problème (constat de terrain)

Le dump de la partition `dagstore` du T-Deck fondateur (2026-07-15, DAG convergée à
26 tx sur les 4 devices) montre que la fenêtre partagée transporte **10 transactions
d'une « monnaie » morte**, `currency_id=1` — la config de repli d'avant les
descripteurs :

- 9 MINT auto-adressés de 10 (l'auto-émission « boot credit » du schéma historique
  C4, émise par chaque identité ayant booté sans descripteur — dont 5 identités
  fantômes détruites depuis par les erase/reflash) ;
- 1 TRANSFER de test (`d89a8145 → 63dc99b9`, amount=3, seq=20) entre deux de ces
  fantômes.

Ces tx sont **inertes mais immortelles** :

| Effet | Détail |
| --- | --- |
| Slots gaspillés | 10 des 250 slots de la fenêtre (`meshpay_dag_t`), soit ~2,3 Ko de RAM et de flash (`dag_store`) par device |
| Sync perpétuelle | comptées dans `tx_count` des summaries, incluses dans les digests et les batchs — re-propagées à chaque nouveau membre, pour toujours |
| Checkpoint pollué | elles rapprochent du seuil `MESHPAY_DAG_CHECKPOINT_THRESHOLD` (200) sans porter aucune valeur pour la monnaie active |
| Bruit d'audit | tout dépouillement comptable (cf. § 5) doit les écarter à la main |

Rien ne les purge : `meshpay_dag_merge_tx` accepte tout `currency_id` (l'ingestion ne
valide rien, P0 connu) et les calculs de solde se contentent de les ignorer.

## 2. Objectif

Sous une monnaie à descripteur (`has_descriptor`), la DAG locale ne contient QUE des
tx de la monnaie active ; les tx d'un `currency_id` étranger sont purgées de
l'existant ET refusées à l'ingestion. En config de repli (pas de descripteur), rien ne
change : `currency_id=1` y est la monnaie légitime.

**Invariant de couplage** : la purge locale (N1/N3) sans le filtre d'ingestion (N2)
est une pompe à ré-infection — le premier batch d'un pair non purgé réinjecte tout.
N2 se déploie avec ou avant N3, jamais après.

## 3. Décomposition

### N1 — API de purge (composant `dag`)

`size_t meshpay_dag_purge_foreign(meshpay_dag_t *dag, uint32_t currency_id)` :
retire toutes les tx dont `tx->currency_id != currency_id`, retourne le nombre
purgé. Compactage en place, **ordre relatif des survivantes préservé** (l'ordre
d'insertion diffère déjà entre devices ; la purge ne doit pas créer un nouveau mode
de divergence). Tests Unity : DAG mixte, DAG déjà propre (0 purgée, no-op), DAG
vide, tips/digest recalculés cohérents après purge.

### N2 — Filtre d'ingestion (première marche du chantier P0)

Dans `meshpay_dag_sync_apply_batch` (ou son appelant `runtime_handle_dag_resource`),
sous `has_descriptor` : toute tx décodée dont `currency_id != config.currency_id`
est comptée `skipped` et n'atteint jamais `meshpay_dag_merge_tx`. Log récapitulatif
(`dag resource merged=X skipped_foreign=Y`). Même filtre dans le chemin de paiement
direct si absent. Tests : batch mixte → seules les tx de la monnaie active mergées ;
config de repli → tout passe (comportement actuel intact).

> Note : ce filtre est volontairement MINIMAL (un `uint32` comparé). La vérification
> de signature/règles éco à l'ingestion reste le périmètre du chantier P0 — ne pas
> l'embarquer ici.

### N3 — Câblage runtime

Purge appelée : (a) au boot, après la restauration `dag_store` et AVANT le premier
announce/summary — `dag_store_save(reason="purge")` immédiat (les tâches et leur
débounce ne tournent pas encore) ; (b) dans `runtime_import_currency_descriptor`,
goulot commun aux TROIS chemins d'ancrage (création fondateur, rejointe par code,
rejointe par découverte) — persistance par le débounce habituel (`mark_dirty`), la
purge étant idempotente au boot suivant. Le monitor H752
(`MESHPAY_DAG_MONITOR_ONLY`, pas de monnaie active) ne purge rien : il observe
tout. Note : le chemin de paiement direct était DÉJÀ filtré
(`validate_tx` → `ERR_WRONG_ID` en tête), rien à ajouter.

### N4 — Outillage de diagnostic versionné

Verser le décodeur de session au repo : `scripts/decode_dagstore.py` (dump esptool →
parse des 2 slots A/B, gen max, décodage `meshpay_tx_t` avec auto-détection du
`record_size` 224/232, comptabilité par monnaie : masse émise, soldes bruts vs
planchés, conservation, comptes orphelins). C'est l'outil qui a produit le constat
du § 1 ; il servira de preuve avant/après pour N5 et pour tout audit futur.
Documenter l'usage en tête du script (T-Deck = flash claire ; les Waveshare chiffrés
ne sont PAS dumpables — le T-Deck convergé est la sonde du réseau).

### N5 — Banc + validation réelle

- Banc Unity complet (les suites `dag`, `dag_sync`, `app_main` couvrent N1-N3).
- Réel (4 devices, « minimes ») : flash, vérifier par sonde série
  `skipped_foreign` puis convergence à **16 tx** (26 − 10) sur les 4 ; re-dump du
  T-Deck : plus aucune tx `currency_id=1`, soldes inchangés (12/8/7/4 + 1 orphelin
  `199e5066` — celui-ci est de la monnaie active, il RESTE, cf. § 4).

## 4. Hors périmètre (décisions actées le 2026-07-15)

- **Le minime orphelin** (compte `199e5066`, clé perdue) : c'est une tx VALIDE de la
  monnaie active — la purge ne le touche pas. Toute « récupération » passerait par
  une tx forgée via le trou P0 : refusé (bombe à retardement au durcissement).
  Le filtrage des cibles de paiement (F2, `dc2f78e`) empêche la récidive côté UI ;
  exiger `to` membre dans `validate_tx` est une question ouverte pour le chantier P0.
- **Compaction/checkpoint > 200** : Phase B (`chantier_phase_b_checkpoint.md`),
  indépendante.
- **Multi-monnaies** : mono-monnaie par design ; si ça change, le filtre N2 devient
  une liste de `currency_id` autorisés — l'API N1 (paramètre explicite) est déjà
  compatible.

## 5. Références

- Dump et dépouillement : session du 2026-07-15 (`decode_dagstore.py`,
  `dagstore_tdeck.bin`, slot gen=16, 26 tx). Comptabilité vérifiée : masse 32
  conservée exactement (4 CLAIM × 8), aucun frais brûlé.
- `chantier_durcissement_ingestion.md` — P0 parent du filtre N2.
- Mémoire projet : `palierF-paiements-membres.md` (F2, filtrage des pairs).

## 6. Critères de fin — TOUS VALIDÉS (2026-07-15)

1. ✅ Banc Unity complet : 286 tests, 0 échec (bench_n1).
2. ✅ Les 4 devices convergent à 16 tx en digest EXACT (`conv=1` croisé — le
   digest étant indépendant de l'ordre, la convergence post-purge est stricte,
   pas heuristique) ; transition observée : le T-Deck flashé seul face aux 3
   Waveshare non flashés a loggé `dag resource merged=0 skipped_foreign=10
   total=16` sur un batch de 26 tx.
3. ✅ Re-dump T-Deck (gen=17) : 16 tx toutes en 0xc5c42609, 8 tx aux parents
   hors fenêtre (tolérés), soldes inchangés 12/8/7/4 + orphelin 1, total 32.
4. ✅ `scripts/decode_dagstore.py` versionné, validé sur les dumps avant/après.
