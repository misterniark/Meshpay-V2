---
tags:
  - meshpay
  - design
  - dag
  - lora
Projets:
  - Mesh Pay
Topics:
  - Documentation technique
  - Synchronisation
  - Paiement offline
Date: 2026-05-17
---

# DAG et sync LoRa v2

> [!summary]
> Cette note remplace l'approche "patch par patch" autour de la DAG. Objectif : rendre explicite ce qui est la source de verite locale, ce qui est une preuve reseau, et comment un device rebooté rattrape l'etat sans serveur.

## 1. Pourquoi reprendre le design

Les tests hardware du 17 mai 2026 ont mis en evidence trois problemes lies :

- apres reboot, la DAG runtime est vide et le device revient visuellement au checkpoint local ;
- les peers ne peuvent pas verifier un solde qui n'existe que dans le checkpoint prive d'un autre device ;
- la sync LoRa actuelle est un gossip periodique aveugle : elle rediffuse ce que le device a encore en RAM, mais elle ne permet pas de demander explicitement les TX manquantes.

Les correctifs temporaires ont rendu les tests possibles :

- replay LoRa depuis `0` ;
- catch-up boot rapide ;
- self-MINT de seed test persistée et rediffusée.

Mais ces correctifs ne constituent pas un protocole durable. Le checkpoint local est encore trop central dans le calcul du solde local et trop invisible pour le reseau.

## 2. Invariants cibles

### I1 - Une TX valide est une preuve autonome

Une transaction signee doit rester verifiable sans connaitre l'etat prive du device qui l'a emise. Pour un `TRANSFER`, les peers ont besoin d'un historique minimal permettant de verifier que l'emetteur avait le solde necessaire.

### I2 - Le checkpoint est local, pas une preuve reseau

Un checkpoint non signe ne doit jamais servir a convaincre un autre device. Il peut accelerer le calcul local et reduire la RAM, mais il ne remplace pas les preuves de credits/debits diffusables.

### I3 - La DAG recente doit survivre au reboot

Apres reboot, un device doit pouvoir :

- recalculer son solde sans attendre le reseau, au moins pour les TX recentes ;
- rediffuser ses TX confirmees recentes ;
- repondre a une demande de rattrapage d'un peer.

### I4 - Le reseau converge par demande explicite

La convergence ne doit pas dependre uniquement d'un broadcast periodique. Un device qui detecte qu'il lui manque des TX doit pouvoir les demander.

### I5 - Les doublons sont normaux

La couche de sync doit accepter les doublons comme comportement nominal. La deduplication reste faite par `tx_id` et par les regles `(from, seq)`.

### I6 - Aucun double comptage checkpoint + DAG

Une TX consolidee dans le checkpoint local ne doit pas etre recomptee si elle est aussi presente dans la fenetre DAG durable. Il faut une frontiere claire : `checkpoint_base_ts` ou `checkpoint_base_tx_id`.

## 3. Modele de donnees cible

### 3.1 Etat local durable

| Donnee | Role | Stockage cible |
|---|---|---|
| Keypair | Identite du wallet | NVS chiffree |
| Alias / config | UX et parametres | NVS chiffree |
| `next_seq` + `own_max_seq` | anti-rejeu local | NVS chiffree |
| Checkpoint local | acceleration de solde | partition durable, pas petite NVS |
| Fenetre TX durable | preuves recentes + rattrapage | partition durable dediee |
| Index TX | lookup par hash, seq, timestamp | partition durable dediee |

La petite partition NVS ne doit plus porter les objets volumineux et frequemment réécrits. L'erreur observee `0x1105` pendant `nvs_checkpoint_save` indique que cette piste est deja a bout.

### 3.2 Fenetre TX durable

La fenetre doit contenir au minimum :

- self-MINT ou credit initial/test seed ;
- TX emises par le device tant qu'elles peuvent encore etre demandees ;
- TX recues et confirmees recemment ;
- attestations de confirmation pour les `TRANSFER`.

Pour le prototype :

- taille cible : 64 a 128 TX ;
- format : blob append-only ou petit log structure ;
- reconstruction RAM au boot : charger les TX dont `timestamp > checkpoint.timestamp` et les TX de preuve necessaires.

### 3.3 Checkpoint local

Le checkpoint devient :

- une base de calcul locale ;
- un point de prune local ;
- une optimisation, pas un objet de confiance reseau.

