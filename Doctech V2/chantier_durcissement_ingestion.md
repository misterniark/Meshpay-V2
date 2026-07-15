# Chantier — Durcissement de la validation à l'ingestion (sync DAG)

> Statut : **VALIDÉ — banc 293/0 + validation réelle 4 devices le 2026-07-15 (voir § 7)** ·
> Créé le 2026-07-14 (revue adversariale du Palier C).
> Priorité : **P0 sécurité** (voir `preprod.md`). Le solde/plafond à l'ingestion
> reste couplé au consensus / Phase B checkpoint (voir § 6).

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

## 5. Décision d'architecture (2026-07-15) — « la CLAIM porte la clé »

La difficulté n° 2 (« résolution d'identité ») est tranchée : s'adosser à la table
d'announces (volatile) rendrait les tx d'émetteurs éteints invérifiables pour tout
nouveau membre → fork comptable garanti. À la place, **la DAG devient l'annuaire** :

- **Wire v2** : la CLAIM — l'acte de rejointe — embarque `member_public[64]`
  (clé publique d'identité du membre), champ SIGNÉ, obligatoire sur les CLAIM et
  interdit ailleurs (+67 o wire sur les CLAIM seulement).
- **Annuaire** : fondateur → clé publiée par le descripteur signé (précédent du
  Palier A) ; membre → clé publiée par sa CLAIM. `meshpay_currency_member_key`.
- **Gate d'ingestion** (`meshpay_currency_ingest_check`) — règles **stateless
  uniquement** (déterministes quel que soit l'ordre d'application) :
  - MINT : autorité + signature vérifiée contre la clé fondateur ;
  - CLAIM : réflexivité, `amount == initial_credit`, **lien clé↔compte**
    (`wallet-hash(member_public) == from`, imparable par préimage) + signature
    vérifiée contre la clé publiée ;
  - TRANSFER : `fee == transfer_fee`, émetteur au registre (sinon
    `ERR_UNKNOWN_MEMBER`, motif TRANSITOIRE) + signature vérifiée contre sa clé.
- **Injection dans la sync** : `apply_batch_gated` (dag_sync ne connaît pas la
  couche monnaie : callback tri-état ACCEPT/REJECT/RETRY). REJECT = skip compté,
  JAMAIS fatal (un pair pollué ne bloque pas la sync des tx saines) ; RETRY
  (membre inconnu) re-testé à chaque passe du multi-passes — la CLAIM de
  l'émetteur peut être plus loin dans le batch — et compté à la stabilisation.
  Table des verdicts par tx : le coût Ed25519 ne se multiplie pas par les passes.
- **Paiement direct** : le gate remplace `verify_sender_if_known` (l'opportuniste
  par announce, contournable par un émetteur silencieux) sous descripteur ; la
  rétention F1 est étendue à `UNKNOWN_MEMBER` (la CLAIM du payeur peut être en
  route), signature invalide = reject définitif immédiat.
- **Repli sans descripteur** (et monitor) : ni gate ni annuaire — aucune racine
  de confiance n'existe ; comportement historique conservé, documenté.

Migration : breaking change assumé (pré-prod). Le `record_size` de `dag_store`
change (296) → vieux snapshots proprement invalidés au boot → DAG vide → chaque
membre ré-émet automatiquement sa CLAIM v2 (auto-émission du Palier C) : la
monnaie de test survit, seul l'historique des paiements d'essai est remis à zéro.

## 6. Reste (différé Phase B consensus/checkpoint)

- **Solde et max_supply à l'ingestion** : dépendants de l'état, donc de l'ordre
  d'application — les gater ferait diverger les nœuds. L'exposition résiduelle se
  limite désormais à des MEMBRES AUTHENTIFIÉS malveillants (découvert au-delà du
  solde, sur-frappe fondateur), bornée par la défense comptable et TRAÇABLE
  (signature engageante). Le checkpoint Phase B devra préserver l'annuaire
  (soldes + set des CLAIM) — exigence déjà notée au § 4.
- Exiger `to` membre sur un TRANSFER (question ouverte du Palier F — refuser de
  payer un compte-tombe par protocole, pas seulement par l'UI).

## 7. Validation finale (2026-07-15)

### Banc on-device (T-Deck, test_app)

- **293 tests Unity, 0 échec, 0 ignoré** — deux passages : après I1-I5, puis un
  second par rigueur après les changements de piles (§ leçons ci-dessous).
- Nouveaux tests du chantier : wire v2 (clé signée, substitution/retrait cassent
  la signature, clé interdite hors CLAIM), annuaire (`member_key`
  fondateur/membre/inconnu/repli), gate (MINT/TRANSFER/CLAIM vrais et forgés,
  usurpation de binding, mauvais fee, WRONG_ID), batch retors dag_sync
  (TRANSFER avant sa CLAIM dans le même batch → mergé au multi-passes ; forge →
  `skipped_invalid`, orphelin résiduel compté), rétention runtime
  (`UNKNOWN_MEMBER` retenu puis livré à l'arrivée de la CLAIM ; forge depuis un
  compte connu rejetée immédiatement).

### Validation réelle (T-Deck fondateur + 3 Waveshare, monnaie « minimes »)

- **Migration automatique** : record_size 232→296 invalide les vieux snapshots
  au boot → 4 CLAIM v2 ré-émises automatiquement, monnaie conservée.
- **6 paiements à l'écran** traversant le gate : 0 rejet, `skipped_invalid=0`
  partout, rien dans les logs d'erreur. Un paiement livré au destinataire par
  paquet direct relayé multi-hop, un autre rattrapé par sync DAG (summary
  `conv=0` → request → resource → merge) : les deux chemins passent le gate.
- **Convergence stricte** : les 4 devices à `tx=10 local=10 conv=1 tips=1`,
  digest commun `8595da24`.
- **Conservation monétaire** (dump dagstore T-Deck, `decode_dagstore.py`) :
  4 CLAIM ×8 + 6 TRANSFER, soldes 12/8/6/6 = **32/32 exact**, fee=0 conforme.
- Uptime > 350 s sans gel ni reboot ; le T-Deck re-passe banc↔firmware sans
  perte (NVS + dagstore intacts, reconvergence immédiate).

### Leçons (traque du « gel » post-flash — cause racine RAM interne)

Le premier flash ×4 du chantier « gelait » le réseau à ~4,4 s. Trois fausses
pistes réfutées (radios sales, débordement de pile, tâches figées) avant la
cause réelle :

1. **`member_public[64]` × fenêtre DAG 250 = +16 Ko de `.bss`** → RAM interne
   épuisée au boot → les DERNIÈRES créations de tâches (`dag_summary_task`,
   touch) échouaient en silence (`start failed` WARN noyé) → plus aucun summary
   périodique → réseau muet. Les devices étaient vivants (radio, UI) : un
   réseau qui se tait ressemble à un réseau gelé.
   **Fix** : l'état applicatif `s_app` (~76 Ko) alloué en **PSRAM**
   (`heap_caps_calloc(MALLOC_CAP_SPIRAM)`, repli RAM interne loggé) dans
   `main/app_main.c`.
2. **`xTaskCreate` : `usStackDepth` est en OCTETS sur ESP-IDF** (pas en mots
   comme FreeRTOS vanilla) : `MESHPAY_APP_TASK_STACK_WORDS 8192` donnait 8 Ko
   réels depuis toujours. Constantes renommées/dimensionnées :
   `MESHPAY_APP_RETICULUM_STACK_BYTES 16384`, `MESHPAY_APP_CORE_STACK_BYTES
   12288` (crypto Monocypher sur ces tâches).
3. **Le banc test_app est AVEUGLE aux contraintes RAM du firmware** (mono-tâche,
   pile 128 Ko, pas les tâches réelles) : 293/0 au banc ne prouve rien sur le
   budget RAM interne au boot — seul le flash réel l'a révélé.
