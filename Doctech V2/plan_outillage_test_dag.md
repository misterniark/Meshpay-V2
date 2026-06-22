# Plan d'implémentation — Outillage du protocole de test DAG

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fournir l'outillage minimal qui rend le protocole `Doctech V2/protocole_test_dag.md` exécutable : un digest de DAG loggé (preuve de convergence) et un capteur série 4 ports.

**Architecture:** Une fonction pure `meshpay_dag_digest()` dans le composant `dag` (SHA-256 des `id` de TX triés → ordre-indépendant), loggée toutes les 15 s depuis `dag_summary_task` sous `s_runtime.lock`. Un script Python hôte capture les 4 ports série. Le firmware injecteur (5e device) est une phase **différée** (matériel absent).

**Tech Stack:** ESP-IDF 5.4.3 (C), Unity (tests composant via `test_app`), mbedTLS via `rns_crypto`, Python 3 + pyserial (env IDF).

---

## File Structure

- `components/dag/include/meshpay/dag.h` — déclarer `meshpay_dag_digest()`.
- `components/dag/dag.c` — implémenter `meshpay_dag_digest()` (qsort des id + `rns_crypto_sha256`).
- `components/dag/CMakeLists.txt` — garantir `rns_crypto` dans `REQUIRES` (lien SHA-256).
- `components/dag/test/test_dag.c` — 3 tests Unity du digest.
- `main/app_main.c` — log `dag_digest=… count=…` dans `dag_summary_task` (sous `s_runtime.lock`).
- `tools/quad_capture.py` — **créer** : capture 4 ports série avec reset DTR/RTS + préfixe alias.

> Phase différée : `components/mp_injector/` + `main_injector/` (firmware attaquant) — spécifié en Task 4, non implémenté maintenant.

---

## Task 1 : `meshpay_dag_digest()` (composant dag, TDD)

**Files:**
- Modify: `components/dag/include/meshpay/dag.h`
- Modify: `components/dag/dag.c`
- Modify: `components/dag/CMakeLists.txt`
- Test: `components/dag/test/test_dag.c`

- [ ] **Step 1 : Écrire les tests qui échouent**

Ajouter à la fin de `components/dag/test/test_dag.c` (avant toute garde de fin de fichier) :

```c
/* Helper : TX minimale identifiée par un octet de graine (digest ne lit que id). */
static meshpay_tx_t mp_tx_with_id(uint8_t seed)
{
    meshpay_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    memset(tx.id, seed, MESHPAY_TX_ID_SIZE);
    return tx;
}

TEST_CASE("dag digest is identical for the same tx set in different order", "[dag]")
{
    meshpay_dag_t a, b;
    meshpay_dag_init(&a);
    meshpay_dag_init(&b);
    meshpay_tx_t t1 = mp_tx_with_id(0x11);
    meshpay_tx_t t2 = mp_tx_with_id(0x22);
    meshpay_tx_t t3 = mp_tx_with_id(0x33);
    a.transactions[0] = t1; a.transactions[1] = t2; a.transactions[2] = t3; a.count = 3;
    b.transactions[0] = t3; b.transactions[1] = t1; b.transactions[2] = t2; b.count = 3;

    uint8_t da[RNS_CRYPTO_SHA256_SIZE];
    uint8_t db[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&a, da));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&b, db));
    TEST_ASSERT_EQUAL_MEMORY(da, db, RNS_CRYPTO_SHA256_SIZE);
}

TEST_CASE("dag digest differs when one tx differs", "[dag]")
{
    meshpay_dag_t a, b;
    meshpay_dag_init(&a);
    meshpay_dag_init(&b);
    a.transactions[0] = mp_tx_with_id(0x11); a.transactions[1] = mp_tx_with_id(0x22); a.count = 2;
    b.transactions[0] = mp_tx_with_id(0x11); b.transactions[1] = mp_tx_with_id(0x99); b.count = 2;

    uint8_t da[RNS_CRYPTO_SHA256_SIZE];
    uint8_t db[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&a, da));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&b, db));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(da, db, RNS_CRYPTO_SHA256_SIZE));
}

TEST_CASE("dag digest of empty dag is deterministic", "[dag]")
{
    meshpay_dag_t a;
    meshpay_dag_init(&a);
    uint8_t d1[RNS_CRYPTO_SHA256_SIZE];
    uint8_t d2[RNS_CRYPTO_SHA256_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&a, d1));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_dag_digest(&a, d2));
    TEST_ASSERT_EQUAL_MEMORY(d1, d2, RNS_CRYPTO_SHA256_SIZE);
}
```

