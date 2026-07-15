/*
 * dag_store — persistance de la fenêtre DAG (snapshot) sur partition flash.
 *
 * Disposition de la partition : 2 slots (A à l'offset 0, B à size/2). Chaque
 * sauvegarde écrit dans le slot le PLUS ANCIEN (double-buffer) avec une
 * génération incrémentée ; le chargement prend le slot valide de génération la
 * plus haute. Un slot partiellement écrit (coupure) échoue au CRC → on retombe
 * sur l'autre slot.
 *
 * Slot = [header][records (count × meshpay_tx_t)][footer].
 *   header = magic, version, generation, count, record_size
 *   footer = digest(32) + crc32(header+records) + magic2
 *
 * Écriture : sur partition `encrypted`, esp_partition_write exige offset ET
 * longueur alignés sur 16 o. On sérialise donc tout le slot dans un buffer RAM
 * (rembourré au multiple de 16) et on l'écrit en UNE passe depuis la base du
 * slot (16-alignée). La lecture, elle, n'a aucune contrainte d'alignement.
 */
#include "meshpay/dag_store.h"

#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dag_store";

#define DAG_STORE_MAGIC 0x53474144u  /* 'D','A','G','S' */
#define DAG_STORE_MAGIC2 0x46534744u /* footer */
#define DAG_STORE_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint32_t count;
    uint32_t record_size;
} dag_store_header_t;

typedef struct __attribute__((packed)) {
    uint8_t digest[RNS_CRYPTO_SHA256_SIZE];
    uint32_t crc32;
    uint32_t magic2;
} dag_store_footer_t;

typedef struct {
    bool valid;
    uint32_t generation;
    uint32_t count;
} slot_info_t;

/* CRC32 IEEE (poly réfléchi 0xEDB88320), sans table — autonome (pas de dép.). */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static size_t slot_base(const meshpay_dag_store_backend_t *be, int slot)
{
    return slot == 0 ? 0U : be->size / 2U;
}

/*
 * La disposition exige que la base du slot B (size/2) ET les effacements soient
 * alignés sur le secteur. On l'impose explicitement : une partition mal
 * dimensionnée échoue franchement plutôt que de corrompre la flash en silence.
 * (Le wrap d'uint32 de `generation` est ignoré : la flash meurt à ~100 K
 * écritures, des milliards d'ordres de grandeur avant 2^32.)
 */
static bool backend_layout_ok(const meshpay_dag_store_backend_t *be)
{
    return be->erase_size != 0U && be->size >= 2U * be->erase_size &&
           (be->size / 2U) % be->erase_size == 0U;
}

static size_t slot_capacity(const meshpay_dag_store_backend_t *be)
{
    return be->size / 2U;
}

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1U) / a * a;
}

/* Lit l'en-tête + footer d'un slot, valide magic/version/taille/CRC. */
static esp_err_t read_slot_info(const meshpay_dag_store_backend_t *be, int slot,
                                slot_info_t *out)
{
    out->valid = false;
    out->generation = 0;
    out->count = 0;

    size_t base = slot_base(be, slot);
    dag_store_header_t h;
    esp_err_t err = be->read(be->ctx, base, &h, sizeof(h));
    if (err != ESP_OK) {
        return err;
    }
    if (h.magic != DAG_STORE_MAGIC || h.version != DAG_STORE_VERSION ||
        h.record_size != sizeof(meshpay_tx_t) ||
        h.count > MESHPAY_DAG_MAX_TRANSACTIONS) {
        return ESP_OK; /* slot invalide/vierge — pas une erreur d'E/S */
    }
    size_t records_len = (size_t)h.count * sizeof(meshpay_tx_t);
    if (sizeof(h) + records_len + sizeof(dag_store_footer_t) > slot_capacity(be)) {
        return ESP_OK;
    }

    /* CRC sur header + records (lecture en flux pour ne pas allouer). */
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, &h, sizeof(h));
    uint8_t chunk[256];
    size_t off = base + sizeof(h);
    size_t remaining = records_len;
    while (remaining > 0) {
        size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        err = be->read(be->ctx, off, chunk, n);
        if (err != ESP_OK) {
            return err;
        }
        crc = crc32_update(crc, chunk, n);
        off += n;
        remaining -= n;
    }
    crc ^= 0xFFFFFFFFu;

    dag_store_footer_t f;
    err = be->read(be->ctx, base + sizeof(h) + records_len, &f, sizeof(f));
    if (err != ESP_OK) {
        return err;
    }
    if (f.magic2 != DAG_STORE_MAGIC2 || f.crc32 != crc) {
        return ESP_OK; /* footer absent/incohérent => slot non committé */
    }

    out->valid = true;
    out->generation = h.generation;
    out->count = h.count;
    return ESP_OK;
}