Tant qu'il n'est pas signe par une autorite ou par un quorum, il ne doit pas etre transmis comme preuve.

## 4. Protocole LoRa v2

Le LoRa doit passer d'un "gossip de TX" a un protocole en 4 messages.

### 4.1 `DAG_SUMMARY`

Annonce courte diffusee periodiquement et au boot.

Contenu :

- `node_pubkey`
- `epoch` ou `sync_generation`
- `checkpoint_timestamp`
- `last_tx_timestamp`
- quelques `tip_ids`
- `tx_count_window`
- `bloom` ou digest compact optionnel de la fenetre TX
- signature du resume

But : permettre a un peer de savoir rapidement s'il lui manque quelque chose sans recevoir toute la fenetre.

### 4.2 `DAG_REQUEST`

Demande ciblee, envoyee en unicast LoRa si possible ou broadcast avec `target_pubkey`.

Types de demande :

- `since_timestamp`
- `missing_tx_ids[]`
- `around_tip_id`
- `proof_for_pubkey`

But : rattraper apres reboot ou perte radio sans attendre un cycle complet.

### 4.3 `DAG_TX_BATCH`

Reponse contenant un petit lot de TX.

Contraintes :

- batch fragmente si necessaire ;
- 4 a 8 TX max par lot selon time-on-air ;
- chaque TX reste signee individuellement ;
- le batch peut etre signe aussi pour l'anti-flood et l'audit.

### 4.4 `DAG_ATTEST_BATCH`

Lot d'attestations de confirmation.

But : eviter le cas ou une TX est connue mais reste `LOCKED` faute d'avoir recu l'attestation du destinataire.

## 5. Flux paiement cible

### 5.1 Paiement direct ESP-NOW

1. Le payeur choisit un peer.
2. Le payeur s'assure d'avoir une preuve locale diffusable de son solde.
3. Le payeur envoie si besoin les preuves manquantes au destinataire.
4. Le payeur envoie le `TRANSFER LOCKED`.
5. Le destinataire valide :
   - signature ;
   - currency_id ;
   - seq ;
   - solde emetteur a partir des preuves connues ;
   - regles monnaie.
6. Le destinataire insere la TX, la confirme et envoie ACK ESP-NOW.
7. Le destinataire diffuse une attestation LoRa.
8. Les deux devices persistent la TX et l'attestation dans la fenetre durable.

### 5.2 Paiement reçu via LoRa

Un `TRANSFER` reçu par LoRa ne doit pas etre accepte sans chemin de confirmation clair :

- soit il est deja accompagne d'une attestation valide ;
- soit il reste `LOCKED` et le destinataire final doit pouvoir l'attester ;
- soit il est ignore jusqu'a reception des preuves manquantes.

## 6. Regles de validation reseau

### 6.1 Solde d'un peer

Pour valider le solde d'un peer, utiliser :

1. credits prouvés par MINT/TRANSFER confirmés connus ;
2. debits connus par TRANSFER `LOCKED` ou `CONFIRMED` ;
3. attestations de confirmation connues ;
4. jamais le checkpoint prive du peer.

### 6.2 Parents manquants

`dag_merge_transaction` accepte aujourd'hui des parents manquants en sync. En v2, un parent manquant doit déclencher :

- insertion possible en quarantaine si la TX est cryptographiquement valide ;
- demande `DAG_REQUEST missing_tx_ids` ;
- promotion normale quand les parents ou le checkpoint local permettent de verifier la frontiere.

### 6.3 Conflits `(from, seq)`

Le premier conflit detecte doit etre conserve comme preuve, pas seulement rejete silencieusement :

- stocker les deux TX conflictuelles ;
- marquer le compte emetteur comme suspect localement ;
- exposer l'info dans le debug console ;
- reporter en LoRa si un protocole d'audit est ajoute plus tard.

## 7. Persistance cible

### 7.1 Probleme NVS actuel

Le checkpoint fait environ 9 KB et la partition NVS actuelle fait 24 KB. Avec chiffrement, metadata, keypairs, alias, seed, next_seq et réécritures frequentes, elle n'est pas adaptee.

Erreur observee :

```text
hal_storage_esp32: Erreur NVS non mappée : 0x1105
nvs_chk: Erreur ecriture checkpoint NVS
```

Action cible :

- garder NVS pour les petites cles ;
- utiliser `storage` pour les logs DAG/checkpoint ;
- choisir LittleFS ou FATFS selon support ESP-IDF et facilite de tests ;
- versionner le format.

