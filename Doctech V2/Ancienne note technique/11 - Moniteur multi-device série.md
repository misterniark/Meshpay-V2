---
tags:
  - meshpay
  - meshpay/tooling
  - meshpay/debug
  - tech/python
Projets:
  - Mesh Pay
Topics:
  - Outillage
  - Debug
  - Tests terrain
Date: 2026-05-13
---

# Moniteur multi-device série (`meshpay_monitor.py`)

> [!abstract] À quoi sert cette note
> Mode d'emploi du moniteur Python qui se connecte simultanément à plusieurs devices Mesh Pay flashés avec la console de debug (composant `debug_console`) et affiche en temps réel l'état du DAG, du wallet et la convergence du gossip LoRa entre devices. Conçu pour les sessions de tests terrain à 3+ devices.

## Ce que tu vois

Une TUI Rich avec trois panneaux empilés :

1. **Devices** — un coup d'œil rapide : alias, solde, taille DAG, verrous actifs, mode horloge (LAMPORT/MASTER), compteur Lamport, état de connexion série.
2. **Convergence DAG** — pour chaque transaction connue d'au moins un device, une matrice `tx_id × device → OK / L / --`. Une colonne `--` persistante = problème de gossip LoRa ou de fragmentation.
3. **Events** — les derniers `ESP_LOGI` contenant `lora`, `dag`, `attest`, `tx`, `merge`, `ack`, `lock`, `fragment`, `sync`, `ping`, `pong`, `broadcast`, `mint`.

## Prérequis

- **Python 3.10+**
- **Firmware buildé avec `CONFIG_MESHPAY_DEBUG_CONSOLE=y`** (défaut tant que `SECURE_FLASH_ENCRYPTION_MODE_RELEASE` n'est pas actif — voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Flash Encryption en mode DEVELOPMENT Audit Sonnet item 5|item 5 de la dette]]).
- **Tous les devices branchés en USB** sur la même machine. Le moniteur n'a pas de vision RF — pour ça il faudrait un sniffer LoRa passif distinct (voir [[#Limites]]).

## Installation

```bash
cd <repo>/tools/meshpay-monitor
pip install -r requirements.txt
```

Dépendances : `pyserial>=3.5`, `rich>=13.0`.

## Trouver les ports série

```bash
python -m serial.tools.list_ports
```

Sur macOS les ports ressemblent à `/dev/cu.usbmodem101`, `/dev/cu.usbmodem201`, etc. Sur Linux ce sont `/dev/ttyUSB0`, `/dev/ttyACM0`. Pour identifier quel port correspond à quel device, débrancher/rebrancher un device à la fois et observer la liste change.

## Lancer le moniteur

```bash
python meshpay_monitor.py \
    --device A=/dev/cu.usbmodem101 \
    --device B=/dev/cu.usbmodem201 \
    --device C=/dev/cu.usbmodem301 \
    --device D=/dev/cu.usbmodem401
```

Le label (`A=`, `B=`, …) est libre — c'est juste l'étiquette d'affichage. Si on omet `LABEL=`, le port lui-même sert d'étiquette.

Quitter : `Ctrl+C`.

### Options

| Option | Défaut | Rôle |
|---|---|---|
| `--device LABEL=PORT`, `-d` | (requis) | À répéter pour chaque device |
| `--baud`, `-b` | `115200` | Baudrate UART |
| `--poll`, `-p` | `5.0` | Intervalle d'envoi de `dump_all` (secondes) |
| `--refresh` | `4.0` | FPS du rendu TUI |

## Comment ça marche

> [!info] Architecture
> - Un **thread par device** ouvre le port série, lit ligne par ligne, et alimente une state-machine de parsing.
> - Un **poll toutes les `--poll` secondes** envoie `dump_all\n` à chaque device.
> - Le firmware répond avec quatre blocs `dump_dag` / `dump_wallet` / `dump_currency` / `dump_time`, chacun encadré par des marqueurs `<<<MESHPAY_DEBUG ... BEGIN/END>>>` et contenant des lignes JSON.
> - Le **main thread** rafraîchit la TUI Rich à `--refresh` FPS sur l'état agrégé.

Le parseur tolère l'**interleaving** des `ESP_LOGI` qui coulent en parallèle sur la même UART : une ligne entre BEGIN et END qui n'est pas du JSON valide est silencieusement ignorée.

## Comment lire chaque vue

### Devices

```text
ID  Alias       Balance  DAG     Locks  Mode    Lamport  Last  Status
A   Brave-Loup  300      12/250  0      MASTER  4523     1s    OK
B   Vif-Renard  120      12/250  1      MASTER  4523     1s    OK
C   Calme-Ours  580      11/250  0      MASTER  4520     2s    OK
D   Sage-Cerf   0        12/250  0      MASTER  4523     1s    OK
```

**Signaux à surveiller** :
- Toutes les `DAG` count doivent **converger** dans les ~2 min (un cycle [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Communication radio hybride|LoRa sync]]). Si C reste à 11 alors que tout le monde est à 12 → un message LoRa perdu.
- Tous les `Mode` doivent être identiques. Si un device reste en `LAMPORT` alors que les autres sont en `MASTER`, il n'a pas encore reçu de `LORA_TIME_SYNC` (voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Authentification cryptographique]]).
- Tous les `Lamport` doivent être proches (à ±quelques unités). Un grand écart = un device qui rate les broadcasts.
- `Last` au-delà de 10-15 s = device probablement déconnecté.