esp_err_t meshpay_dag_store_save(const meshpay_dag_store_backend_t *backend,
                                 const meshpay_dag_t *dag,
                                 const char *reason)
{
    if (backend == NULL || backend->read == NULL || backend->write == NULL ||
        backend->erase == NULL || backend->size == 0 || dag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!backend_layout_ok(backend)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t count = meshpay_dag_count(dag);
    if (count > MESHPAY_DAG_MAX_TRANSACTIONS) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t records_len = count * sizeof(meshpay_tx_t);
    size_t total = sizeof(dag_store_header_t) + records_len +
                   sizeof(dag_store_footer_t);
    if (total > slot_capacity(backend)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Choix du slot cible (on écrase le plus ancien / un slot invalide). */
    slot_info_t s0, s1;
    esp_err_t err = read_slot_info(backend, 0, &s0);
    if (err != ESP_OK) {
        return err;
    }
    err = read_slot_info(backend, 1, &s1);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t maxgen = 0;
    if (s0.valid && s0.generation > maxgen) {
        maxgen = s0.generation;
    }
    if (s1.valid && s1.generation > maxgen) {
        maxgen = s1.generation;
    }
    int target;
    if (!s0.valid) {
        target = 0;
    } else if (!s1.valid) {
        target = 1;
    } else {
        target = (s0.generation <= s1.generation) ? 0 : 1;
    }
    uint32_t new_gen = maxgen + 1U;

    /* Sérialisation dans un buffer aligné 16 o (contrainte écriture chiffrée). */
    size_t padded = round_up(total, 16U);
    uint8_t *buf = malloc(padded);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xFF, padded);

    dag_store_header_t *h = (dag_store_header_t *)buf;
    h->magic = DAG_STORE_MAGIC;
    h->version = DAG_STORE_VERSION;
    h->reserved = 0;
    h->generation = new_gen;
    h->count = (uint32_t)count;
    h->record_size = (uint32_t)sizeof(meshpay_tx_t);

    for (size_t i = 0; i < count; ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        memcpy(buf + sizeof(dag_store_header_t) + i * sizeof(meshpay_tx_t),
               tx, sizeof(meshpay_tx_t));
    }

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf, sizeof(dag_store_header_t) + records_len);
    crc ^= 0xFFFFFFFFu;

    dag_store_footer_t *f =
        (dag_store_footer_t *)(buf + sizeof(dag_store_header_t) + records_len);
    err = meshpay_dag_digest(dag, f->digest);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    f->crc32 = crc;
    f->magic2 = DAG_STORE_MAGIC2;

    /* Effacement du slot cible (aligné secteur) puis écriture unique. */
    size_t base = slot_base(backend, target);
    size_t erase_len = round_up(padded, backend->erase_size);
    if (erase_len > slot_capacity(backend)) {
        erase_len = slot_capacity(backend);
    }
    err = backend->erase(backend->ctx, base, erase_len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    err = backend->write(backend->ctx, base, buf, padded);
    free(buf);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "saved slot=%d gen=%u count=%u reason=%s", target,
             (unsigned)new_gen, (unsigned)count, reason != NULL ? reason : "");
    return ESP_OK;
}

