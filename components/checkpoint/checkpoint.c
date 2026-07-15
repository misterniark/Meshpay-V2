#include "meshpay/checkpoint.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Mini CBOR canonique (mêmes idiomes que meshpay_tx / currency_descriptor :
 * en-têtes au plus court, clés entières croissantes — une seule forme
 * d'octets par contenu, condition du déterminisme de la signature).
 * ------------------------------------------------------------------------ */

#define CP_KEY_CURRENCY 1
#define CP_KEY_GENERATION 2
#define CP_KEY_CREATED 3
#define CP_KEY_DIGEST 4
#define CP_KEY_ACCOUNTS 5
#define CP_KEY_SIGNATURE 6

#define CP_BODY_FIELDS 5U
#define CP_WIRE_FIELDS 6U

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
} cp_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} cp_reader_t;

static esp_err_t cw_put(cp_writer_t *w, uint8_t byte)
{
    if (w->pos >= w->size) {
        return ESP_ERR_NO_MEM;
    }
    w->buf[w->pos++] = byte;
    return ESP_OK;
}

static esp_err_t cw_head(cp_writer_t *w, uint8_t major, uint64_t value)
{
    if (value < 24U) {
        return cw_put(w, (uint8_t)((major << 5) | value));
    }
    if (value <= UINT8_MAX) {
        esp_err_t err = cw_put(w, (uint8_t)((major << 5) | 24U));
        return err != ESP_OK ? err : cw_put(w, (uint8_t)value);
    }
    if (value <= UINT16_MAX) {
        esp_err_t err = cw_put(w, (uint8_t)((major << 5) | 25U));
        if (err == ESP_OK) {
            err = cw_put(w, (uint8_t)(value >> 8));
        }
        return err != ESP_OK ? err : cw_put(w, (uint8_t)value);
    }
    if (value <= UINT32_MAX) {
        esp_err_t err = cw_put(w, (uint8_t)((major << 5) | 26U));
        for (int shift = 24; err == ESP_OK && shift >= 0; shift -= 8) {
            err = cw_put(w, (uint8_t)(value >> shift));
        }
        return err;
    }
    esp_err_t err = cw_put(w, (uint8_t)((major << 5) | 27U));
    for (int shift = 56; err == ESP_OK && shift >= 0; shift -= 8) {
        err = cw_put(w, (uint8_t)(value >> shift));
    }
    return err;
}

