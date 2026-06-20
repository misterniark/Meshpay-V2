---
tags:
  - meshpay
  - design
  - dag
  - lora
  - epaper
  - monitoring
Projets:
  - Mesh Pay
Topics:
  - Documentation technique
  - Tests terrain
  - Synchronisation
  - Hardware ESP32-S3
Date: 2026-05-19
status: Prototype terrain implemente, build e-paper et ESP32-S3 OK
---

# Moniteur DAG LoRa e-paper

> [!summary]
> Cette note documente le mode **moniteur passif DAG LoRa** sur LILYGO T5 E-Paper S3 Pro 4.7 H752. L'objectif n'est pas de payer avec ce device, mais d'avoir une carte terrain qui ecoute le mesh LoRa, verifie les messages critiques, affiche la sante du DAG et aide a diagnostiquer la convergence sans modifier le ledger.

## 1. Intention

Le moniteur DAG LoRa repond a un besoin tres concret : pendant les tests terrain, le moniteur serie multi-device ([[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/11 - Moniteur multi-device série]]) donne une excellente vue quand tous les devices sont branches en USB, mais il ne voit pas directement le reseau radio.

Le mode e-paper sert donc de **tableau de bord autonome** :

- ecouter les paquets LoRa du reseau MeshPay ;
- verifier les signatures et formats avant de compter un evenement comme sain ;
- suivre les peers vus par `DAG_SUMMARY` ;
- detecter les regressions de fenetre DAG, conflits de sequence, doublons et signatures invalides ;
- afficher une carte de presence via PING/PONG LoRa multi-hop ;
- rester passif : ne pas confirmer, ne pas merger, ne pas emettre de `DAG_REQUEST` de rattrapage automatique.

Le bon mental model : c'est un observateur de coherence reseau, pas un wallet.

## 2. Cible materielle

La cible active est :

| Element | Choix |
|---|---|
| Carte | LILYGO T5 E-Paper S3 Pro 4.7 revision **H752** |
| MCU | ESP32-S3 |
| Affichage | ED047TC1 via driver natif LilyGo, pas `epdiy` |
| Tactile | GT911/CST sur I2C |
| Radio | SX1262 integre, backend `CORE1262` |
| Build | ESP-IDF 5.4.3 via `./scripts/idf.sh` |

Point crucial : cette carte **n'est pas** la revision H752-01 / PRO avec expander PCA9535 et driver `epdiy`. La H752 utilise le chemin `hal_display_lilygo_t5s3_h752` + composant `lilygo_epd47_h752`.

Cette distinction a debloque l'affichage : chercher un PCA9535 sur l'ancien bus I2C menait a un diagnostic faux. La bonne piste etait le driver LilyGO ED047TC1 natif avec tactile sur SDA=6 / SCL=5.

## 3. Fonctionnalites visibles

### Dashboard DAG

L'ecran `DAG monitor` affiche :

| Zone | Contenu |
|---|---|
| Sante | Score `0..100` calcule a partir des anomalies observees |
| Compteurs | RX, TX, TX DAG vues, `DAG_SUMMARY`, `DAG_REQUEST`, erreurs de format, signatures invalides |
| Peers | cle courte, `tx_count_window`, nombre de tips, dernier timestamp TX annonce |
| Alertes | derniers warnings/criticals en anneau |

Le score de sante n'est pas une preuve cryptographique globale ; c'est un indicateur terrain. Il baisse quand le moniteur voit des signaux qui meritent inspection.

### Alertes detectees

Le composant `dag_monitor` remonte notamment :

- paquet malforme ;
- signature invalide ;
- echec d'emission LoRa ;
- TX deja vue recemment ;
- conflit actif `(from, seq)` avec deux `tx_id` differents ;
- parent zero sur une TX non-MINT ;
- `last_tx_timestamp` qui recule dans un `DAG_SUMMARY` ;
- fenetre TX qui diminue sans checkpoint plus recent ;
- peer qui annonce des TX mais aucun tip ;
- tips qui changent sans avance du timestamp.

