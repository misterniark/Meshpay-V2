#include "meshpay/storage.h"

#include <stdlib.h>
#include <string.h>

/* Buffer de travail des blobs (load/save/archive/migrate) : alloué sur le
 * TAS, jamais sur la pile. Leçon apprise au premier flash réel du chantier
 * migration : ces ~1,5 Ko empilés sur le chemin de boot (bootstrap -> migrate
 * -> save) débordaient la pile de la main task (8 Ko) et écrasaient les
 * structures esp_pm -> boot-loop LoadProhibited. Le banc test_app (pile
 * 128 Ko) est structurellement aveugle à ce genre de défaut. */
static uint8_t *blob_buf_alloc(void)
{
    return (uint8_t *)malloc(MESHPAY_STORAGE_BLOB_MAX);
}

static void blob_buf_free(uint8_t *buf)
{
    if (buf != NULL) {
        rns_crypto_secure_zero(buf, MESHPAY_STORAGE_BLOB_MAX);
        free(buf);
    }
}

/* Lecture little-endian explicite du préfixe magic+version : le blob a été
 * écrit en layout natif Xtensa (LE) ; lire par décalages plutôt que par cast
 * de struct rend le routage de schéma indépendant de l'ABI du lecteur. */
static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---------------------------------------------------------------------------
 * Mini CBOR du record v3 (mêmes idiomes que le wire meshpay_tx, restreints) :
 * le record n'admet que des clés entières et des valeurs scalaires (uint) ou
 * chaînes (bstr/tstr). Cette restriction rend le SKIP d'une clé inconnue
 * trivial et sans récursion — c'est elle qui garantit la tolérance forward.
 * ------------------------------------------------------------------------ */

/* Clés de la map v3 — ne JAMAIS réaffecter un numéro (compat des deux sens). */
#define REC_KEY_IDENTITY 1
#define REC_KEY_ALIAS 2
#define REC_KEY_PIN_HASH 3
#define REC_KEY_NEXT_SEQ 4
#define REC_KEY_CHECKPOINT_SEQ 5
#define REC_KEY_CHECKPOINT_HASH 6
#define REC_KEY_CHECKPOINT 7
#define REC_KEY_DESCRIPTOR 8
/* Garde-fou de décodage : une map annonçant plus d'entrées que ça n'est pas
 * un record plausible (8 clés connues + marge généreuse d'inconnues). */
#define REC_MAP_MAX_ENTRIES 64u

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
} rec_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} rec_reader_t;

static esp_err_t rw_put(rec_writer_t *w, uint8_t byte)
{
    if (w->pos >= w->size) {
        return ESP_ERR_NO_MEM;
    }
    w->buf[w->pos++] = byte;
    return ESP_OK;
}

static esp_err_t rw_head(rec_writer_t *w, uint8_t major, uint64_t value)
{
    if (value < 24U) {
        return rw_put(w, (uint8_t)((major << 5) | value));
    }
    if (value <= UINT8_MAX) {
        esp_err_t err = rw_put(w, (uint8_t)((major << 5) | 24U));
        return err != ESP_OK ? err : rw_put(w, (uint8_t)value);
    }
    if (value <= UINT16_MAX) {
        esp_err_t err = rw_put(w, (uint8_t)((major << 5) | 25U));
        if (err == ESP_OK) {
            err = rw_put(w, (uint8_t)(value >> 8));
        }
        return err != ESP_OK ? err : rw_put(w, (uint8_t)value);
    }
    esp_err_t err = rw_put(w, (uint8_t)((major << 5) | 26U));
    for (int shift = 24; err == ESP_OK && shift >= 0; shift -= 8) {
        err = rw_put(w, (uint8_t)(value >> shift));
    }
    return err;
}

static esp_err_t rw_uint_field(rec_writer_t *w, uint64_t key, uint64_t value)
{
    esp_err_t err = rw_head(w, 0, key);
    return err != ESP_OK ? err : rw_head(w, 0, value);
}

