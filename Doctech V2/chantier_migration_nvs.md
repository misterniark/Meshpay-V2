# Chantier — Migration du record NVS (schéma évolutif, fin du storage mort)

> Statut : **VALIDÉ — banc 309/0 + migration réelle de flotte 4 devices le 2026-07-15 (voir § 9)**.
> Priorité : **P1 robustesse** — devient **P0 avant toute release** : chaque évolution du
> record briquerait silencieusement le stockage de toute la flotte déployée.
> Décisions utilisateur (2026-07-15) : **Option B retenue** (record CBOR tolérant, § 5)
> et **UI de secours « réinitialiser le stockage » à l'écran** (§ 8, backup conservé).

## 1. Symptôme terrain (Palier E, 2026-07-15)

Les Waveshare portaient un record NVS **v1** (firmwares de juin, d'avant le blob
descripteur du Palier A) : au boot, `persistent identity unavailable: nvs=ESP_OK
boot=ESP_ERR_INVALID_SIZE` (WARN noyé dans le boot), puis un device apparemment
fonctionnel mais **amnésique et muet en écriture** : identité RÉGÉNÉRÉE à chaque
boot (alias changeant — l'utilisateur change de compte sans le savoir), et la
rejointe par découverte refusée : `join_discovered` retourne
`ESP_ERR_INVALID_STATE` (l'import exige le storage) mais **l'UI n'affiche rien**
→ « OK ne fait rien » à l'œil, alors que découverte et tactile étaient parfaits.
Remède appliqué : `esptool erase_flash --force` (le `--force` se place APRÈS la
sous-commande ; requis sur S3 à flash chiffrée) + flash-encrypted, la clé NVS se
régénérant seule au boot — inacceptable en production : il détruit aussi la clé
privée d'identité, donc le compte et ses fonds (le compte orphelin `199e5066` à
1 minime vient probablement d'une perte de ce genre).

## 2. Chaîne de mort exacte (références code)

Le record est **la struct C brute `meshpay_storage_record_t` sérialisée telle
quelle** (~1,1 Ko), en UN blob NVS unique `meshpay_state` (namespace `meshpay`) :
`meshpay_storage_save` écrit `sizeof(*record)` octets (`storage.c:180-194`).

Au load (`storage.c:196-224`) :

1. `nvs_get_blob` avec `len = sizeof(record)` courant :
   - blob stocké **plus petit** (ancien schéma) → lecture OK mais `len !=
     sizeof(loaded)` → **`ESP_ERR_INVALID_SIZE`** ;
   - blob stocké **plus grand** (downgrade de firmware) → NVS retourne
     `ESP_ERR_NVS_INVALID_LENGTH` ;
   - taille identique mais `version` différente → `validate_record` exige
     `version == MESHPAY_STORAGE_VERSION` → **`ESP_ERR_INVALID_ARG`**.
2. `meshpay_app_bootstrap_identity` (`app_main_logic.c:452-519`) ne tolère QUE
   `ESP_ERR_NOT_FOUND` pour créer un record neuf ; **tout autre code remonte tel
   quel**.
3. `main/app_main.c:3289-3299` : repli identité éphémère, `storage_ready=false`,
   un WARN et c'est tout.
4. `storage_ready=false` en aval : PIN jamais chargé, boot-credit sauté,
   `meshpay_app_runtime_set_storage` jamais appelé (`main/app_main.c:3414`) →
   **aucun save runtime** (next_seq, alias, PIN). Deux visages du même mal :
   les chemins qui REFUSENT (`join_discovered` → `INVALID_STATE`) mais dont
   l'erreur est avalée par l'UI, et les chemins qui « réussissent » en sautant
   silencieusement la persistance (seq, alias). Vu de l'écran : « OK ne fait
   rien » (chantier UX connexe : « erreurs UI invisibles », noté au Palier E).

> ⚠ Le commentaire de `storage.h:22-23` (« un blob v1 plus petit ne se relit
> pas — traité comme absent, ce qui est sûr ») est **faux** : `INVALID_SIZE`
> n'est PAS traité comme absent — c'est précisément ce chantier. À corriger.

## 3. Le paradoxe protecteur (contrainte de conception)

Le comportement actuel a une vertu accidentelle : **il n'écrase jamais le record
illisible**. La clé privée d'identité est toujours dans la flash, récupérable par
une future migration. Le remède naïf (« tout échec ≠ NOT_FOUND ⇒ traiter comme
neuf et sauver ») serait PIRE que le mal : le save du record neuf **écraserait
définitivement la clé privée** d'un record simplement démodé.

**Invariant du chantier : ne JAMAIS réécrire `meshpay_state` par-dessus un blob
illisible sans l'avoir soit migré, soit archivé (copie sous une clé de backup).**

## 4. Cause de fond

La struct C brute n'est pas un format d'échange :

- `sizeof` change à CHAQUE ajout/retrait de champ (v1→v2 : +descripteur au
  Palier A ; le prochain champ — p. ex. multi-monnaies, réglages UI — re-brique) ;
- padding d'alignement, `size_t`, `bool` : le layout dépend de l'ABI Xtensa et du
  compilateur — jamais figé nulle part (aucun `static_assert(sizeof...)`) ;
- le header magic+version EST dans le blob (préfixe stable `u32+u16` depuis v1)
  mais ne sert aujourd'hui qu'à REFUSER, pas à router vers un lecteur.

Contexte matériel : partition `nvs` de 24 Ko (0x6000) sur les deux cibles ;
Waveshare en **NVS chiffrée** (`nvs_key`) — transparent pour `nvs_get_blob`, mais
tout remède « erase partition » y est encore plus destructif (clés NVS incluses).

## 5. Options

### Option A — Lecteurs par version (structs figées chaînées)

Router sur `version` du préfixe vers une copie figée de chaque ancienne struct
(`record_v1_t`, `record_v2_t`…), convertir champ à champ, re-sauver au format
courant.

- ✅ Petit diff, pas de nouveau format.
- ❌ Les anciennes structs n'ont JAMAIS été figées (padding ABI reconstruit de
  mémoire — risque de désalignement silencieux sur la clé privée) ; une struct
  morte de plus à chaque évolution ; ne résout pas la fragilité de fond.

### Option B — Record en CBOR clé→valeur, tolérant (RECOMMANDÉE)

Sérialiser champ par champ (map CBOR, clés entières stables), comme le wire des
tx (`meshpay_tx`) : champ inconnu → ignoré (un vieux firmware relit un record
plus récent), champ absent → valeur par défaut (un nouveau firmware relit un
vieux record). Un lecteur legacy UNIQUE pour l'existant : blob struct-brut v2
détecté par préfixe magic+version+taille → conversion → save CBOR → backup de
l'ancien blob.

- ✅ Plus AUCUN breaking à vie (ajouts gratuits dans les deux sens) ;
  indépendant de l'ABI ; encodeur CBOR maison déjà éprouvé (meshpay_tx, palier
  descripteur) ; borne de taille maîtrisée.
