# Chantier — Durcissement de la validation à l'ingestion (sync DAG)

> Statut : **identifié, non démarré** · Créé le 2026-07-14 (revue adversariale du Palier C).
> Priorité : **P0 sécurité** (voir `preprod.md`). Couplé au consensus / Phase B checkpoint.

## 1. Problème

Le chemin d'ingestion réseau des transactions par la **synchronisation DAG** merge les tx
reçues **sans vérifier ni la signature ni les règles économiques** :

```
runtime_rx_packet (context RESOURCE)
  → runtime_handle_dag_resource           (app_main_logic.c)
    → meshpay_dag_sync_apply_batch         (dag_sync.c)
      → meshpay_tx_decode + meshpay_dag_merge_tx   ← SEULE barrière = tx_shape_valid
```

Aucun appel à `meshpay_tx_verify` (signature Ed25519 contre la clé publique du `from`)
ni à `meshpay_currency_validate_tx` (règles éco). `meshpay_tx_decode` termine par
`validate_common(tx, true)` qui n'exige que des `id`/`signature` **non nuls**, jamais une
signature **valide**.

Conséquence : un pair à portée peut forger une tx (id/signature arbitraires non nuls) et
l'injecter dans un BATCH ; elle est mergée puis re-diffusée à tout le réseau.

**Ce n'est PAS une régression du Palier C** : c'est vrai pour **tous** les types
(TRANSFER/MINT/CLAIM) depuis l'origine de la sync. Le Palier C l'a mis en lumière car la
CLAIM crée de la valeur ex nihilo.

## 2. Défenses en aval actuelles (partielles, insuffisantes)

Aucune ne remplace la vérif de signature — elles bornent seulement l'impact comptable :

| Type | Défense en profondeur (couche `currency`) | Reste exploitable ? |
| --- | --- | --- |
| MINT | `get_balance`/`total_minted` exigent `is_mint_authority(from)` | Oui si `from == hash fondateur` (public) |
| TRANSFER | double entrée (un crédit forgé débite un `from`) | Oui (crédit net au `to` de l'attaquant) |
| CLAIM | `amount == initial_credit` exigé (ajouté au Palier C, 2026-07-14) | Borné au modèle Sybil (≤ initial_credit/compte), mais inflation d'obfuscation possible |

## 3. Correctif visé

À l'`apply_batch` (ou dans un pré-filtre en amont du merge), pour chaque tx décodée :

1. **Signature** : résoudre l'identité publique du `from` (table `known_destinations` /
   announces) puis `meshpay_tx_verify`. Rejeter si non résolue ou signature invalide.
2. **Règles éco** : `meshpay_currency_validate_tx` (montant, autorité MINT, plafond,
   `amount==initial_credit` pour CLAIM).

## 4. Difficultés (pourquoi c'est du consensus, pas un patch local)

- **Ordre / multi-passes** : `apply_batch` applique en plusieurs passes pour tolérer un
  ordre non topologique (enfant avant parent). Une validation éco (solde, plafond) dépend
  de l'état du DAG au moment de l'application → ne peut pas rejeter une tx dont le parent
  n'est pas encore appliqué sans casser la reconciliation sous fork.
- **Résolution d'identité** : la vérif de signature exige la clé publique du `from`. Si le
  `from` est inconnu (pas encore d'announce reçu), que fait-on ? File d'attente ? Rejet
  temporaire ? Cela introduit un état.
- **Cohérence avec le checkpoint (Phase B)** : l'élagueur > 200 TX doit conserver assez
  d'état (soldes, set des membres ayant réclamé) pour re-valider — voir
  `chantier_phase_b_checkpoint.md`.

## 5. Décision (2026-07-14)

Différé et traité **avec la Phase B consensus/checkpoint**, pas dans le Palier C. Le Palier
C se contente de la défense en profondeur comptable de la CLAIM (`amount==initial_credit`)
qui ramène l'exposition CLAIM au **modèle Sybil déjà assumé** (borné par `max_supply`,
réglé par le fondateur). Voir `preprod.md` §Priorité 0.