static esp_err_t rw_bytes_field(rec_writer_t *w,
                                uint64_t key,
                                uint8_t major,
                                const uint8_t *data,
                                size_t len)
{
    esp_err_t err = rw_head(w, 0, key);
    if (err == ESP_OK) {
        err = rw_head(w, major, len);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len > w->size || w->pos > w->size - len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
    return ESP_OK;
}

static esp_err_t rr_head(rec_reader_t *r, uint8_t *major, uint64_t *value)
{
    if (r->pos >= r->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t initial = r->buf[r->pos++];
    *major = initial >> 5;
    uint8_t additional = initial & 0x1FU;
    if (additional < 24U) {
        *value = additional;
        return ESP_OK;
    }
    size_t bytes;
    if (additional == 24U) {
        bytes = 1;
    } else if (additional == 25U) {
        bytes = 2;
    } else if (additional == 26U) {
        bytes = 4;
    } else if (additional == 27U) {
        bytes = 8;
    } else {
        return ESP_ERR_INVALID_ARG; /* longueurs indéfinies refusées */
    }
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (r->pos >= r->len) {
            return ESP_ERR_INVALID_SIZE;
        }
        v = (v << 8) | r->buf[r->pos++];
    }
    *value = v;
    return ESP_OK;
}

/* Lit la valeur d'une clé chaîne connue : major attendu, longueur bornée. */
static esp_err_t rr_bytes(rec_reader_t *r,
                          uint8_t expected_major,
                          size_t min_len,
                          size_t max_len,
                          uint8_t *out,
                          size_t *out_len)
{
    uint8_t major = 0;
    uint64_t len = 0;
    esp_err_t err = rr_head(r, &major, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (major != expected_major) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < min_len || len > max_len ||
        len > r->len - r->pos) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, r->buf + r->pos, (size_t)len);
    r->pos += (size_t)len;
    if (out_len != NULL) {
        *out_len = (size_t)len;
    }
    return ESP_OK;
}