Les alertes ont trois niveaux : `INFO`, `WARN`, `CRIT`. L'UI garde les 8 dernieres, le moteur garde jusqu'a 12 peers.

### Carte LoRa

L'ecran `LoRa map` lance un PING LoRa signe et affiche les PONG recus :

- alias du device ;
- cle publique courte ;
- compteur de reponses ;
- etat `Pret`, `Ping multi-hop en cours`, `Scan termine`, `Aucune reponse`.

En mode debug, `CONFIG_MESHPAY_LORA_DISCOVERY_PING=y` autorise un moniteur non-maitre a envoyer ce PING. En release, ce point doit rester controle : exposer aliases et pubkeys publiquement n'est pas toujours souhaitable.

## 4. Mode passif

Le flag important est :

```text
CONFIG_MESHPAY_DAG_MONITOR_ONLY=y
```

Ses effets :

- l'UI demarre directement sur `UI_SCREEN_DAG_MONITOR`, sans flow PIN/setup ;
- ESP-NOW est desactive pour reserver RAM et attention systeme au LoRa ;
- `lora_sync` decode et observe les paquets, mais ne pousse pas les TX/attestations vers `core_task` ;
- les cycles de sync LoRa ne publient pas de TX locales ;
- un `DAG_SUMMARY` plus recent ne declenche pas de `DAG_REQUEST` automatique depuis le moniteur.

Le bypass PIN est volontaire dans ce mode : le device n'expose pas les actions dangereuses de wallet/admin. Sur un firmware normal, l'acces admin reste protege par `ui_pin` et `PIN admin`.

Depuis l'accueil, quand le firmware est compile en monitor-only, les boutons sont remappes :

| Firmware normal | Firmware monitor-only |
|---|---|
| Payer | DAG |
| Recevoir | Pairs |
| Histo. | Histo. |
| Param. | Param. |

## 5. Architecture logicielle

### Chemin de donnees

```text
SX1262 / LoRa
    -> hal_lora_core1262
    -> lora_sync_handle_rx()
    -> verification format + signature
    -> dag_monitor_record_*()
    -> ui_screen_dag_monitor
```

Le moniteur est volontairement branche dans `lora_sync`, pas dans `core_task`. Il observe donc aussi les messages que le firmware normal rejetterait avant application au ledger.

### Cablage interne

| Fichier | Role |
|---|---|
| `components/dag_monitor/include/dag_monitor/dag_monitor.h` | snapshot, peers, alertes, API record/snapshot |
| `components/dag_monitor/src/dag_monitor.c` | calcul sante, detection anomalies, mutex interne |
| `components/comm/lora_sync/src/lora_sync.c` | instrumentation RX/TX via `monitor_packet()` et `dag_monitor_record_*()` |
| `main/transport/transport_lora.c` | passe `s_dag_monitor` et `passive_monitor` a `lora_sync` |
| `main/main.c` | initialise `dag_monitor`, le donne a l'UI, desactive ESP-NOW en monitor-only |
| `components/ui/src/ui_screen_dag_monitor.c` | dashboard LVGL |
| `components/ui/src/ui_screen_lora_map.c` | PING/PONG terrain |

### Thread-safety

`dag_monitor_t` a son propre mutex FreeRTOS. Les callbacks LoRa enregistrent les evenements sous mutex, et l'UI lit une copie de snapshot via `dag_monitor_snapshot()`.

Ce decouplage evite de reprendre `s_state_mutex` dans le moniteur et respecte la correction deja faite dans `lora_sync` : la couche communication ne tient plus directement le mutex applicatif du DAG.

## 6. Specs radio et protocole observe

Configuration radio actuelle :

| Parametre | Valeur |
|---|---|
| Frequence | 868.1 MHz |
| Spreading factor | SF9 |
| Bandwidth | 125 kHz |
| Coding rate | 4/5 |
| Puissance | 14 dBm |
| Payload LoRa max | 255 octets |
| Fragment utile | 251 octets |
| Fragments max | 16 |
| Timeout reassemblage | 10 s |
| TX max par cycle | 8 |

