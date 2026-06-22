# Protocole de test — Robustesse de la DAG (MeshPayV2)

> Date : 2026-06-22 · Cible : 4 × Waveshare ESP32-S3-Touch en `policy=all:espnow`
> (ESP-NOW seul, LoRa désactivé) + 1 × ESP32-S3 **injecteur** dédié.
> Approche **hybride** : pilotage tactile + observation par logs série pour la
> propagation / le catch-up / le merge ; injecteur ESP-NOW pour l'anti-corruption.

---

## 1. Objectif

Valider quatre propriétés de la DAG sur le mesh ESP-NOW :

1. **Propagation** — une transaction émise sur une carte est intégrée par toutes.
2. **Intégrité (anti-corruption)** — une transaction invalide injectée sur le réseau est rejetée par toutes, sans altérer la DAG.
3. **Catch-up** — un device absent retrouve la DAG à jour à son retour.
4. **Merge de partition** — deux groupes ayant divergé pendant un éloignement fusionnent sans perte ni conflit quand l'un rejoint l'autre.

## 2. Matériel & rôles

| Rôle | Port série | MAC | Alias | Firmware |
|---|---|---|---|---|
| Honnête A | `/dev/cu.usbmodem11101` | `20:6e:f1:9a:1e:b0` | loup sobre | secure espnow-only |
| Honnête B | `/dev/cu.usbmodem11201` | `44:1b:f6:86:14:64` | loup doux | secure espnow-only |
| Honnête C | `/dev/cu.usbmodem11301` | `44:1b:f6:86:11:e4` | orque curieux | secure espnow-only |
| Honnête D | `/dev/cu.usbmodem11401` | `44:1b:f6:86:13:c8` | castor precis | secure espnow-only |
| Injecteur | (5e ESP32-S3) | — | — | firmware « attaquant » (§4.1) |

> Les 4 honnêtes tournent déjà sur le build `build-hardware-smoke-s3-secure`
> (`CONFIG_MESHPAY_FORCE_ESPNOW_ONLY=y`). Toutes sur le **même canal** (`CONFIG_MESHPAY_ESPNOW_CHANNEL=1`).

## 3. Repères firmware (constantes & logs)

Sync DAG (`components/dag_sync/`, `main/app_main.c`) :
- `DAG_SUMMARY` périodique : **15 s** (`MESHPAY_DAG_SUMMARY_INTERVAL_MS`).
- `DAG_REQUEST` timeout : **30 s** ; batch via Reticulum `Resource`.
- Fenêtre DAG : **250** TX ; seuil checkpoint : **200** ; max **32** tips (`components/dag/include/meshpay/dag.h`).

Logs clés (TAG `meshpayv2` / `app_runtime`) :
- Acceptation : `payment received amount=<a> from=<8hex> dag=<N>` — **`dag=<N>` = nombre de TX dans la DAG locale** (marqueur de convergence).
- Rejets : `payment reject verify … err=…` (signature) · `payment reject merge=<code> … dag=<N>` (CONFLICT / MISSING_PARENT / FULL / DUPLICATE) · `payment reject tx_for_us=… currency=… dag=<N>`.
- Découverte : `peer announce accepted alias=<…> peers=<n>`.
- Boot/persistance : `identity loaded from NVS next_seq=<s>` · `boot credit restored amount=<a>` · `Reticulum radio ready backend=espnow policy=all:espnow (lora off)`.

## 4. Outillage à préparer (avant les tests)

### 4.1 Firmware injecteur (5e carte)
Nouveau firmware réutilisant `rns_node` + `meshpay_tx` + `rns_packet_crypto` pour
**construire et émettre des paquets DATA Reticulum forgés** vers une destination honnête.
Quatre forges, déclenchables successivement :

| # | Forge | Construction | Rejet attendu côté honnête |
|---|---|---|---|
| F1 | Signature invalide | TX bien formée, signature Ed25519 corrompue (1 octet flippé) | `payment reject verify … err=…` |
| F2 | Double-dépense | deux TX avec mêmes `(from, seq)` et `id` différents, montants différents | `payment reject merge=CONFLICT` |
| F3 | Parent inexistant | `parents[0]` = hash aléatoire absent de la DAG | `payment reject merge=MISSING_PARENT` |
| F4 | Solde / règle monnaie | `amount` > solde de l'émetteur, ou `MINT` non autorisé, ou `fee ≥ amount` | `payment reject tx_for_us=… currency=…` |