static esp_err_t cw_bstr(cp_writer_t *w, const uint8_t *data, size_t len)
{
    esp_err_t err = cw_head(w, 2, len);
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

static esp_err_t cr_head(cp_reader_t *r, uint8_t *major, uint64_t *value)
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
        return ESP_ERR_INVALID_ARG;
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

static esp_err_t cr_expect_uint(cp_reader_t *r, uint64_t *out)
{
    uint8_t major = 0;
    esp_err_t err = cr_head(r, &major, out);
    if (err != ESP_OK) {
        return err;
    }
    return major == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t cr_expect_bstr(cp_reader_t *r,
                                uint8_t *out,
                                size_t expected_len)
{
    uint8_t major = 0;
    uint64_t len = 0;
    esp_err_t err = cr_head(r, &major, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (major != 2 || len != expected_len || len > r->len - r->pos) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, r->buf + r->pos, (size_t)len);
    r->pos += (size_t)len;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */

void meshpay_checkpoint_init(meshpay_checkpoint_t *cp)
{
    if (cp != NULL) {
        memset(cp, 0, sizeof(*cp));
    }
}

const meshpay_checkpoint_account_t *meshpay_checkpoint_find_account(
    const meshpay_checkpoint_t *cp,
    const uint8_t account[RNS_IDENTITY_HASH_SIZE])
{
    if (cp == NULL || account == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < cp->account_count &&
                         i < MESHPAY_CHECKPOINT_MAX_ACCOUNTS; ++i) {
        if (memcmp(cp->accounts[i].account, account,
                   RNS_IDENTITY_HASH_SIZE) == 0) {
            return &cp->accounts[i];
        }
    }
    return NULL;
}

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

/* Invariants d'un corps émissible/adoptable : génération >= 1 (0 = « aucun
 * checkpoint »), currency non nulle, bornes, comptes non nuls et UNIQUES
 * (une table à doublon serait ambiguë : refusée à l'encode ET au decode). */
static esp_err_t validate_body(const meshpay_checkpoint_t *cp)
{
    if (cp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cp->currency_id == 0 || cp->generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cp->account_count > MESHPAY_CHECKPOINT_MAX_ACCOUNTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint16_t i = 0; i < cp->account_count; ++i) {
        if (bytes_zero(cp->accounts[i].account, RNS_IDENTITY_HASH_SIZE)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (uint16_t j = (uint16_t)(i + 1U); j < cp->account_count; ++j) {
            if (memcmp(cp->accounts[i].account, cp->accounts[j].account,
                       RNS_IDENTITY_HASH_SIZE) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t encode_body_fields(cp_writer_t *w,
                                    const meshpay_checkpoint_t *cp,
                                    size_t map_fields)
{
    esp_err_t err = cw_head(w, 5, map_fields);
    if (err == ESP_OK) {
        err = cw_head(w, 0, CP_KEY_CURRENCY);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, cp->currency_id);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, CP_KEY_GENERATION);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, cp->generation);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, CP_KEY_CREATED);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, cp->created_at_ms);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, CP_KEY_DIGEST);
    }
    if (err == ESP_OK) {
        err = cw_bstr(w, cp->horizon_digest, MESHPAY_CHECKPOINT_DIGEST_SIZE);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 0, CP_KEY_ACCOUNTS);
    }
    if (err == ESP_OK) {
        err = cw_head(w, 4, cp->account_count); /* array(N) */
    }
    for (uint16_t i = 0; err == ESP_OK && i < cp->account_count; ++i) {
        const meshpay_checkpoint_account_t *a = &cp->accounts[i];
        err = cw_head(w, 4, 4U); /* [account, balance, seq_floor, clé] */
        if (err == ESP_OK) {
            err = cw_bstr(w, a->account, RNS_IDENTITY_HASH_SIZE);
        }
        if (err == ESP_OK) {
            err = cw_head(w, 0, a->balance);
        }
        if (err == ESP_OK) {
            err = cw_head(w, 0, a->seq_floor);
        }
        if (err == ESP_OK) {
            err = cw_bstr(w, a->member_public, RNS_IDENTITY_PUBLIC_SIZE);
        }
    }
    return err;
}

static esp_err_t encode_internal(const meshpay_checkpoint_t *cp,
                                 bool with_signature,
                                 uint8_t *out,
                                 size_t out_size,
                                 size_t *out_len)
{
    if (cp == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = validate_body(cp);
    if (err != ESP_OK) {
        return err;
    }
    if (out_size < MESHPAY_CHECKPOINT_PREFIX_SIZE) {
        return ESP_ERR_NO_MEM;
    }
    out[0] = (uint8_t)(MESHPAY_CHECKPOINT_MAGIC & 0xFF);
    out[1] = (uint8_t)((MESHPAY_CHECKPOINT_MAGIC >> 8) & 0xFF);
    out[2] = (uint8_t)((MESHPAY_CHECKPOINT_MAGIC >> 16) & 0xFF);
    out[3] = (uint8_t)((MESHPAY_CHECKPOINT_MAGIC >> 24) & 0xFF);
    out[4] = (uint8_t)(MESHPAY_CHECKPOINT_VERSION & 0xFF);
    out[5] = (uint8_t)((MESHPAY_CHECKPOINT_VERSION >> 8) & 0xFF);

    cp_writer_t w = {
        .buf = out,
        .size = out_size,
        .pos = MESHPAY_CHECKPOINT_PREFIX_SIZE,
    };
    err = encode_body_fields(&w, cp,
                             with_signature ? CP_WIRE_FIELDS : CP_BODY_FIELDS);
    if (err == ESP_OK && with_signature) {
        err = cw_head(&w, 0, CP_KEY_SIGNATURE);
        if (err == ESP_OK) {
            err = cw_bstr(&w, cp->founder_signature,
                          MESHPAY_CHECKPOINT_SIGNATURE_SIZE);
        }
    }
    if (err == ESP_OK && out_len != NULL) {
        *out_len = w.pos;
    }
    return err;
}

esp_err_t meshpay_checkpoint_encode_body(const meshpay_checkpoint_t *cp,
                                         uint8_t *out,
                                         size_t out_size,
                                         size_t *out_len)
{
    return encode_internal(cp, false, out, out_size, out_len);
}

esp_err_t meshpay_checkpoint_encode(const meshpay_checkpoint_t *cp,
                                    uint8_t *out,
                                    size_t out_size,
                                    size_t *out_len)
{
    if (cp == NULL ||
        bytes_zero(cp->founder_signature, sizeof(cp->founder_signature))) {
        return ESP_ERR_INVALID_STATE; /* wire complet = signé d'abord */
    }
    return encode_internal(cp, true, out, out_size, out_len);
}

esp_err_t meshpay_checkpoint_compute_hash(const meshpay_checkpoint_t *cp,
                                          uint8_t out_hash[RNS_CRYPTO_SHA256_SIZE])
{
    if (cp == NULL || out_hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Buffer d'encodage sur le TAS : jusqu'à ~12 Ko à 128 comptes — jamais
     * sur une pile de tâche (leçon des chantiers durcissement + migration). */
    uint8_t *buf = (uint8_t *)malloc(MESHPAY_CHECKPOINT_CBOR_MAX);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    esp_err_t err = meshpay_checkpoint_encode_body(
        cp, buf, MESHPAY_CHECKPOINT_CBOR_MAX, &len);
    if (err == ESP_OK) {
        err = rns_crypto_sha256(buf, len, out_hash);
    }
    rns_crypto_secure_zero(buf, MESHPAY_CHECKPOINT_CBOR_MAX);
    free(buf);
    return err;
}

esp_err_t meshpay_checkpoint_sign(meshpay_checkpoint_t *cp,
                                  const rns_identity_t *founder)
{
    if (cp == NULL || founder == NULL || !founder->has_private) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = meshpay_checkpoint_compute_hash(cp, hash);
    if (err == ESP_OK) {
        err = rns_identity_sign(founder, hash, sizeof(hash),
                                cp->founder_signature);
    }
    rns_crypto_secure_zero(hash, sizeof(hash));
    return err;
}

esp_err_t meshpay_checkpoint_verify(const meshpay_checkpoint_t *cp,
                                    const uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE])
{
    if (cp == NULL || founder_public == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = meshpay_checkpoint_compute_hash(cp, hash);
    if (err != ESP_OK) {
        return err;
    }
    rns_identity_t founder;
    err = rns_identity_load_public(&founder, founder_public);
    if (err == ESP_OK) {
        err = rns_identity_verify(&founder, hash, sizeof(hash),
                                  cp->founder_signature);
    }
    rns_crypto_secure_zero(hash, sizeof(hash));
    rns_identity_clear(&founder);
    return err;
}

esp_err_t meshpay_checkpoint_decode(const uint8_t *data,
                                    size_t len,
                                    meshpay_checkpoint_t *cp)
{
    if (data == NULL || cp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < MESHPAY_CHECKPOINT_PREFIX_SIZE ||
        len > MESHPAY_CHECKPOINT_CBOR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    uint16_t version = (uint16_t)(data[4] | (data[5] << 8));
    if (magic != MESHPAY_CHECKPOINT_MAGIC) {
        return ESP_ERR_INVALID_CRC;
    }
    if (version != MESHPAY_CHECKPOINT_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    meshpay_checkpoint_init(cp);
    cp_reader_t r = {
        .buf = data,
        .len = len,
        .pos = MESHPAY_CHECKPOINT_PREFIX_SIZE,
    };
    uint8_t major = 0;
    uint64_t count = 0;
    esp_err_t err = cr_head(&r, &major, &count);
    if (err != ESP_OK) {
        goto fail;
    }
    if (major != 5 || count != CP_WIRE_FIELDS) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    /* Objet canonique : les clés arrivent dans l'ordre EXACT 1..6 — toute
     * déviation (clé inconnue, désordre, doublon) est refusée. */
    for (uint64_t field = 1; field <= CP_WIRE_FIELDS; ++field) {
        uint64_t key = 0;
        err = cr_expect_uint(&r, &key);
        if (err != ESP_OK) {
            goto fail;
        }
        if (key != field) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }
        uint64_t v = 0;
        switch (key) {
        case CP_KEY_CURRENCY:
            err = cr_expect_uint(&r, &v);
            if (err == ESP_OK && (v == 0 || v > UINT32_MAX)) {
                err = ESP_ERR_INVALID_ARG;
            }
            cp->currency_id = (uint32_t)v;
            break;
        case CP_KEY_GENERATION:
            err = cr_expect_uint(&r, &v);
            if (err == ESP_OK && (v == 0 || v > UINT32_MAX)) {
                err = ESP_ERR_INVALID_ARG;
            }
            cp->generation = (uint32_t)v;
            break;
        case CP_KEY_CREATED:
            err = cr_expect_uint(&r, &v);
            cp->created_at_ms = v;
            break;
        case CP_KEY_DIGEST:
            err = cr_expect_bstr(&r, cp->horizon_digest,
                                 MESHPAY_CHECKPOINT_DIGEST_SIZE);
            break;
        case CP_KEY_ACCOUNTS: {
            uint64_t n = 0;
            err = cr_head(&r, &major, &n);
            if (err != ESP_OK) {
                break;
            }
            if (major != 4 || n > MESHPAY_CHECKPOINT_MAX_ACCOUNTS) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            cp->account_count = (uint16_t)n;
            for (uint64_t i = 0; err == ESP_OK && i < n; ++i) {
                uint64_t tuple = 0;
                err = cr_head(&r, &major, &tuple);
                if (err != ESP_OK) {
                    break;
                }
                if (major != 4 || tuple != 4U) {
                    err = ESP_ERR_INVALID_ARG;
                    break;
                }
                meshpay_checkpoint_account_t *a = &cp->accounts[i];
                err = cr_expect_bstr(&r, a->account, RNS_IDENTITY_HASH_SIZE);
                if (err == ESP_OK) {
                    err = cr_expect_uint(&r, &v);
                    if (err == ESP_OK && v > UINT32_MAX) {
                        err = ESP_ERR_INVALID_ARG;
                    }
                    a->balance = (uint32_t)v;
                }
                if (err == ESP_OK) {
                    err = cr_expect_uint(&r, &v);
                    if (err == ESP_OK && v > UINT32_MAX) {
                        err = ESP_ERR_INVALID_ARG;
                    }
                    a->seq_floor = (uint32_t)v;
                }
                if (err == ESP_OK) {
                    err = cr_expect_bstr(&r, a->member_public,
                                         RNS_IDENTITY_PUBLIC_SIZE);
                }
            }
            break;
        }
        case CP_KEY_SIGNATURE:
            err = cr_expect_bstr(&r, cp->founder_signature,
                                 MESHPAY_CHECKPOINT_SIGNATURE_SIZE);
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        if (err != ESP_OK) {
            goto fail;
        }
    }
    if (r.pos != r.len) {
        err = ESP_ERR_INVALID_SIZE; /* octets orphelins */
        goto fail;
    }
    err = validate_body(cp);
    if (err != ESP_OK) {
        goto fail;
    }
    if (bytes_zero(cp->founder_signature, sizeof(cp->founder_signature))) {
        err = ESP_ERR_INVALID_ARG; /* wire complet = signé */
        goto fail;
    }
    return ESP_OK;

fail:
    meshpay_checkpoint_init(cp);
    return err;
}