Si `string.h` n'est pas déjà inclus en haut de `test_dag.c`, ajouter `#include <string.h>`.

- [ ] **Step 2 : Compiler les tests → doit échouer**

Run: `./scripts/idf.sh -C test_app build`
Expected: ÉCHEC de compilation — `implicit declaration of function 'meshpay_dag_digest'`.

- [ ] **Step 3 : Déclarer la fonction dans le header**

Dans `components/dag/include/meshpay/dag.h`, après la déclaration de `meshpay_dag_get_tips(...)` (≈ ligne 51), ajouter :

```c
/* Digest stable de l'ENSEMBLE des transactions (SHA-256 des id triés).
 * Indépendant de l'ordre d'insertion : deux DAG de même contenu => même digest.
 * Sert de critère de convergence pour les tests multi-devices. */
esp_err_t meshpay_dag_digest(const meshpay_dag_t *dag,
                             uint8_t out[RNS_CRYPTO_SHA256_SIZE]);
```

Ajouter `#include "esp_err.h"` en tête de `dag.h` s'il n'y est pas (pour `esp_err_t`).

- [ ] **Step 4 : Implémenter la fonction**

En tête de `components/dag/dag.c`, garantir les includes :

```c
#include <stdlib.h>   /* qsort */
#include <string.h>   /* memcpy, memcmp */
#include "meshpay/rns/rns_crypto.h"
```

Ajouter à la fin de `components/dag/dag.c` :

```c
/* Comparateur d'id pour qsort : ordre lexicographique des id (ensemble canonique). */
static int mp_dag_cmp_id(const void *a, const void *b)
{
    const meshpay_tx_t *const *pa = (const meshpay_tx_t *const *)a;
    const meshpay_tx_t *const *pb = (const meshpay_tx_t *const *)b;
    return memcmp((*pa)->id, (*pb)->id, MESHPAY_TX_ID_SIZE);
}

esp_err_t meshpay_dag_digest(const meshpay_dag_t *dag,
                             uint8_t out[RNS_CRYPTO_SHA256_SIZE])
{
    if (dag == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Scratch statique borné par la fenêtre DAG ; appelé sous s_runtime.lock
     * côté firmware et séquentiellement en test => non-réentrant assumé. */
    static uint8_t scratch[MESHPAY_DAG_MAX_TRANSACTIONS * MESHPAY_TX_ID_SIZE];
    const meshpay_tx_t *ptrs[MESHPAY_DAG_MAX_TRANSACTIONS];

    size_t n = dag->count;
    if (n > MESHPAY_DAG_MAX_TRANSACTIONS) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; ++i) {
        ptrs[i] = &dag->transactions[i];
    }
    qsort(ptrs, n, sizeof(ptrs[0]), mp_dag_cmp_id);
    for (size_t i = 0; i < n; ++i) {
        memcpy(scratch + i * MESHPAY_TX_ID_SIZE, ptrs[i]->id, MESHPAY_TX_ID_SIZE);
    }
    return rns_crypto_sha256(scratch, n * MESHPAY_TX_ID_SIZE, out);
}
```

- [ ] **Step 5 : Garantir le lien vers `rns_crypto`**

Ouvrir `components/dag/CMakeLists.txt`. Si `rns_crypto` n'est pas listé dans `REQUIRES`, l'y ajouter. Exemple attendu :

```cmake
idf_component_register(SRCS "dag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES meshpay_tx rns_crypto)
```

- [ ] **Step 6 : Recompiler les tests → doit passer**