Messages observes :

| Type | Code | Usage |
|---|---:|---|
| `LORA_TX` | `0x10` | TX unique confirmee |
| `LORA_FRAG` | `0x11` | Fragment de TX trop grosse |
| `LORA_TIME_SYNC` | `0x12` | temps maitre |
| `LORA_BROADCAST` | `0x13` | message maitre |
| `LORA_PING` | `0x14` | decouverte terrain |
| `LORA_PONG` | `0x15` | reponse signee |
| `LORA_ATTESTATION` | `0x18` | preuve signee de confirmation |
| `LORA_DAG_SUMMARY` | `0x19` | resume de fenetre DAG |
| `LORA_DAG_REQUEST` | `0x1A` | rattrapage depuis timestamp |
| `LORA_DAG_ATTEST_BATCH` | `0x1B` | lot d'attestations |

En firmware normal, le cycle LoRa est de 120 s, avec jitter ±25 %. En configuration de seed/test device, il descend a 15 s pour rendre les bancs observables plus vite. En monitor-only, cette cadence de publication est neutralisee : la carte ecoute et affiche.

## 7. UI e-paper

Le H752 n'est pas un petit LCD rafraichi en continu. L'UI a donc ete adaptee :

- rendu LVGL plein ecran dans un framebuffer grayscale ;
- rafraichissement e-paper limite a environ 2,5 s ;
- nettoyage e-paper avant dessin (`epd_clear_area_cycles`) ;
- trois passes de rendu pour renforcer la lisibilite ;
- dilation des pixels noirs pour compenser le rendu trop fin ;
- panneaux plus grands, padding augmente et rayon reduit pour mieux tenir sur e-paper ;
- labels mis a jour seulement si le texte change, pour eviter des refresh inutiles.

L'ecran DAG est dense par design : il privilegie les chiffres et alertes lisibles de loin plutot qu'une navigation riche.

## 8. GPIO et pinout qui ont compte

### LoRa SX1262 integre H752

Pinout retenu dans `sdkconfig.defaults.esp32s3` :

| Signal | GPIO |
|---|---:|
| SCK | 18 |
| MOSI | 17 |
| MISO | 8 |
| NSS / CS | 46 |
| RESET | 43 |
| BUSY | 44 |
| DIO1 | 3 |
| RXEN | -1 |
| TXEN | -1 |

Sur le H752, le SX1262 integre n'utilise pas les memes pins que le montage Waveshare Core1262 externe. Il n'y a pas de RXEN/TXEN exposes : le switch RF passe par DIO2, donc `RXEN=-1` et `TXEN=-1`.

### E-paper ED047TC1

L'e-paper H752 utilise le bus parallele ED047TC1 :

| Signal | GPIO |
|---|---:|
| CFG_DATA | 2 |
| CFG_CLK | 42 |
| CFG_STR | 1 |
| CKV | 39 |
| STH | 9 |
| CKH | 10 |
| D7..D0 | 38, 45, 47, 21, 14, 13, 12, 11 |

Le driver utilise le chemin `esp_lcd` I80 (`USER_I2S_REG=0`) plutot que l'ancien acces brut aux registres I2S ESP32. Sur ESP32-S3, ce choix a ete determinant : l'ancien layout de registres I2S ne correspondait plus assez bien et menait a des hangs de transfert.

Deux details ont stabilise le rendu :

- buffer ligne DMA aligne 64 octets en RAM interne DMA ;
- `.clk_src = LCD_CLK_SRC_DEFAULT` dans la config I80, pour eviter le crash sur source d'horloge inconnue avec ESP-IDF 5.4.3.

### Touch et backlight

| Fonction | GPIO / adresse |
|---|---|
| I2C SDA | 6 |
| I2C SCL | 5 |
| Touch INT | 15 |
| Touch RST | 41 |
| Backlight | 40 |
| GT911 attendu | `0x5D` |
| GT911 alternatif | `0x14` |
| CST | `0x5A` |

Le tactile n'est pas critique pour l'observation pure, mais il rend le mode `Pairs` utilisable sur le terrain.