### 7.2 Format minimal v1

Fichiers ou zones :

- `ledger/checkpoint.bin`
- `ledger/txlog.bin`
- `ledger/attlog.bin`
- `ledger/index.bin`
- `ledger/meta.bin`

Le format peut rester binaire C au debut, mais doit porter :

- magic ;
- version ;
- taille record ;
- CRC ;
- compteur d'ecriture ou generation.

## 8. Plan d'implementation

### Phase A - Stabilisation locale

- Extraire la persistance checkpoint hors NVS.
- Ajouter une persistance TX window append-only.
- Recharger la fenetre au boot.
- Supprimer les hacks special seed du chemin normal autant que possible.

Definition of done :

- paiement A -> B ;
- reboot A ;
- A affiche son vrai solde sans attendre LoRa ;
- A peut rediffuser ses TX recentes.

Etat implementation au 2026-05-17 :

- checkpoint runtime déplacé vers la partition interne `storage` ;
- partition `storage` marquée `encrypted` dans `partitions.csv` ;
- NVS conservée pour keypair, alias, next_seq et petites clés ;
- migration douce depuis l'ancien checkpoint NVS si le nouveau stockage est vide ;
- fenêtre durable de 128 TX récentes sauvegardée dans `storage` ;
- au boot, les TX de la fenêtre sont relues, vérifiées structure + signature/hash, puis réinjectées dans la DAG RAM si elles sont postérieures au checkpoint ;
- implémentation volontairement simple en snapshot par région flash, pas encore append-only.

Note micro-SD :

- les Waveshare ESP32-S3 1.47 Touch Display ont un emplacement micro-SD utilisable pour archive longue, export debug ou gros historique ;
- ce n'est pas le stockage par défaut de Phase A, afin de garder le prototype autonome et éviter une dépendance carte SD ;
- la micro-SD reste une option Phase E si on veut journal longue durée ou forensic terrain.

### Phase B - LoRa summary/request

- Ajouter `DAG_SUMMARY`.
- Ajouter `DAG_REQUEST since_timestamp`.
- Ajouter `DAG_TX_BATCH`.
- Garder le gossip legacy pendant transition.

Definition of done :

- B reste online ;
- A reboot ;
- A detecte via summary qu'il manque des TX ;
- A demande et recupere les TX en moins de 30 s.

Etat implementation au 2026-05-17 :

- `DAG_SUMMARY` signé ajouté (`COMM_MSG_LORA_DAG_SUMMARY`) :
  - pubkey du node ;
  - `checkpoint_timestamp` ;
  - `last_tx_timestamp` ;
  - nombre de TX dans la fenêtre ;
  - jusqu'à 2 tips.
- `DAG_REQUEST` signé ajouté (`COMM_MSG_LORA_DAG_REQUEST`) :
  - requester ;
  - target ;
  - `since_timestamp` ;
  - `max_count`.
- Au cycle LoRa, chaque device émet un `DAG_SUMMARY` avant le gossip legacy.
- Si un device reçoit un summary plus récent que son état local, il demande explicitement les TX depuis son dernier timestamp.
- La réponse réutilise le chemin `LORA_TX` existant, donc les grosses TX continuent d'être fragmentées par `lora_tx_packetize`.
- `DAG_TX_BATCH` compact n'est pas encore activé : reporté en Phase B.2 pour ne pas casser le réassemblage fragmenté actuel, qui est encore orienté "une TX par séquence".

### Phase C - Attestations et confirmations

- Ajouter `DAG_ATTEST_BATCH`.
- Persister les attestations.
- Rejouer les attestations au boot.

Definition of done :

- une TX ne reste pas `LOCKED` apres reboot si une attestation valide existe sur le reseau.

Etat implementation au 2026-05-17 :

- ajout de `COMM_MSG_LORA_DAG_ATTEST_BATCH` ;
- format compact : clé attestante commune, jusqu'à 2 attestations par paquet LoRa ;
- chaque entrée garde une signature individuelle `sig(tx_id)` compatible avec l'attestation unitaire existante ;
- ajout d'une fenêtre durable d'attestations dans la partition interne chiffrée `storage` ;
- une attestation reçue pour une TX encore inconnue est conservée au lieu d'être perdue ;
- replay des attestations au boot après rechargement de la fenêtre TX ;
- replay aussi après insertion d'une TX reçue, pour confirmer une TX arrivée après son attestation ;
- les cycles LoRa rediffusent les attestations récentes en batch compact.
- les cycles LoRa relisent aussi la fenêtre TX durable, pas seulement le DAG RAM, afin de continuer à propager les TX récentes après reboot/checkpoint.
- le driver Core1262 remet le SX1262 en standby avant TX et tente un reset/reconfiguration radio si le module reste bloqué après un échec d'émission.