> L'injecteur doit s'annoncer (announce) pour être routable, puis émettre les forges
> à la demande. Pas de CLI → prévoir un déclenchement simple (tap écran, ou émission
> séquentielle temporisée au boot avec log `inject sent forge=Fx`).

### 4.2 Log « digest DAG » sur le firmware honnête
Ajouter dans `components/dag/` un log périodique (aligné sur l'émission du `DAG_SUMMARY`) :

```
dag_digest=<hex8> count=<N> tips=<hex8>[,<hex8>]
```

où `<hex8>` = 8 premiers octets du **SHA-256 des `id` de TX triés** (la DAG est un
ensemble : l'ordre d'insertion ne doit pas changer le digest). **Justification** : le seul
`dag=<N>` ne prouve pas l'identité de contenu — deux DAG de même taille mais divergentes
auraient le même `N`. Le digest est le critère de convergence rigoureux.

### 4.3 Capture multi-port
Script `quad_capture.py` (dérivé de `/tmp/dual_capture.py`) : ouvre les 4 ports,
fait le reset DTR/RTS, horodate et préfixe chaque ligne par l'alias. Sert de journal brut.

## 5. Convention d'observation & critère de convergence

> **CONVERGENCE** ≝ tous les devices considérés affichent le **même `dag_digest`**
> (et donc le même `count=N`) après stabilisation. Le `dag=N` seul sert d'indicateur
> rapide ; le **digest fait foi**.

Délai de stabilisation de référence : **≤ 2 cycles de sync** après la dernière action,
soit **~30–45 s** (intervalle SUMMARY 15 s + marge de transfert batch).

## 6. Phase 1 — Propagation (4 cartes ensemble)

**But** : une TX émise n'importe où atteint et est intégrée par toutes.

1. Démarrer la capture des 4 ports ; vérifier que chacune logge `policy=all:espnow` et que les 4 se découvrent (`peer announce accepted`, `peers=3` à terme).
2. Noter le `dag_digest`/`N` initial des 4 (doit déjà être commun si elles ont sync au boot).
3. Émettre **15–20 paiements** répartis : A→B, B→C, C→D, D→A, etc. (tactile : HOME→PAY→peer→montant→CONFIRM). Espacer de quelques secondes.
4. Après chaque paiement : la carte émettrice logge l'émission, les autres `payment received … dag=N` incrémenté.

**Réussite** : après stabilisation, les **4 cartes ont le même `dag_digest`** et `N` attendu ; soldes cohérents (somme conservée, fees vers l'autorité de mint).
**Échec** : une carte reste en retard (digest/N divergent) au-delà de 2 cycles, ou un paiement n'apparaît jamais ailleurs.

## 7. Phase 2 — Intégrité / anti-corruption (injecteur)

**But** : aucune TX invalide réseau n'entre dans la DAG.

1. Réseau stabilisé (Phase 1 terminée), relever `dag_digest`/`N` de référence sur les 4.
2. Allumer l'injecteur ; vérifier qu'il est routable (`peer announce accepted` sur les honnêtes).
3. Émettre **F1**, attendre, observer les 4 honnêtes → log de rejet attendu (tableau §4.1). Répéter pour **F2, F3, F4**.
4. Émettre une rafale mixte (les 4 forges plusieurs fois) pour vérifier l'absence de fuite sous charge.

**Réussite** : pour chaque forge, **toutes** les honnêtes loggent le **rejet attendu**, le **`dag_digest`/`N` reste inchangé** sur les 4, aucune carte ne crashe/fige (announces continuent).
**Échec** : un `dag_digest`/`N` change après une injection (TX corrompue acceptée), absence de rejet, crash, ou blocage du wallet.

## 8. Phase 3 — Catch-up (device absent qui revient)

**But** : un device absent retrouve la DAG à jour à son retour.

1. Réseau stabilisé sur les 4 ; relever le digest commun.
2. **Couper l'alim de D** (débrancher l'USB). Confirmer sa disparition (les 3 autres ne le voient plus s'annoncer).
3. Les **3 restantes émettent M = 10–15 paiements** → leur DAG avance à `N+M`, digest commun aux 3.
4. **Rebrancher D** + reset DTR/RTS. Observer au boot : `identity loaded from NVS` + `boot credit restored` (D ne repart pas de zéro), puis le catch-up : réception d'un `DAG_SUMMARY`, émission d'un `DAG_REQUEST`, application du batch.
5. **Variante absence longue** : faire avancer la DAG au-delà du **checkpoint (200)** avant le retour, pour valider la restauration via checkpoint + rattrapage.

**Réussite** : après retour, **D reconverge** vers le `dag_digest`/`N` des 3 autres dans le délai de stabilisation ; pas de trou de séquence, pas de double-dépense de D (`next_seq` cohérent).
**Échec** : D reste en retard, rejoue/duplique des TX, ou repart de zéro (perte d'état NVS).

## 9. Phase 4 — Merge de partition (éloignement physique réel)

**But** : deux groupes divergents fusionnent sans perte ni conflit quand l'un rejoint l'autre.

1. **Constituer 2 groupes** : G1 = {loup sobre, loup doux} sur place ; G2 = {orque curieux, castor precis} **emmenées dans une autre pièce, hors portée ESP-NOW**.
2. **Vérifier l'isolement** (crucial, ESP-NOW porte loin) : dans les logs, **aucun `peer announce accepted` cross-groupe** ne doit apparaître (G1 ne voit que G1, G2 ne voit que G2). Si l'isolement n'est pas obtenu, éloigner davantage. Relever le digest de chaque groupe (ils vont diverger).
3. **Chaque groupe transacte en interne** : G1 fait p paiements entre ses 2 cartes ; G2 fait q paiements entre les siennes. Les deux digests **divergent** (historiques différents).
4. **Rapprocher une carte de G2** (p.ex. orque curieux) de G1, jusqu'à reprise des announces cross-groupe.
5. Observer la réconciliation : `DAG_SUMMARY` croisés → `DAG_REQUEST` → batchs `Resource` dans les deux sens.

**Réussite** : **toutes** les cartes rejointes convergent vers une **DAG = union** des TX des deux groupes (digest/N identiques), **aucun `merge=CONFLICT`** sur des TX légitimes, soldes globaux cohérents. La carte restée éloignée (castor precis) reconverge à son tour à son retour.
**Échec** : conflit sur des TX légitimes, TX perdues (N final < union attendue), digests qui ne convergent pas, ou double-comptage de solde.

## 10. Critères d'acceptation globaux

- [ ] Phase 1 : convergence des 4 (digest commun) après émissions.
- [ ] Phase 2 : 0 TX corrompue acceptée ; rejet attendu sur les 4 pour F1–F4 ; digest inchangé.
- [ ] Phase 3 : catch-up complet d'un device absent (digest reconvergé), y compris au-delà du checkpoint.
- [ ] Phase 4 : merge complet de 2 historiques divergents (DAG = union), sans conflit ni perte.
- [ ] Aucune carte ne crashe / ne fige / ne perd son état NVS sur l'ensemble du protocole.

## 11. Journal de banc (template)

```
Date / heure :
Build honnête (commit) :        Build injecteur (commit) :
Étape | alias | port | dag=N | dag_digest | log notable
------+-------+------+-------+------------+-------------
P1.0  | …     | …    |       |            | état initial
P1.fin| …     |      |       |            | convergence ?
P2.F1 | …     |      |       |            | reject verify ?
…
```

### 11.1 Run 2026-06-22 — Phase 1 (propagation des MINT, sans paiements tactiles)

- **Build honnête** : `883dc41` (main) — `policy=all:espnow`, log `dag_digest` actif. **Injecteur** : n/a (différé).
- **Méthode** : `esptool erase_region 0x9000 0x6000 --force` (partition `nvs`) sur les 4 → identités neuves + 1 boot-credit MINT par carte ; capture `quad_capture.py 120`.
- **Couverture** : propagation des 4 MINT auto (cœur « une TX émise n'importe où atteint toutes »). **Étape 3 (15-20 paiements TRANSFER tactiles + vérif soldes/fees) NON exécutée** — pas d'accès tactile ni de console série.

```
Étape | portée      | dag=N | dag_digest | log notable
------+-------------+-------+------------+-------------
P1.0  | 4 cartes    | 1     | distinct   | boot credit restored=10 ; policy=all:espnow ; peers=3
P1.a  | doux+sobre  | 2     | bd0c4d25   | 2 MINT mergés (~12-28 s)
P1.b  | +orque      | 3     | 2f1724e4   | 3 MINT mergés (~29-44 s)
P1.fin| LES 4       | 4     | bdaf111a   | CONVERGENCE OUI ~59 s, stable 60 s ; 0 conflit/rejet
```

**Verdict P1 (propagation MINT) : RÉUSSITE** — critère atteint (même `dag_digest=bdaf111a` / `N=4` sur les 4, dans le délai de stabilisation). Aucun `merge=CONFLICT`. Reste à couvrir : paiements TRANSFER tactiles + soldes. Log brut : `/tmp/phase1_cap.log`.

### 11.2 Run 2026-06-22 — Phase 1 complément (paiements TRANSFER tactiles)

- **Build** : `883dc41` (main). Capture `quad_capture.py --no-reset` (sans reboot) ~557 s pendant les paiements tactiles, puis **75 s de stabilisation passive** (clé : conclure APRÈS le délai de stabilisation, pas à l'instant du dernier clic).
- **Méthode** : 10 paiements TRANSFER aboutis (montants 2-5) émis depuis les 4 cartes.

```
Étape  | portée | dag=N | dag_digest | log notable
-------+--------+-------+------------+-------------
P1b.0  | 4      | 4     | bdaf111a   | etat post-MINT (cf. 11.1)
P1b.tx | divers | 5→14  | (varie)    | 10 payment received (montants 2-5), 4 emetteurs distincts
P1b.rej| 2 rej  | --    | --         | merge=3 MISSING_PARENT (transitoire, re-accepte) ; currency=6 (definitif)
P1b.fin| LES 4  | 14    | d5fbea51   | CONVERGENCE OUI, stable 75s ; 14 = 4 MINT + 10 TRANSFER valides
```

**Verdict complément P1 : RÉUSSITE.** Les 4 convergent vers `d5fbea51` / `N=14`. Anti-double-dépense vérifiée : le paiement invalide (`currency=6`, sur-dépense) n'entre dans aucune DAG (pas de `count=15`) ; le rejet `MISSING_PARENT` était transitoire (ré-accepté après propagation du parent). **Soldes : reconstruction comptable complète OK.** Mapping identité→carte par élimination (une carte ne reçoit jamais d'elle-même), puis `solde = 10 (boot) + reçus − émis` par identité : `d89a8145`=16, `63dc99b9`=12, `336e16cc`=6, `d8fd53cc`=6 — **exactement les 4 soldes affichés (Hibou 16, Aigle 12, Renard 6, Chamois 6), somme=40=total frappé**. Les soldes écran sont reproductibles depuis la DAG répliquée ⇒ aucun paiement perdu/double-compté, fee à effet net nul sur ces transferts. Logs bruts : `/tmp/phase1_pay.log`, `/tmp/phase1_stab.log`.

## 12. Notes & pièges

- **LoRa désactivé** (`policy=all:espnow`) : tout passe en ESP-NOW. Ne pas conclure sur la propagation LoRa ici.
- **Hors-portée ESP-NOW (Phase 4)** : la portée peut dépasser une pièce ; valider l'isolement par les logs, pas par hypothèse.
- **Reset après (dé)branchement** : USB-CDC ne reboot pas seul → reset DTR/RTS (cf. `CLAUDE.md`). Le device peut rester en download mode après un flash `--after no_reset`.
- **Digest = ensemble** : trier les `id` avant hash, sinon deux DAG identiques donneraient des digests différents.
- **Échelle** : rester sous la fenêtre de 250 TX sauf pour la variante checkpoint (Phase 3) qui vise justement à la franchir.