## 9. Ce qui a permis d'en venir a bout

### Identifier la vraie revision H752

Le blocage initial venait d'une hypothese materielle fausse : traiter la carte comme une H752-01 / PRO avec `epdiy` et PCA9535. Le H752 reel utilise un driver LilyGO ED047TC1 sans PCA9535. Le signe utile : rien de coherent sur SDA=39 / SCL=40, alors que le tactile repond sur SDA=6 / SCL=5.

### Separer le moniteur du ledger

Le mode passif a clarifie le design :

- observer les paquets LoRa est utile ;
- appliquer ces paquets au ledger est une autre responsabilite ;
- le moniteur ne doit pas changer l'etat qu'il observe.

Ce choix a rendu l'UI fiable : une erreur de reseau devient une alerte, pas une mutation de DAG semi-valide.

### Brancher le diagnostic au bon niveau

Instrumenter `lora_sync` permet de voir :

- les paquets refuses avant `core_task` ;
- les signatures invalides ;
- les fragments reassembles ;
- les `DAG_SUMMARY`/`DAG_REQUEST` avant toute decision applicative.

Si l'observation avait ete faite seulement apres merge DAG, les anomalies radio ou crypto auraient disparu dans les logs.

### Respecter les contraintes ESP32-S3

Les points bas niveau qui ont compte :

- ne pas appeler de log lourd depuis les chemins proches ISR ;
- garder le callback de fin DMA minimal (`output_done = true`, retour `false`) ;
- utiliser `esp_lcd` I80 plutot que les registres I2S bruts ;
- aligner les buffers DMA ;
- ne pas demarrer LoRa avant queues/mutex valides ;
- capturer les erreurs d'init LoRa et eviter tout appel a un pointeur HAL non initialise ;
- reserver de la RAM en monitor-only en coupant ESP-NOW.

### Flash encryption

Le device ESP32-S3 a deja le flash encryption actif en mode DEVELOPMENT. Pour les validations hardware, il faut eviter les flashs plaintext inutiles et preferer les commandes `encrypted-flash` deja validees pour ce projet.

## 10. Build et validation

Builds valides le 2026-05-19 avec ESP-IDF 5.4.3 :

```zsh
./scripts/idf.sh -B build-epaper build
./scripts/idf.sh -B build-s3 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
```

Resultat :

| Build | Resultat | Binaire |
|---|---|---|
| `build-epaper` | OK | `offline-payment.bin` |
| `build-s3` | OK | `offline-payment.bin` |

Taille app observee : `0xdb420`, avec environ 51 % libre dans la plus petite partition app.

Warnings non bloquants observes :

- `LV_TICK_CUSTOM` inconnu dans les defaults ;
- migration `CONFIG_ESP32S3_SPIRAM_SUPPORT` vers `CONFIG_SPIRAM` ;
- warnings `IRAM_ATTR` sur les declarations/definitions du driver LilyGO ;
- headers legacy RMT / periph_ctrl de l'ancien driver e-paper.

## 11. Limites

- Le score de sante est local au moniteur : il ne prouve pas que tout le reseau converge, seulement ce que la carte a entendu.
- Le moniteur ne sniffe pas les collisions radio invisibles ; il voit surtout les absences et les anomalies decodables.
- En monitor-only, il n'envoie pas de `DAG_REQUEST` automatique : c'est volontaire pour rester observateur.
- Le PING/PONG de cartographie est utile en debug terrain, mais doit rester controle en release.
- Le rendu e-paper est volontairement lent ; ce n'est pas une TUI temps reel.

## Voir aussi

- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/02 - Architecture générale#Le registre DAG]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/11 - Moniteur multi-device série]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/14 - Driver LoRa Core1262 (design)]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/16 - DAG et sync LoRa v2 (design)]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/13 - Gestion de l'énergie (design)]]
- [[Misterniark/Projet/Mesh Pay/Doctech V2/Ancienne note technique/08 - Journal des corrections récentes]]
- Concepts : [[DAG]], [[LoRa]], [[ESP-NOW]]