esp_err_t meshpay_dag_store_load(const meshpay_dag_store_backend_t *backend,
                                 meshpay_dag_t *dag)
{
    if (backend == NULL || backend->read == NULL || backend->size == 0 ||
        dag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!backend_layout_ok(backend)) {
        return ESP_ERR_INVALID_SIZE;
    }

    slot_info_t s0, s1;
    esp_err_t err = read_slot_info(backend, 0, &s0);
    if (err != ESP_OK) {
        return err;
    }
    err = read_slot_info(backend, 1, &s1);
    if (err != ESP_OK) {
        return err;
    }

    int best = -1;
    uint32_t best_gen = 0;
    if (s0.valid) {
        best = 0;
        best_gen = s0.generation;
    }
    if (s1.valid && (best < 0 || s1.generation > best_gen)) {
        best = 1;
        best_gen = s1.generation;
    }
    if (best < 0) {
        return ESP_ERR_NOT_FOUND; /* aucun slot valide (premier boot) */
    }

    size_t base = slot_base(backend, best);
    uint32_t count = (best == 0) ? s0.count : s1.count;

    meshpay_dag_init(dag);
    if (count > 0) {
        err = backend->read(backend->ctx, base + sizeof(dag_store_header_t),
                            dag->transactions,
                            (size_t)count * sizeof(meshpay_tx_t));
        if (err != ESP_OK) {
            meshpay_dag_init(dag);
            return err;
        }
    }
    dag->count = count;

    /* Contrôle d'intégrité supplémentaire : le digest recalculé doit coïncider
     * avec celui stocké (le CRC a déjà validé les octets bruts). */
    dag_store_footer_t f;
    err = backend->read(backend->ctx,
                        base + sizeof(dag_store_header_t) +
                            (size_t)count * sizeof(meshpay_tx_t),
                        &f, sizeof(f));
    if (err != ESP_OK) {
        meshpay_dag_init(dag); /* ne pas court-circuiter le contrôle en silence */
        return err;
    }
    uint8_t digest[RNS_CRYPTO_SHA256_SIZE];
    if (meshpay_dag_digest(dag, digest) == ESP_OK &&
        memcmp(digest, f.digest, sizeof(digest)) != 0) {
        meshpay_dag_init(dag);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "loaded slot=%d gen=%u count=%u", best, (unsigned)best_gen,
             (unsigned)count);
    return ESP_OK;
}

/* CRC32 complet d'un buffer (init/final 0xFFFFFFFF), pour la zone checkpoint. */
static uint32_t crc32_ieee(const void *data, size_t len)
{
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/* --- Phase B : zone checkpoint en queue de partition --- */

#define CP_STORE_MAGIC 0x44535043u  /* 'C','P','S','D' */
#define CP_STORE_MAGIC2 0x43505346u /* footer */
#define CP_STORE_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint32_t length; /* longueur du wire CBOR signé qui suit */
} cp_store_header_t;

typedef struct __attribute__((packed)) {
    uint32_t crc32;
    uint32_t magic2;
} cp_store_footer_t;

static size_t cp_slot_size(const meshpay_dag_store_backend_t *be)
{
    return round_up(sizeof(cp_store_header_t) + MESHPAY_CHECKPOINT_CBOR_MAX +
                        sizeof(cp_store_footer_t),
                    be->erase_size);
}

static size_t cp_slot_base(const meshpay_dag_store_backend_t *be, int slot)
{
    size_t z = cp_slot_size(be);
    return be->size - (size_t)(2 - slot) * z;
}

/* Lit le header d'un slot checkpoint ; rend la génération valide ou 0. */
static uint32_t cp_slot_generation(const meshpay_dag_store_backend_t *be,
                                   int slot)
{
    cp_store_header_t hdr;
    if (be->read(be->ctx, cp_slot_base(be, slot), &hdr, sizeof(hdr)) !=
        ESP_OK) {
        return 0;
    }
    if (hdr.magic != CP_STORE_MAGIC || hdr.version != CP_STORE_VERSION ||
        hdr.length == 0 || hdr.length > MESHPAY_CHECKPOINT_CBOR_MAX ||
        hdr.generation == 0) {
        return 0;
    }
    return hdr.generation;
}