### Convergence DAG

```text
tx_id          A  B  C  D  type      from->to       amt
abcd1234...    OK OK -- OK TRANSFER  ab...->cd...   50
ef560000...    OK OK OK OK MINT      00...->ab...   1000
```

**Signaux à surveiller** :
- Une cellule `--` = ce device **n'a pas la TX dans son DAG**. Si ça persiste > 2 min → le gossip [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/10 - Glossaire et concepts#LoRa|LoRa]] a échoué pour ce device.
- `L` = LOCKED. Si une TX reste `L` indéfiniment sur le destinataire, l'[[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes#I2 attestation signée LoRa|attestation]] n'a pas été émise/reçue.
- Une TX vue par certains devices avec `OK` et d'autres avec `L` = soit la propagation est en cours, soit l'attestation est partielle.

### Events

Flux fusionné des `ESP_LOGI` parlant de LoRa, DAG, attestation, paiement. Utile pour **reconstituer la séquence** d'événements quand une convergence diverge.

## Compromis assumés

> [!warning] Mutex applicatif pris pendant `dump_*`
> Chaque dump prend `s_state_mutex` avec un timeout de 1 s puis écrit en streaming sur l'UART. À 250 TX, le `dump_dag` tient le mutex ~150 ms. Si ton scénario fait beaucoup de paiements/seconde, **augmente `--poll`** (ex. `--poll 10`) pour ne pas perturber `core_task`. En cas de timeout, le firmware émet `{"err":"mutex_timeout"}` et le moniteur conserve le snapshot précédent sans rien casser.

> [!info] Aucun byte en mode production
> Le composant `debug_console` est entièrement gardé par `CONFIG_MESHPAY_DEBUG_CONSOLE`. Quand le flag passe à `n` (ex. quand on bascule [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🔴 Flash Encryption en mode DEVELOPMENT Audit Sonnet item 5|Flash Encryption en mode RELEASE]]), le code entier est strippé par le linker — **zéro octet flash, zéro octet RAM, zéro surface d'attaque**. Pas de checklist humaine à se rappeler en prod.

## Limites

- **Pas de vision RF** : le moniteur observe ce que le firmware **logge**. Si un message LoRa est perdu en radio (interférence, distance, CRC), le device émetteur logge l'envoi et les destinataires ne loguent rien — on voit l'absence mais pas la cause. Pour un diagnostic RF complet, un 5ᵉ ESP32 + module LoRa configuré en réception passive reste nécessaire.
- **ESP32-S3 sans LoRa** : la cible Waveshare a ESP-NOW (paiement direct OK) mais pas de Wio-E5 onboard (voir [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟢 ESP32-S3 sans LoRa]]). Ses Events n'auront pas de lignes LoRa et son DAG ne convergera qu'à travers les TX reçues en ESP-NOW. C'est attendu.
- **Fragments LoRa** : si une TX > 250 octets est fragmentée et qu'un fragment se perd, on voit "réassemblage échoué" mais pas lequel.
- **Tous USB sur une machine** : pas adapté à un test « éparpillé dans l'espace ». Pour ça, prévoir un sniffer LoRa sur batterie.

## Tester le moniteur sans hardware

```bash
cd tools/meshpay-monitor
python -m unittest discover -s tests
```

Couvre la state-machine de parsing des marqueurs et l'extraction des snapshots DAG / wallet / currency / time. **12 cas**.

## Commandes manuelles (sans le moniteur)

Pour debugger un seul device, on peut aussi parler directement à la console de debug avec `screen` ou `picocom` :

```bash
screen /dev/cu.usbmodem101 115200
# Une fois connecté, taper :
help
dump_dag
dump_wallet
dump_currency
dump_time
dump_all
```

Quitter `screen` : `Ctrl+A` puis `K`.

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Le registre DAG]] — structure de la fenêtre DAG observée
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/04 - Décisions techniques#Registre DAG plutôt que blockchain]] — pourquoi un DAG (qui rend la convergence non triviale)
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/09 - Sécurité et durcissement#Authentification cryptographique]] — signatures qui sont vérifiées avant insertion DAG
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/07 - Dette technique#🟡 Tests embarqués non intégrés au runner]] — autre canal de validation, complémentaire

## Notes liées

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/00 - MeshPay (MOC)]] — index de la documentation technique
- [[Mesh Pay (MOOC)]] — hub principal du projet
- Concepts : [[DAG]], [[LoRa]], [[ESP-NOW]]