- ❌ Travail initial le plus gros (encode/decode + migration + tests) ; ~1,1 Ko
  de RAM transitoire pour la conversion (négligeable).

### Option C — Éclater en clés NVS séparées (une par champ)

`identity`, `alias`, `pin`, `seq`, `descriptor`… chacune sa clé NVS.

- ✅ NVS gère les tailles par entrée ; ajouts = nouvelles clés.
- ❌ Perte de l'atomicité du record (commits par clé → états partiels possibles
  sur coupure : identité sans seq, PIN sans flag) ; invariants croisés éclatés ;
  consommation d'entrées sur 24 Ko ; la migration reste à faire de toute façon.

**Recommandation : Option B.** C'est la seule qui éteint la classe de bug (pas
seulement l'occurrence), au prix d'un palier de plus ; l'atomicité du record
unique est conservée. L'Option A reste le sous-composant « lecteur legacy » de B,
limité à la SEULE v2-struct courante (layout encore vérifiable sur les devices
du banc — le figer maintenant avant qu'il ne devienne archéologie).

## 6. Décomposition en paliers

### M1 — Filet de sécurité : diagnostic honnête + backup (indépendant du format)

- `meshpay_storage_load` distingue et remonte un motif exploitable :
  `NOT_FOUND` (neuf) / `LEGACY` (préfixe magic reconnu, version ou taille
  d'un ancien schéma) / `CORRUPT` (préfixe inconnu, CRC/validate KO).
- Au boot, si non lisible : **archiver** le blob tel quel sous `meshpay_bak`
  (une seule fois, ne jamais écraser un backup existant) AVANT tout autre geste.
- Tests : record plus petit / plus grand / version inconnue / magic inconnu →
  motif attendu, backup présent, jamais d'écrasement.
- (La visibilité UI — bandeau « stockage indisponible » et erreurs d'écriture
  affichées — est regroupée au M4 avec le reste du câblage écran.)

### M2 — Format CBOR du record (storage v3)

- `storage_record_encode/decode` CBOR map (clés entières documentées dans le
  .h ; champs : identité, alias, pin_hash, next_seq, checkpoint{seq,hash,blob},
  descripteur). Inconnu ignoré, absent = défaut, bornes strictes par champ.
- `MESHPAY_STORAGE_VERSION 3` ; le blob commence par le même préfixe
  magic+version (u32+u16) pour rester routable à vie.
- Tests : round-trip complet/partiel, champ inconnu toléré, record v4 simulé
  relu par v3 (forward), bornes (alias 32, checkpoint 512, descripteur 384).

### M3 — Lecteur legacy v2-struct + migration au boot

- Figer le layout v2 courant : struct figée `record_v2_legacy_t` +
  `static_assert` — **valeur mesurée sur cible au banc du 2026-07-15 :
  1088 octets** (test « v2 wire size is frozen », log du T-Deck).
- `LEGACY` v2 au load → conversion vers le record courant → save CBOR →
  l'ancien blob archivé en `meshpay_bak` (idempotent : au boot suivant le load
  CBOR réussit directement).
- v1 (d'avant Palier A) : les seuls porteurs connus (les Waveshare du § 1) ont
  été erase_flashés par le remède du Palier E — plus aucun v1 vivant → pas de
  lecteur, chemin `CORRUPT`-like : archive + état visible (décision § 8 si un
  cas réel refait surface).
- Tests : blob v2 fabriqué octet-à-octet → migration → identité/PIN/seq/
  descripteur INTACTS ; blob tronqué → archive sans écrasement ; double boot
  idempotent.

### M4 — Câblage boot + UI

- `bootstrap_identity` route sur le motif : neuf → générer ; CBOR → charger ;
  legacy → migrer (M3) ; corrompu → archiver + mode dégradé explicite.
- `main/app_main.c` : le WARN devient un état UI persistant (bandeau/écran) ;
  aucune identité éphémère silencieuse — l'utilisateur SAIT que rien ne
  persiste et que son compte n'est pas celui d'avant.

### M5 — Banc + validation réelle (transition de flotte)

- Banc Unity complet (cible ≥ M2+M3 : ~15 tests neufs).
- Réel : device du banc porteur d'un record v2-struct RÉEL (avant flash) →
  flash M1-M4 → au boot : migration loggée, identité CONSERVÉE (même hash
  court), monnaie/descripteur intacts, rejointe et paiement re-testés.
- Test du chemin sinistré : blob corrompu injecté → backup + état visible à
  l'écran, pas d'écrasement, erase ciblé possible depuis l'UI (décision § 8).

## 7. Critères de validation

- Plus AUCUN chemin où un blob `meshpay_state` illisible aboutit à un device
  qui écrit « OK » sans persister, ni à une identité régénérée en silence.
- Un record v2-struct réel traverse le flash du nouveau firmware avec identité,
  PIN, seq et descripteur INTACTS (vérif hash court à l'écran + paiement).
- Un blob illisible n'est JAMAIS écrasé sans backup `meshpay_bak` préalable.
- Un record v(N+1) simulé (champ inconnu) est relu par v(N) sans erreur —
  preuve que la classe « évolution = brick » est éteinte dans les deux sens.
- Banc complet 0 échec ; NVS chiffrée Waveshare ET claire T-Deck validées.

## 8. Questions ouvertes

- **UI de secours** : offrir « réinitialiser le stockage » (erase ciblé
  `meshpay_state` + backup conservé) depuis l'écran d'erreur M4, ou exiger
  l'USB ? (penchant : oui à l'écran, le backup rend le geste réversible.)
- **v1 réels** : si un device v1 d'avant le Palier A refait surface, écrire le
  lecteur v1 à ce moment-là (le backup M1 garantit qu'aucune donnée n'est
  perdue en attendant).
- **`meshpay_bak`** : politique de rétention (garder à vie ? purger après N
  boots sains ? — penchant : garder, 1,1 Ko sur 24 Ko est indolore).
- Chantier connexe : le record `dag_store` a le MÊME défaut de struct brute
  (record_size discriminé mais pas de migration — assumé jusqu'ici comme
  reset volontaire de la fenêtre, cf. chantier durcissement ingestion § 5).
  Décider si la Phase B checkpoint l'aligne sur le format CBOR.

## 9. Validation finale (2026-07-15)

### Banc on-device (T-Deck, test_app)

**309 tests, 0 échec, 0 ignoré** (293 avant chantier + 16 nouveaux) :
- M1 : motifs EMPTY/LEGACY/CORRUPT (v1 plus petit, downgrade futur, v2 réel),
  archive une-seule-fois, erase préserve le backup ;
- M2 : taille variable prouvée (record minimal ≪ sizeof struct), tolérance
  forward (blob « v4 simulé » à clés inconnues relu champ à champ), doublon /
  octets orphelins / NUL embarqué / checkpoint sans hash rejetés CORRUPT ;
- M3 : migration v2 intacte champ à champ + backup témoin, v1 archivé
  INVALID_VERSION, v2 incohérent refusé sans remplacement, idempotence ;
- M4 : bootstrap migrateur (identité conservée), jamais d'écrasement d'un
  blob illisible (0 write), alerte UI + reset 2 temps + feedback d'échec.

### Validation réelle (migration de flotte, 4 devices, records v2 RÉELS)

- **3 Waveshare (NVS chiffrée)** : `record NVS migre v2 -> v3 (original
  archive)` au boot, puis `identity loaded from NVS` — alias et next_seq
  CONSERVÉS (castor precis seq=7, lezard vif seq=5, morse patient seq=5).
- **T-Deck (NVS claire)** : migré (voir incident ci-dessous), dump NVS de
  preuve : **1 blob v3 (record courant) + blobs v2 journalisés dont le backup
  `meshpay_bak`** — le témoin d'avant migration est physiquement en flash.
- DAG restaurée count=10 sur les 4, monnaie « minimes » intacte, aucun crash.
- Écrans : alerte « Stockage HS » + bouton Réinitialiser VISIBLES sur T-Deck
  (footer + détails) ; sur Waveshare le renderer n'affiche pas les
  detail_lines → footer + bouton seulement (limitation d'affichage notée,
  pas de perte fonctionnelle).
- Chemin sinistré (CORRUPT réel) : couvert par le banc (mock exhaustif), pas
  reproduit au réel — fabriquer un blob NVS corrompu à travers l'API esptool
  casserait la NVS entière (auto-erase à l'init), pas le cas visé.

### Incident du premier flash — la leçon de pile, ENCORE

Le premier flash réel a mis le T-Deck en **boot-loop** : `Guru Meditation
LoadProhibited` sur l'IDLE task du core 1, `EXCVADDR=0xa3e8681c` — 0xA3 est
une map(3) CBOR : les buffers de travail du storage (3× ~1,5 Ko empilés sur
le chemin bootstrap → migrate → save) **débordaient la pile de la main task
(8 Ko)** et écrasaient les structures esp_pm voisines. Le crash étant
asynchrone, une itération de la boucle avait DÉJÀ migré le record proprement
(migration validée après coup par le dump NVS).

- **Fix** : `blob_buf_alloc/free` — les buffers de blob vont sur le TAS
  (alloc transitoire, zéroïsée au free), jamais sur la pile, jamais en
  `.bss` RAM interne permanente.
- **Leçon (bis repetita du chantier durcissement)** : le banc test_app
  (pile 128 Ko, 309/0 vert) est STRUCTURELLEMENT aveugle aux budgets de pile
  du firmware. Tout nouveau buffer ≥ ~1 Ko sur un chemin de boot ou de tâche
  DOIT passer par le tas, et le flash réel reste la seule preuve.
- L'invariant « jamais d'écrasement sans backup » a tenu PENDANT le crash :
  aucune donnée perdue à aucun moment de l'incident.