esp_err_t meshpay_dag_store_save_checkpoint(
    const meshpay_dag_store_backend_t *backend,
    const meshpay_checkpoint_t *cp,
    const char *reason)
{
    if (backend == NULL || backend->read == NULL || backend->write == NULL ||
        backend->erase == NULL || cp == NULL || cp->generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* La zone doit tenir dans la partition SANS mordre le slot fenêtre 1
     * (capacité réelle du snapshot fenêtre << size/2, vérifié ici). */
    size_t z = cp_slot_size(backend);
    if (backend->size < 2 * z ||
        backend->size / 2U + slot_capacity(backend) >
            backend->size) { /* garde structurelle (layout historique) */
        return ESP_ERR_INVALID_SIZE;
    }

    /* Slot inactif = génération la plus basse (0 = libre/invalide). */
    uint32_t gen0 = cp_slot_generation(backend, 0);
    uint32_t gen1 = cp_slot_generation(backend, 1);
    int target = (gen0 <= gen1) ? 0 : 1;

    /* Un buffer unique [header|wire|footer] paddé 16 o (contrainte d'écriture
     * en flash CHIFFRÉE — même pattern que le save fenêtre), sur le TAS, une
     * seule écriture dans le slot INACTIF : une coupure laisse l'autre slot
     * (le checkpoint précédent) intact. */
    size_t max_total = sizeof(cp_store_header_t) +
                       MESHPAY_CHECKPOINT_CBOR_MAX +
                       sizeof(cp_store_footer_t);
    size_t padded_max = round_up(max_total, 16U);
    uint8_t *buf = (uint8_t *)malloc(padded_max);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xFF, padded_max);

    size_t wire_len = 0;
    esp_err_t err = meshpay_checkpoint_encode(
        cp, buf + sizeof(cp_store_header_t), MESHPAY_CHECKPOINT_CBOR_MAX,
        &wire_len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    cp_store_header_t *hdr = (cp_store_header_t *)buf;
    hdr->magic = CP_STORE_MAGIC;
    hdr->version = CP_STORE_VERSION;
    hdr->reserved = 0;
    hdr->generation = cp->generation;
    hdr->length = (uint32_t)wire_len;
    cp_store_footer_t *footer =
        (cp_store_footer_t *)(buf + sizeof(cp_store_header_t) + wire_len);
    footer->crc32 = crc32_ieee(buf + sizeof(cp_store_header_t), wire_len);
    footer->magic2 = CP_STORE_MAGIC2;

    size_t total = sizeof(cp_store_header_t) + wire_len +
                   sizeof(cp_store_footer_t);
    size_t padded = round_up(total, 16U);
    size_t base = cp_slot_base(backend, target);
    err = backend->erase(backend->ctx, base, z);
    if (err == ESP_OK) {
        err = backend->write(backend->ctx, base, buf, padded);
    }
    free(buf);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "checkpoint saved slot=%d gen=%u len=%u reason=%s",
                 target, (unsigned)cp->generation, (unsigned)wire_len,
                 reason != NULL ? reason : "?");
    }
    return err;
}

esp_err_t meshpay_dag_store_load_checkpoint(
    const meshpay_dag_store_backend_t *backend,
    meshpay_checkpoint_t *out_cp)
{
    if (backend == NULL || backend->read == NULL || out_cp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (backend->size < 2 * cp_slot_size(backend)) {
        return ESP_ERR_NOT_FOUND;
    }

    int best = -1;
    uint32_t best_gen = 0;
    for (int slot = 0; slot < 2; ++slot) {
        uint32_t gen = cp_slot_generation(backend, slot);
        if (gen > best_gen) {
            /* Valide le footer AVANT d'élire le slot (commit incomplet =
             * slot ignoré). */
            cp_store_header_t hdr;
            size_t base = cp_slot_base(backend, slot);
            if (backend->read(backend->ctx, base, &hdr, sizeof(hdr)) !=
                ESP_OK) {
                continue;
            }
            uint8_t *wire = (uint8_t *)malloc(hdr.length);
            if (wire == NULL) {
                return ESP_ERR_NO_MEM;
            }
            cp_store_footer_t footer;
            esp_err_t err = backend->read(backend->ctx, base + sizeof(hdr),
                                          wire, hdr.length);
            if (err == ESP_OK) {
                err = backend->read(backend->ctx,
                                    base + sizeof(hdr) + hdr.length, &footer,
                                    sizeof(footer));
            }
            if (err == ESP_OK && footer.magic2 == CP_STORE_MAGIC2 &&
                footer.crc32 == crc32_ieee(wire, hdr.length) &&
                meshpay_checkpoint_decode(wire, hdr.length, out_cp) ==
                    ESP_OK) {
                best = slot;
                best_gen = gen;
            }
            free(wire);
        }
    }
    if (best < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "checkpoint loaded slot=%d gen=%u", best,
             (unsigned)best_gen);
    return ESP_OK;
}