/* Lit un uint borné u32 (les compteurs du record). */
static esp_err_t rr_u32(rec_reader_t *r, uint32_t *out)
{
    uint8_t major = 0;
    uint64_t v = 0;
    esp_err_t err = rr_head(r, &major, &v);
    if (err != ESP_OK) {
        return err;
    }
    if (major != 0 || v > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (uint32_t)v;
    return ESP_OK;
}

/* Saute la valeur d'une clé INCONNUE : scalaire (rien à faire) ou chaîne
 * (avancer). Tout conteneur/tag/float est refusé — contrainte documentée du
 * format (storage.h) qui garde le skip trivial. */
static esp_err_t rr_skip_value(rec_reader_t *r)
{
    uint8_t major = 0;
    uint64_t v = 0;
    esp_err_t err = rr_head(r, &major, &v);
    if (err != ESP_OK) {
        return err;
    }
    if (major == 0 || major == 1) {
        return ESP_OK;
    }
    if (major == 2 || major == 3) {
        if (v > r->len - r->pos) {
            return ESP_ERR_INVALID_SIZE;
        }
        r->pos += (size_t)v;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------------------
 * Lecteur legacy v2 (chantier migration NVS, M3).
 *
 * Layout FIGÉ de la struct brute que la flotte persistait avant le format
 * CBOR : copie exacte de meshpay_storage_record_t telle qu'elle était au gel
 * (2026-07-15), compilée par la même toolchain Xtensa. Ne JAMAIS modifier ce
 * typedef — c'est une photographie du wire v2, pas une struct vivante. Le
 * static_assert ci-dessous casse la compilation le jour où la struct RAM
 * évolue : il faudra alors remplacer sizeof(meshpay_storage_record_t) par la
 * taille v2 EN DUR (mesurée au banc : voir le test « v2 wire size ») au lieu
 * d'aligner ce gel sur la struct vivante.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t magic;
    uint16_t version;
    bool has_identity;
    bool has_pin_hash;
    bool has_checkpoint;
    uint8_t identity_private[RNS_IDENTITY_PRIVATE_SIZE];
    char alias[MESHPAY_STORAGE_ALIAS_MAX];
    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    uint32_t next_seq;
    uint32_t checkpoint_seq;
    uint8_t checkpoint_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t checkpoint[MESHPAY_STORAGE_CHECKPOINT_MAX];
    size_t checkpoint_len;
    bool has_currency_descriptor;
    uint8_t currency_descriptor[MESHPAY_STORAGE_DESCRIPTOR_MAX];
    size_t currency_descriptor_len;
} record_v2_legacy_t;

_Static_assert(sizeof(record_v2_legacy_t) == sizeof(meshpay_storage_record_t),
               "La struct RAM du record a divergé du gel v2 : graver ici la "
               "taille v2 mesurée sur cible (test « v2 wire size ») au lieu "
               "de suivre la struct vivante.");

#define RECORD_V2_WIRE_SIZE sizeof(record_v2_legacy_t)

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

void meshpay_storage_record_init(meshpay_storage_record_t *record)
{
    if (record == NULL) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->magic = MESHPAY_STORAGE_MAGIC;
    record->version = MESHPAY_STORAGE_VERSION;
}

static bool record_header_valid(const meshpay_storage_record_t *record)
{
    return record != NULL &&
           record->magic == MESHPAY_STORAGE_MAGIC &&
           record->version == MESHPAY_STORAGE_VERSION;
}

esp_err_t meshpay_storage_record_set_identity(meshpay_storage_record_t *record,
                                              const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE])
{
    if (!record_header_valid(record) || private_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(private_key, RNS_CRYPTO_X25519_KEY_SIZE) ||
        bytes_zero(private_key + RNS_CRYPTO_X25519_KEY_SIZE,
                   RNS_CRYPTO_ED25519_SEED_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->identity_private, private_key, RNS_IDENTITY_PRIVATE_SIZE);
    record->has_identity = true;
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_alias(meshpay_storage_record_t *record,
                                           const char *alias)
{
    if (!record_header_valid(record) || alias == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(alias);
    if (len == 0 || len >= MESHPAY_STORAGE_ALIAS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(record->alias, 0, sizeof(record->alias));
    memcpy(record->alias, alias, len);
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_pin_hash(meshpay_storage_record_t *record,
                                              const uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE])
{
    if (!record_header_valid(record) || pin_hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(pin_hash, RNS_CRYPTO_SHA256_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->pin_hash, pin_hash, RNS_CRYPTO_SHA256_SIZE);
    record->has_pin_hash = true;
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_checkpoint(meshpay_storage_record_t *record,
                                                uint32_t checkpoint_seq,
                                                const uint8_t *checkpoint,
                                                size_t checkpoint_len)
{
    if (!record_header_valid(record) ||
        (checkpoint == NULL && checkpoint_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (checkpoint_len == 0 || checkpoint_len > MESHPAY_STORAGE_CHECKPOINT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256(checkpoint, checkpoint_len, hash);
    if (err != ESP_OK) {
        return err;
    }

    memset(record->checkpoint, 0, sizeof(record->checkpoint));
    memcpy(record->checkpoint, checkpoint, checkpoint_len);
    memcpy(record->checkpoint_hash, hash, sizeof(hash));
    record->checkpoint_seq = checkpoint_seq;
    record->checkpoint_len = checkpoint_len;
    record->has_checkpoint = true;
    rns_crypto_secure_zero(hash, sizeof(hash));
    return ESP_OK;
}

esp_err_t meshpay_storage_record_set_currency_descriptor(
    meshpay_storage_record_t *record,
    const uint8_t *descriptor,
    size_t descriptor_len)
{
    if (!record_header_valid(record) || descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (descriptor_len == 0 ||
        descriptor_len > MESHPAY_STORAGE_DESCRIPTOR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Blob opaque : copié tel quel, le reste du buffer remis à zéro pour ne pas
     * persister de résidus. La signature sera vérifiée par la couche currency. */
    memset(record->currency_descriptor, 0, sizeof(record->currency_descriptor));
    memcpy(record->currency_descriptor, descriptor, descriptor_len);
    record->currency_descriptor_len = descriptor_len;
    record->has_currency_descriptor = true;
    return ESP_OK;
}

static esp_err_t validate_record(const meshpay_storage_record_t *record)
{
    if (!record_header_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->alias[MESHPAY_STORAGE_ALIAS_MAX - 1] != '\0') {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->checkpoint_len > MESHPAY_STORAGE_CHECKPOINT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->currency_descriptor_len > MESHPAY_STORAGE_DESCRIPTOR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->has_currency_descriptor &&
        record->currency_descriptor_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->has_identity) {
        if (bytes_zero(record->identity_private, RNS_CRYPTO_X25519_KEY_SIZE) ||
            bytes_zero(record->identity_private + RNS_CRYPTO_X25519_KEY_SIZE,
                       RNS_CRYPTO_ED25519_SEED_SIZE)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (record->has_pin_hash &&
        bytes_zero(record->pin_hash, sizeof(record->pin_hash))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record->has_checkpoint) {
        if (record->checkpoint_len == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
        esp_err_t err = rns_crypto_sha256(record->checkpoint,
                                          record->checkpoint_len,
                                          hash);
        if (err != ESP_OK) {
            return err;
        }
        bool ok = rns_crypto_constant_equal(hash,
                                            record->checkpoint_hash,
                                            sizeof(hash));
        rns_crypto_secure_zero(hash, sizeof(hash));
        if (!ok) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

/* Encode le record au format v3 (préfixe + map CBOR, champs présents
 * seulement). Le blob est de TAILLE VARIABLE : c'est ce qui éteint la
 * fragilité sizeof des schémas struct v1/v2. */
static esp_err_t encode_record(const meshpay_storage_record_t *record,
                               uint8_t *buf,
                               size_t size,
                               size_t *out_len)
{
    if (size < MESHPAY_STORAGE_PREFIX_SIZE) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = (uint8_t)(MESHPAY_STORAGE_MAGIC & 0xFF);
    buf[1] = (uint8_t)((MESHPAY_STORAGE_MAGIC >> 8) & 0xFF);
    buf[2] = (uint8_t)((MESHPAY_STORAGE_MAGIC >> 16) & 0xFF);
    buf[3] = (uint8_t)((MESHPAY_STORAGE_MAGIC >> 24) & 0xFF);
    buf[4] = (uint8_t)(MESHPAY_STORAGE_VERSION & 0xFF);
    buf[5] = (uint8_t)((MESHPAY_STORAGE_VERSION >> 8) & 0xFF);

    rec_writer_t w = {
        .buf = buf,
        .size = size,
        .pos = MESHPAY_STORAGE_PREFIX_SIZE,
    };

    size_t alias_len = strnlen(record->alias, MESHPAY_STORAGE_ALIAS_MAX);
    size_t count = (record->has_identity ? 1U : 0U) +
                   (alias_len > 0 ? 1U : 0U) +
                   (record->has_pin_hash ? 1U : 0U) +
                   (record->next_seq != 0 ? 1U : 0U) +
                   (record->has_checkpoint ? 3U : 0U) +
                   (record->has_currency_descriptor ? 1U : 0U);
    esp_err_t err = rw_head(&w, 5, count);

    if (err == ESP_OK && record->has_identity) {
        err = rw_bytes_field(&w, REC_KEY_IDENTITY, 2, record->identity_private,
                             sizeof(record->identity_private));
    }
    if (err == ESP_OK && alias_len > 0) {
        err = rw_bytes_field(&w, REC_KEY_ALIAS, 3,
                             (const uint8_t *)record->alias, alias_len);
    }
    if (err == ESP_OK && record->has_pin_hash) {
        err = rw_bytes_field(&w, REC_KEY_PIN_HASH, 2, record->pin_hash,
                             sizeof(record->pin_hash));
    }
    if (err == ESP_OK && record->next_seq != 0) {
        err = rw_uint_field(&w, REC_KEY_NEXT_SEQ, record->next_seq);
    }
    if (err == ESP_OK && record->has_checkpoint) {
        err = rw_uint_field(&w, REC_KEY_CHECKPOINT_SEQ, record->checkpoint_seq);
        if (err == ESP_OK) {
            err = rw_bytes_field(&w, REC_KEY_CHECKPOINT_HASH, 2,
                                 record->checkpoint_hash,
                                 sizeof(record->checkpoint_hash));
        }
        if (err == ESP_OK) {
            err = rw_bytes_field(&w, REC_KEY_CHECKPOINT, 2, record->checkpoint,
                                 record->checkpoint_len);
        }
    }
    if (err == ESP_OK && record->has_currency_descriptor) {
        err = rw_bytes_field(&w, REC_KEY_DESCRIPTOR, 2,
                             record->currency_descriptor,
                             record->currency_descriptor_len);
    }
    if (err == ESP_OK && out_len != NULL) {
        *out_len = w.pos;
    }
    return err;
}

/* Décode un corps CBOR v3 vers *record (réinitialisé d'abord). Clé inconnue
 * ignorée, clé connue en double refusée, bornes strictes par champ. En échec
 * le record est laissé RÉINITIALISÉ (jamais de contenu partiel). */
static esp_err_t decode_record(const uint8_t *data,
                               size_t len,
                               meshpay_storage_record_t *record)
{
    meshpay_storage_record_init(record);

    rec_reader_t r = { .buf = data, .len = len, .pos = 0 };
    uint8_t major = 0;
    uint64_t count = 0;
    esp_err_t err = rr_head(&r, &major, &count);
    if (err != ESP_OK) {
        goto fail;
    }
    if (major != 5 || count > REC_MAP_MAX_ENTRIES) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    uint32_t seen = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        err = rr_head(&r, &major, &key);
        if (err != ESP_OK) {
            goto fail;
        }
        if (major != 0) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }
        if (key >= REC_KEY_IDENTITY && key <= REC_KEY_DESCRIPTOR) {
            uint32_t bit = 1UL << key;
            if (seen & bit) {
                err = ESP_ERR_INVALID_ARG; /* doublon de clé connue */
                goto fail;
            }
            seen |= bit;
        }
        switch (key) {
        case REC_KEY_IDENTITY:
            err = rr_bytes(&r, 2, sizeof(record->identity_private),
                           sizeof(record->identity_private),
                           record->identity_private, NULL);
            record->has_identity = (err == ESP_OK);
            break;
        case REC_KEY_ALIAS: {
            uint8_t alias[MESHPAY_STORAGE_ALIAS_MAX - 1];
            size_t alias_len = 0;
            err = rr_bytes(&r, 3, 1, sizeof(alias), alias, &alias_len);
            if (err == ESP_OK && memchr(alias, '\0', alias_len) != NULL) {
                err = ESP_ERR_INVALID_ARG; /* NUL embarqué interdit */
            }
            if (err == ESP_OK) {
                memcpy(record->alias, alias, alias_len);
                record->alias[alias_len] = '\0';
            }
            break;
        }
        case REC_KEY_PIN_HASH:
            err = rr_bytes(&r, 2, sizeof(record->pin_hash),
                           sizeof(record->pin_hash), record->pin_hash, NULL);
            record->has_pin_hash = (err == ESP_OK);
            break;
        case REC_KEY_NEXT_SEQ:
            err = rr_u32(&r, &record->next_seq);
            break;
        case REC_KEY_CHECKPOINT_SEQ:
            err = rr_u32(&r, &record->checkpoint_seq);
            break;
        case REC_KEY_CHECKPOINT_HASH:
            err = rr_bytes(&r, 2, sizeof(record->checkpoint_hash),
                           sizeof(record->checkpoint_hash),
                           record->checkpoint_hash, NULL);
            break;
        case REC_KEY_CHECKPOINT:
            err = rr_bytes(&r, 2, 1, MESHPAY_STORAGE_CHECKPOINT_MAX,
                           record->checkpoint, &record->checkpoint_len);
            record->has_checkpoint = (err == ESP_OK);
            break;
        case REC_KEY_DESCRIPTOR:
            err = rr_bytes(&r, 2, 1, MESHPAY_STORAGE_DESCRIPTOR_MAX,
                           record->currency_descriptor,
                           &record->currency_descriptor_len);
            record->has_currency_descriptor = (err == ESP_OK);
            break;
        default:
            err = rr_skip_value(&r); /* tolérance forward */
            break;
        }
        if (err != ESP_OK) {
            goto fail;
        }
    }
    if (r.pos != r.len) {
        err = ESP_ERR_INVALID_SIZE; /* octets orphelins après la map */
        goto fail;
    }
    return ESP_OK;

fail:
    rns_crypto_secure_zero(record, sizeof(*record));
    meshpay_storage_record_init(record);
    return err;
}

esp_err_t meshpay_storage_save(const meshpay_storage_backend_t *backend,
                               const meshpay_storage_record_t *record)
{
    if (backend == NULL || backend->write_blob == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = validate_record(record);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *buf = blob_buf_alloc();
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    err = encode_record(record, buf, MESHPAY_STORAGE_BLOB_MAX, &len);
    if (err == ESP_OK) {
        err = backend->write_blob(backend->ctx,
                                  MESHPAY_STORAGE_STATE_KEY,
                                  buf,
                                  len);
    }
    blob_buf_free(buf);
    return err;
}

/* Sonde la taille du blob stocké sous `key` sans le lire (contrat backend :
 * read_blob(data==NULL) remplit *size — nvs_get_blob le fait nativement). */
static esp_err_t probe_blob_size(const meshpay_storage_backend_t *backend,
                                 const char *key,
                                 size_t *size)
{
    *size = 0;
    return backend->read_blob(backend->ctx, key, NULL, size);
}

esp_err_t meshpay_storage_load_ex(const meshpay_storage_backend_t *backend,
                                  meshpay_storage_record_t *record,
                                  meshpay_storage_probe_t *probe)
{
    meshpay_storage_probe_t verdict = MESHPAY_STORAGE_PROBE_ERROR;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (backend == NULL || backend->read_blob == NULL || record == NULL) {
        goto out;
    }

    /* 1) Taille d'abord : elle décide si le blob est plausible et permet de
     * lire un schéma ÉTRANGER (plus grand que la struct courante) sans que le
     * backend refuse la lecture. */
    size_t size = 0;
    err = probe_blob_size(backend, MESHPAY_STORAGE_STATE_KEY, &size);
    if (err == ESP_ERR_NOT_FOUND) {
        verdict = MESHPAY_STORAGE_PROBE_EMPTY;
        goto out;
    }
    if (err != ESP_OK) {
        goto out; /* E/S : indéterminé, ne surtout rien conclure. */
    }
    if (size < MESHPAY_STORAGE_PREFIX_SIZE || size > MESHPAY_STORAGE_BLOB_MAX) {
        verdict = MESHPAY_STORAGE_PROBE_CORRUPT;
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    /* 2) Lecture intégrale dans un buffer de travail borné, sur le TAS (cf.
     * blob_buf_alloc — le mettre en pile a brické le premier flash réel). Le
     * buffer peut contenir la clé privée : blob_buf_free zéroïse toujours. */
    uint8_t *buf = blob_buf_alloc();
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    size_t len = MESHPAY_STORAGE_BLOB_MAX;
    err = backend->read_blob(backend->ctx, MESHPAY_STORAGE_STATE_KEY,
                             buf, &len);
    if (err != ESP_OK) {
        blob_buf_free(buf);
        goto out;
    }
    if (len != size) {
        /* Le blob a changé entre la sonde et la lecture : backend incohérent. */
        blob_buf_free(buf);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* 3) Routage par le préfixe stable magic+version. */
    uint32_t magic = get_u32le(buf);
    uint16_t version = get_u16le(buf + 4);
    if (magic != MESHPAY_STORAGE_MAGIC) {
        blob_buf_free(buf);
        verdict = MESHPAY_STORAGE_PROBE_CORRUPT;
        err = ESP_ERR_INVALID_CRC;
        goto out;
    }
    if (version != MESHPAY_STORAGE_VERSION) {
        /* Préfixe sain mais schéma non courant : struct brute v1/v2, ou bump
         * de format FUTUR (downgrade de firmware). Archivable, potentiellement
         * migrable (M3) — jamais chargé tel quel. */
        blob_buf_free(buf);
        verdict = MESHPAY_STORAGE_PROBE_LEGACY;
        err = ESP_ERR_INVALID_VERSION;
        goto out;
    }

    /* 4) v3 : corps CBOR décodé vers le record du caller (réinitialisé en cas
     * d'échec — jamais de contenu partiel), puis invariants d'usage. */
    err = decode_record(buf + MESHPAY_STORAGE_PREFIX_SIZE,
                        size - MESHPAY_STORAGE_PREFIX_SIZE,
                        record);
    blob_buf_free(buf);
    if (err != ESP_OK) {
        verdict = MESHPAY_STORAGE_PROBE_CORRUPT;
        goto out;
    }
    err = validate_record(record);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(record, sizeof(*record));
        meshpay_storage_record_init(record);
        verdict = MESHPAY_STORAGE_PROBE_CORRUPT;
        goto out;
    }

    verdict = MESHPAY_STORAGE_PROBE_OK;
    err = ESP_OK;

out:
    if (probe != NULL) {
        *probe = verdict;
    }
    return err;
}

esp_err_t meshpay_storage_load(const meshpay_storage_backend_t *backend,
                               meshpay_storage_record_t *record)
{
    return meshpay_storage_load_ex(backend, record, NULL);
}

esp_err_t meshpay_storage_archive(const meshpay_storage_backend_t *backend,
                                  bool *archived)
{
    if (archived != NULL) {
        *archived = false;
    }
    if (backend == NULL || backend->read_blob == NULL ||
        backend->write_blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Un backup existant n'est JAMAIS écrasé : le premier témoin archivé est
     * celui d'avant tout geste du firmware, donc le plus précieux. */
    size_t bak_size = 0;
    esp_err_t err = probe_blob_size(backend, MESHPAY_STORAGE_BACKUP_KEY,
                                    &bak_size);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    size_t size = 0;
    err = probe_blob_size(backend, MESHPAY_STORAGE_STATE_KEY, &size);
    if (err != ESP_OK) {
        return err; /* NOT_FOUND : rien à archiver ; sinon E/S. */
    }
    if (size == 0 || size > MESHPAY_STORAGE_BLOB_MAX) {
        /* Trop gros pour être copié sans troncature : on REFUSE (jamais de
         * backup partiel). L'invariant tient parce que l'appelant ne doit
         * alors pas écraser le record — voir storage.h. */
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = blob_buf_alloc();
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = MESHPAY_STORAGE_BLOB_MAX;
    err = backend->read_blob(backend->ctx, MESHPAY_STORAGE_STATE_KEY, buf, &len);
    if (err == ESP_OK && len != size) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = backend->write_blob(backend->ctx, MESHPAY_STORAGE_BACKUP_KEY,
                                  buf, len);
    }
    blob_buf_free(buf);
    if (err == ESP_OK && archived != NULL) {
        *archived = true;
    }
    return err;
}

/* Copie champ à champ du gel v2 vers le record courant. Champ à champ (et
 * pas memcpy de struct) pour survivre au jour où la struct RAM divergera du
 * gel — les invariants (bornes, hash, terminaison alias) sont re-vérifiés
 * par validate_record derrière. */
static void convert_v2(const record_v2_legacy_t *v2,
                       meshpay_storage_record_t *out)
{
    meshpay_storage_record_init(out);
    out->has_identity = v2->has_identity;
    out->has_pin_hash = v2->has_pin_hash;
    out->has_checkpoint = v2->has_checkpoint;
    memcpy(out->identity_private, v2->identity_private,
           sizeof(out->identity_private));
    memcpy(out->alias, v2->alias, sizeof(out->alias));
    memcpy(out->pin_hash, v2->pin_hash, sizeof(out->pin_hash));
    out->next_seq = v2->next_seq;
    out->checkpoint_seq = v2->checkpoint_seq;
    memcpy(out->checkpoint_hash, v2->checkpoint_hash,
           sizeof(out->checkpoint_hash));
    memcpy(out->checkpoint, v2->checkpoint, sizeof(out->checkpoint));
    out->checkpoint_len = v2->checkpoint_len;
    out->has_currency_descriptor = v2->has_currency_descriptor;
    memcpy(out->currency_descriptor, v2->currency_descriptor,
           sizeof(out->currency_descriptor));
    out->currency_descriptor_len = v2->currency_descriptor_len;
}

esp_err_t meshpay_storage_migrate(const meshpay_storage_backend_t *backend,
                                  meshpay_storage_record_t *record,
                                  bool *migrated,
                                  meshpay_storage_probe_t *probe)
{
    if (migrated != NULL) {
        *migrated = false;
    }
    if (backend == NULL || backend->read_blob == NULL ||
        backend->write_blob == NULL || record == NULL) {
        if (probe != NULL) {
            *probe = MESHPAY_STORAGE_PROBE_ERROR;
        }
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_storage_probe_t verdict = MESHPAY_STORAGE_PROBE_ERROR;
    esp_err_t err = meshpay_storage_load_ex(backend, record, &verdict);
    if (probe != NULL) {
        *probe = verdict;
    }
    if (verdict == MESHPAY_STORAGE_PROBE_OK ||
        verdict == MESHPAY_STORAGE_PROBE_EMPTY ||
        verdict == MESHPAY_STORAGE_PROBE_ERROR) {
        return err;
    }
    if (verdict == MESHPAY_STORAGE_PROBE_CORRUPT) {
        /* Irrécupérable : préserver le témoin, remonter le motif du load.
         * L'archive est best-effort — elle n'écrase jamais rien. */
        (void)meshpay_storage_archive(backend, NULL);
        return err;
    }

    /* LEGACY : seul le gel v2 se migre — tout autre schéma est archivé et
     * laissé en place (un lecteur v1 s'écrirait ici si un cas réel surgit). */
    size_t size = 0;
    esp_err_t serr = probe_blob_size(backend, MESHPAY_STORAGE_STATE_KEY, &size);
    if (serr != ESP_OK) {
        return serr;
    }
    /* Sur le TAS (aligné max_align par malloc : le cast v2 est sûr) — voir
     * blob_buf_alloc pour la leçon de pile. */
    uint8_t *buf = blob_buf_alloc();
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = MESHPAY_STORAGE_BLOB_MAX;
    serr = backend->read_blob(backend->ctx, MESHPAY_STORAGE_STATE_KEY,
                              buf, &len);
    if (serr != ESP_OK) {
        blob_buf_free(buf);
        return serr;
    }
    if (len != size || size != RECORD_V2_WIRE_SIZE ||
        get_u16le(buf + 4) != 2U) {
        blob_buf_free(buf);
        (void)meshpay_storage_archive(backend, NULL);
        return ESP_ERR_INVALID_VERSION;
    }

    convert_v2((const record_v2_legacy_t *)buf, record);
    blob_buf_free(buf);
    serr = validate_record(record);
    if (serr != ESP_OK) {
        /* v2 au contenu incohérent : traité comme corrompu. */
        rns_crypto_secure_zero(record, sizeof(*record));
        meshpay_storage_record_init(record);
        if (probe != NULL) {
            *probe = MESHPAY_STORAGE_PROBE_CORRUPT;
        }
        (void)meshpay_storage_archive(backend, NULL);
        return serr;
    }

    /* Invariant : le blob v2 original DOIT être en backup avant d'être
     * remplacé. « Déjà archivé » (témoin plus ancien) satisfait l'invariant. */
    serr = meshpay_storage_archive(backend, NULL);
    if (serr != ESP_OK) {
        rns_crypto_secure_zero(record, sizeof(*record));
        meshpay_storage_record_init(record);
        return serr;
    }
    serr = meshpay_storage_save(backend, record);
    if (serr != ESP_OK) {
        rns_crypto_secure_zero(record, sizeof(*record));
        meshpay_storage_record_init(record);
        return serr;
    }
    if (migrated != NULL) {
        *migrated = true;
    }
    if (probe != NULL) {
        *probe = MESHPAY_STORAGE_PROBE_OK;
    }
    return ESP_OK;
}

esp_err_t meshpay_storage_erase(const meshpay_storage_backend_t *backend)
{
    if (backend == NULL || backend->erase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return backend->erase(backend->ctx, MESHPAY_STORAGE_STATE_KEY);
}