Limite assumée :

- le batch n'est pas fragmenté ; il est volontairement limité à 2 attestations pour rester dans une trame LoRa unique.

Dette identifiée :

- une TX `LOCKED` rechargée après reboot peut avoir des parents déjà absorbés par le checkpoint. Corrigé côté diagnostic : ces parents sont maintenant comptés comme `pre_checkpoint_parents` si le parent est le `last_tx_id` du checkpoint ou une TX confirmée couverte encore visible dans la fenêtre durable.

### Phase D - Prune/checkpoint propre

- Definir une frontiere locale claire.
- S'assurer qu'une TX n'est jamais comptee deux fois.
- Ajouter tests unitaires checkpoint + fenetre.

Etat implementation au 2026-05-17 :

- `persist_runtime_checkpoint()` met maintenant à jour le checkpoint RAM et prune les TX confirmées couvertes ;
- la fenêtre TX durable n'est plus un miroir destructif du DAG RAM : elle fusionne l'ancien historique durable avec le DAG actuel ;
- les TX confirmées consolidées restent donc disponibles pour l'historique UI et la propagation LoRa post-reboot ;
- `auto_checkpoint_if_needed()` passe par le même chemin de commit checkpoint/fenêtre/prune.

Definition of done :

- checkpoint + fenetre durable donnent le meme solde qu'une DAG complete.

## 9. Tests indispensables

### Tests natifs

- `checkpoint + tx_window = solde DAG complete`
- reboot avec DAG vide RAM mais fenetre durable presente
- peer sans seed local valide une self-MINT reçue
- conflit `(from, seq)` conserve et expose
- parent manquant déclenche une demande
- attestation reçue apres TX LOCKED promeut en CONFIRMED

### Tests hardware

- 3 devices, seed 10 chacun, paiements croises, reboot d'un device ;
- debrancher/rebrancher un device pendant que les deux autres continuent ;
- LoRa KO sur un device : ESP-NOW direct doit fonctionner, rattrapage via peers quand LoRa revient ;
- reset pendant ecriture checkpoint/log ;
- verification que les ecrans n'affichent pas de solde stale comme definitif.

## 10. Decisions ouvertes

| Question | Option A | Option B | Recommandation |
|---|---|---|---|
| Stockage durable | LittleFS | FATFS | LittleFS pour append/log + robustesse coupure |
| Checkpoint reseau | jamais transmis | signé par autorité | jamais transmis en v2, reouvrir plus tard |
| Taille fenetre TX | 64 | 128+ | commencer 128 si flash suffisante |
| Pull LoRa | since timestamp | bloom/digest | commencer timestamp + missing ids |
| Seed test | self-MINT spéciale | fixture de genesis | garder self-MINT, mais via persistance TX normale |

## 11. Impact code

Modules concernes :

- `main/persistence/` : nouveau backend durable pour checkpoint + tx window ;
- `main/dag_glue.c` : séparation claire insertion RAM / persistance / checkpoint ;
- `main/transport/transport_lora.c` : remplacer `collect_confirmed_txs` par summary/request/batch ;
- `components/comm/lora_sync/` : nouveaux messages et state machine ;
- `components/comm/comm_protocol/` : pack/unpack messages DAG v2 ;
- `components/core/dag/` : support quarantaine ou index des parents manquants ;
- `main/handlers/handler_payment.c` : validation par preuves reseau, pas checkpoint peer implicite ;
- debug console : commandes `dump_txlog`, `dump_sync`, `request_sync`.

## 12. Position finale

Le DAG reste le bon choix pour MeshPay. Le probleme n'est pas la structure DAG, mais le fait que la couche de propagation actuelle ne distingue pas assez :

- etat local ;
- preuve reseau ;
- optimisation checkpoint ;
- rattrapage apres reboot.

La v2 doit traiter ces quatre notions separement. Une fois cette frontiere posee, les bugs actuels cessent d'etre des surprises : ils deviennent des cas normaux du protocole.