Run: `./scripts/idf.sh -C test_app build`
Expected: SUCCÈS de compilation. Puis exécuter la suite Unity sur une carte de banc (flash + monitor du runner `test_app`) et vérifier que les 3 cas `[dag]` digest passent. À défaut de device libre, la compilation verte vaut validation statique (les tests s'exécuteront au prochain passage banc).

- [ ] **Step 7 : Commit**

```bash
git add components/dag/include/meshpay/dag.h components/dag/dag.c components/dag/CMakeLists.txt components/dag/test/test_dag.c
git commit -m "feat(dag): meshpay_dag_digest, empreinte SHA-256 stable de la DAG (tests Unity)"
```

---

## Task 2 : Log `dag_digest` périodique (firmware honnête)

**Files:**
- Modify: `main/app_main.c` (fonction `dag_summary_task`, ≈ lignes 1636-1658)

- [ ] **Step 1 : Ajouter le log digest sous mutex**

Dans `dag_summary_task`, à l'intérieur de la boucle `while (true)`, **juste après** le bloc `if (err != ESP_OK) { … }` et **avant** `vTaskDelay(...)`, insérer :

```c
        if (xSemaphoreTake(s_runtime.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            uint8_t dag_digest[RNS_CRYPTO_SHA256_SIZE];
            size_t dag_n = meshpay_dag_count(&s_app.dag);
            esp_err_t digest_err = meshpay_dag_digest(&s_app.dag, dag_digest);
            xSemaphoreGive(s_runtime.lock);
            if (digest_err == ESP_OK) {
                ESP_LOGI(TAG, "dag_digest=%02x%02x%02x%02x count=%u",
                         dag_digest[0], dag_digest[1], dag_digest[2], dag_digest[3],
                         (unsigned)dag_n);
            }
        }
```

Vérifier que `main/app_main.c` inclut déjà `meshpay/dag.h` et `meshpay/rns/rns_crypto.h` (le composant `app_main`/`main` dépend déjà de `dag`). Sinon, ajouter les `#include` et la dépendance dans `main/CMakeLists.txt`.

- [ ] **Step 2 : Compiler le firmware honnête (profil secure espnow-only)**

Run:
```bash
rm -f build-hardware-smoke-s3-secure/sdkconfig
./scripts/hardware_smoke.sh build-s3-secure
```
Expected: `Project build complete`, `meshpayv2.bin` produit, 0 erreur.

- [ ] **Step 3 : Commit**

```bash
git add main/app_main.c main/CMakeLists.txt
git commit -m "feat(app): log periodique dag_digest pour la convergence (sous s_runtime.lock)"
```

> Déploiement sur les 4 cartes : via `flash-encrypted` (procédure éprouvée), hors-plan (action de banc).

---

## Task 3 : Script `quad_capture.py` (capture 4 ports)

**Files:**
- Create: `tools/quad_capture.py`

- [ ] **Step 1 : Créer le script**

Créer `tools/quad_capture.py` :

```python
#!/usr/bin/env python3
"""Capture simultanee des 4 cartes de test MeshPayV2.
Reset DTR/RTS de chaque port puis log horodate + prefixe par alias.
A lancer dans l'env IDF (pyserial dispo) :
  source ~/.espressif/v5.4.3/esp-idf/export.sh
  python3 tools/quad_capture.py [duree_s]
"""
import sys, time, threading
import serial

PORTS = {
    "loup-sobre":   "/dev/cu.usbmodem11101",
    "loup-doux":    "/dev/cu.usbmodem11201",
    "orque-curieux":"/dev/cu.usbmodem11301",
    "castor-precis":"/dev/cu.usbmodem11401",
}
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
T0 = time.time()

def reset_into_app(p):
    # Sequence projet : reset hors download -> boot app
    p.setRTS(True);  p.setDTR(False); time.sleep(0.1)
    p.setDTR(True);  p.setRTS(False); time.sleep(0.05); p.setDTR(False)

def capture(alias, port):
    try:
        p = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"[{alias}] OPEN ERROR: {e}", flush=True)
        return
    reset_into_app(p)
    buf = b""
    while time.time() - T0 < DURATION:
        try:
            data = p.read(512)
        except Exception as e:
            print(f"[{alias}] READ ERROR: {e}", flush=True)
            break
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf-8", "replace").rstrip()
            if txt:
                print(f"{time.time()-T0:7.2f} [{alias:13}] {txt}", flush=True)
    try:
        p.close()
    except Exception:
        pass

threads = [threading.Thread(target=capture, args=(a, p), daemon=True)
           for a, p in PORTS.items()]
for t in threads:
    t.start()
for t in threads:
    t.join()
print(f"--- capture terminee ({DURATION:.0f}s) ---", flush=True)
```

- [ ] **Step 2 : Vérifier la syntaxe**

Run: `python3 -c "import ast; ast.parse(open('tools/quad_capture.py').read()); print('OK')"`
Expected: `OK`

- [ ] **Step 3 : Test fonctionnel (banc)**

Run (env IDF actif) : `python3 tools/quad_capture.py 8`
Expected: lignes préfixées `[loup-sobre]`, `[loup-doux]`, `[orque-curieux]`, `[castor-precis]` avec les logs de boot et `dag_digest=…` (après Task 2 déployée). Si un port n'apparaît pas, vérifier le branchement / le nom de port.

- [ ] **Step 4 : Commit**

```bash
git add tools/quad_capture.py
git commit -m "tools: quad_capture.py — capture serie simultanee des 4 cartes de test"
```

---

## Task 4 (DIFFÉRÉE — 5e device absent) : Firmware injecteur

> À détailler en tâches TDD complètes **à l'arrivée du 5e ESP32-S3**. Spécification figée ici pour ne rien perdre.

**But:** un firmware « attaquant » qui s'annonce sur le mesh ESP-NOW puis émet, à la demande, des paquets DATA Reticulum **forgés** vers une destination honnête, pour valider le rejet (Phase 2 du protocole).

**Structure prévue:** nouveau `main_injector/` (ou option Kconfig `MESHPAY_BUILD_INJECTOR`) réutilisant `rns_node`, `rns_packet`, `rns_packet_crypto`, `meshpay_tx`, `currency`, `device_hal` (ESP-NOW). Pas de wallet/UI.

**Forges à produire (cf. protocole §4.1):**
- **F1** signature invalide : TX bien formée puis 1 octet de `signature` flippé avant émission → rejet `payment reject verify`.
- **F2** double-dépense : deux TX `(from,seq)` identiques, `id`/montant différents → rejet `merge=CONFLICT`.
- **F3** parent inexistant : `parents[0]` = hash aléatoire absent → rejet `merge=MISSING_PARENT`.
- **F4** règle monnaie : `amount` > solde, ou `fee >= amount`, ou MINT non autorisé → rejet `tx_for_us/currency`.

**Déclenchement (pas de CLI):** émission séquentielle temporisée au boot (une forge toutes les N s, log `inject sent forge=Fx hash=…`), ou tap écran si l'injecteur a un écran.

**Validation:** sur chaque carte honnête, le rejet attendu apparaît dans les logs et `dag_digest`/`count` restent inchangés (capturé via `quad_capture.py`).

**Pré-requis avant d'écrire les tâches TDD:** explorer `rns_node`/`rns_packet`/`meshpay_tx` pour la construction et l'émission d'un paquet DATA signé (et sa corruption ciblée).

---

## Self-Review

- **Couverture spec :** Task 1+2 ⇒ critère de convergence `dag_digest` (protocole §2b, §5). Task 3 ⇒ capture multi-port (§4.3) servant Phases 1/3/4. Task 4 ⇒ injecteur (§4.1) servant Phase 2. Les Phases 1/3/4 ne dépendent que de Task 1-3 (réalisables sans le 5e device). ✓
- **Placeholders :** aucun dans Task 1-3 (code complet). Task 4 est explicitement différée et marquée comme telle (matériel absent) — pas un placeholder masqué.
- **Cohérence des types :** `meshpay_dag_digest(const meshpay_dag_t*, uint8_t[32])→esp_err_t` utilisé à l'identique en Task 1 (def/test) et Task 2 (appel). `RNS_CRYPTO_SHA256_SIZE=32`, `MESHPAY_TX_ID_SIZE=32`, `s_runtime.lock`, `s_app.dag`, `meshpay_dag_count` conformes au code relevé. ✓

## Ordre d'exécution recommandé

Task 1 → Task 2 → Task 3 (immédiat, 4 cartes présentes). Task 4 quand le 5e device arrive. Après Task 1-3 déployées, les Phases 1, 3 et 4 du protocole sont exécutables ; la Phase 2 attend Task 4.
