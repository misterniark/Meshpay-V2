#include "meshpay/currency_descriptor.h"

#include "esp_check.h"
#include "meshpay/rns/rns_destination.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "currency_descriptor";

/*
 * Clés CBOR (entiers) du descripteur. Les clés 1..9 forment le corps signable
 * (préimage du genesis) ; 10 et 11 sont les champs dérivés du wire complet.
 * L'ordre croissant stable est garanti par l'ordre d'écriture dans
 * encode_body_fields — c'est ce qui rend le genesis déterministe.
 */
#define CBOR_KEY_FOUNDER 1
#define CBOR_KEY_NAME 2
#define CBOR_KEY_SYMBOL 3
#define CBOR_KEY_MAX_SUPPLY 4
#define CBOR_KEY_TRANSFER_FEE 5
#define CBOR_KEY_DEMURRAGE_ENABLED 6
#define CBOR_KEY_DEMURRAGE_BPS 7
#define CBOR_KEY_INITIAL_CREDIT 8
#define CBOR_KEY_CREATED_AT 9
#define CBOR_KEY_GENESIS 10
#define CBOR_KEY_SIGNATURE 11

/* Nombre de champs du corps (map signable) et du wire complet. */
#define BODY_FIELD_COUNT 9
#define FULL_FIELD_COUNT 11

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
} cbor_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} cbor_reader_t;

/* --- Writer CBOR maison (même style que meshpay_tx) --- */

static esp_err_t writer_put(cbor_writer_t *writer, uint8_t byte)
{
    if (writer == NULL || writer->buf == NULL || writer->pos >= writer->size) {
        return ESP_ERR_NO_MEM;
    }
    writer->buf[writer->pos++] = byte;
    return ESP_OK;
}

static esp_err_t writer_put_bytes(cbor_writer_t *writer,
                                  const uint8_t *data,
                                  size_t len)
{
    if (writer == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Bornes : refuse tout débordement du buffer de sortie. */
    if (len > writer->size || writer->pos > writer->size - len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(writer->buf + writer->pos, data, len);
    writer->pos += len;
    return ESP_OK;
}

/* Encode un en-tête CBOR (major type + valeur) sur la plus courte forme. */
static esp_err_t cbor_write_type_value(cbor_writer_t *writer,
                                       uint8_t major,
                                       uint64_t value)
{
    if (value < 24U) {
        return writer_put(writer, (uint8_t)((major << 5) | value));
    }
    if (value <= UINT8_MAX) {
        ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)((major << 5) | 24U)),
                            TAG, "");
        return writer_put(writer, (uint8_t)value);
    }
    if (value <= UINT16_MAX) {
        ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)((major << 5) | 25U)),
                            TAG, "");
        ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)(value >> 8)), TAG, "");
        return writer_put(writer, (uint8_t)value);
    }
    if (value <= UINT32_MAX) {
        ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)((major << 5) | 26U)),
                            TAG, "");
        for (int shift = 24; shift >= 0; shift -= 8) {
            ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)(value >> shift)),
                                TAG, "");
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)((major << 5) | 27U)),
                        TAG, "");
    for (int shift = 56; shift >= 0; shift -= 8) {
        ESP_RETURN_ON_ERROR(writer_put(writer, (uint8_t)(value >> shift)),
                            TAG, "");
    }
    return ESP_OK;
}

static esp_err_t cbor_write_uint(cbor_writer_t *writer, uint64_t value)
{
    return cbor_write_type_value(writer, 0, value);
}

static esp_err_t cbor_write_bstr(cbor_writer_t *writer,
                                 const uint8_t *data,
                                 size_t len)
{
    if (len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(cbor_write_type_value(writer, 2, len), TAG, "");
    return writer_put_bytes(writer, data, len);
}

static esp_err_t cbor_write_map(cbor_writer_t *writer, size_t count)
{
    if (count > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return cbor_write_type_value(writer, 5, count);
}

/*
 * Longueur bornée d'une chaîne stockée dans un buffer fixe. On s'arrête au
 * premier '\0' OU à la borne du buffer : garantit qu'on n'encode jamais
 * d'octets résiduels non initialisés (déterminisme) et qu'une chaîne non
 * terminée reste bornée.
 */
static size_t bounded_strlen(const char *str, size_t max)
{
    size_t i = 0;
    while (i < max && str[i] != '\0') {
        ++i;
    }
    return i;
}

/*
 * Écrit les 9 champs RÈGLES du corps dans l'ordre croissant des clés.
 * Partagé entre l'encodage signable (genesis) et l'encodage wire complet, afin
 * que les octets soient strictement identiques dans les deux cas.
 */
static esp_err_t encode_body_fields(cbor_writer_t *writer,
                                    const meshpay_currency_descriptor_t *body)
{
    /* 1 : clé publique du fondateur (64 o). */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_FOUNDER), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, body->founder_public,
                                        sizeof(body->founder_public)),
                        TAG, "");

    /* 2 : nom (uniquement les octets utiles, sans le terminateur). */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_NAME), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, (const uint8_t *)body->name,
                                        bounded_strlen(body->name,
                                                       MESHPAY_CURRENCY_NAME_MAX)),
                        TAG, "");

    /* 3 : symbole/ticker (octets utiles seulement). */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_SYMBOL), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, (const uint8_t *)body->symbol,
                                        bounded_strlen(body->symbol,
                                                       MESHPAY_CURRENCY_SYMBOL_MAX)),
                        TAG, "");

    /* 4 : offre maximale. */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_MAX_SUPPLY), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, body->max_supply), TAG, "");

    /* 5 : frais de transfert. */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_TRANSFER_FEE), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, body->transfer_fee), TAG, "");

    /* 6 : fonte activée (0/1). */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_DEMURRAGE_ENABLED), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, body->demurrage_enabled ? 1U : 0U),
                        TAG, "");

    /* 7 : taux de fonte en points de base. */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_DEMURRAGE_BPS), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, body->demurrage_bps), TAG, "");

    /* 8 : crédit initial. */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_INITIAL_CREDIT), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, body->initial_credit), TAG, "");

    /* 9 : horodatage de création. */
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_CREATED_AT), TAG, "");
    return cbor_write_uint(writer, body->created_at_ms);
}

void meshpay_currency_descriptor_init(meshpay_currency_descriptor_t *body)
{
    if (body != NULL) {
        memset(body, 0, sizeof(*body));
    }
}

esp_err_t meshpay_currency_descriptor_encode_body(const meshpay_currency_descriptor_t *body,
                                                  uint8_t *out,
                                                  size_t out_size,
                                                  size_t *out_len)
{
    if (body == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cbor_writer_t writer = {
        .buf = out,
        .size = out_size,
        .pos = 0,
    };
    ESP_RETURN_ON_ERROR(cbor_write_map(&writer, BODY_FIELD_COUNT), TAG, "");
    ESP_RETURN_ON_ERROR(encode_body_fields(&writer, body), TAG, "");
    *out_len = writer.pos;
    return ESP_OK;
}

esp_err_t meshpay_currency_descriptor_compute_genesis(const meshpay_currency_descriptor_t *body,
                                                      uint8_t out_genesis[MESHPAY_CURRENCY_GENESIS_SIZE],
                                                      uint32_t *out_currency_id)
{
    if (body == NULL || out_genesis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Préimage = encodage canonique du corps. */
    uint8_t encoded[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t encoded_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_currency_descriptor_encode_body(body, encoded,
                                                                sizeof(encoded),
                                                                &encoded_len),
                        TAG, "");
    ESP_RETURN_ON_ERROR(rns_crypto_sha256(encoded, encoded_len, out_genesis),
                        TAG, "");
    if (out_currency_id != NULL) {
        /* currency_id = 4 octets de tête du genesis, big-endian. */
        *out_currency_id = ((uint32_t)out_genesis[0] << 24) |
                           ((uint32_t)out_genesis[1] << 16) |
                           ((uint32_t)out_genesis[2] << 8) |
                           ((uint32_t)out_genesis[3]);
    }
    return ESP_OK;
}

esp_err_t meshpay_currency_descriptor_sign(meshpay_currency_descriptor_signed_t *out_signed,
                                           const meshpay_currency_descriptor_t *body,
                                           const rns_identity_t *founder)
{
    if (out_signed == NULL || body == NULL || founder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_signed, 0, sizeof(*out_signed));
    /* Copie le corps fourni, puis renseigne l'autorité depuis l'identité. */
    out_signed->body = *body;
    ESP_RETURN_ON_ERROR(rns_identity_get_public_key(founder,
                                                    out_signed->body.founder_public),
                        TAG, "");
    /* Genesis/currency_id calculés sur le corps DÉFINITIF (avec founder_public). */
    ESP_RETURN_ON_ERROR(meshpay_currency_descriptor_compute_genesis(&out_signed->body,
                                                                    out_signed->genesis_hash,
                                                                    &out_signed->currency_id),
                        TAG, "");
    /* Signature du fondateur sur le genesis (32 o). */
    return rns_identity_sign(founder, out_signed->genesis_hash,
                             sizeof(out_signed->genesis_hash),
                             out_signed->founder_signature);
}

esp_err_t meshpay_currency_descriptor_verify(const meshpay_currency_descriptor_signed_t *signed_desc)
{
    if (signed_desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) Recalcule le genesis depuis le corps et le compare au genesis stocké :
     *    un corps trafiqué après signature produit un genesis différent. */
    uint8_t computed_genesis[MESHPAY_CURRENCY_GENESIS_SIZE];
    ESP_RETURN_ON_ERROR(meshpay_currency_descriptor_compute_genesis(&signed_desc->body,
                                                                    computed_genesis,
                                                                    NULL),
                        TAG, "");
    if (!rns_crypto_constant_equal(computed_genesis, signed_desc->genesis_hash,
                                   sizeof(computed_genesis))) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 2) Recharge l'identité publique du fondateur depuis le corps et vérifie la
     *    signature sur le genesis. */
    rns_identity_t founder;
    ESP_RETURN_ON_ERROR(rns_identity_load_public(&founder,
                                                 signed_desc->body.founder_public),
                        TAG, "");
    return rns_identity_verify(&founder, signed_desc->genesis_hash,
                               sizeof(signed_desc->genesis_hash),
                               signed_desc->founder_signature);
}

esp_err_t meshpay_currency_descriptor_encode(const meshpay_currency_descriptor_signed_t *signed_desc,
                                             uint8_t *out,
                                             size_t out_size,
                                             size_t *out_len)
{
    if (signed_desc == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cbor_writer_t writer = {
        .buf = out,
        .size = out_size,
        .pos = 0,
    };
    /* Map complète : 9 champs du corps + genesis + signature. */
    ESP_RETURN_ON_ERROR(cbor_write_map(&writer, FULL_FIELD_COUNT), TAG, "");
    ESP_RETURN_ON_ERROR(encode_body_fields(&writer, &signed_desc->body), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(&writer, CBOR_KEY_GENESIS), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(&writer, signed_desc->genesis_hash,
                                        sizeof(signed_desc->genesis_hash)),
                        TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(&writer, CBOR_KEY_SIGNATURE), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(&writer, signed_desc->founder_signature,
                                        sizeof(signed_desc->founder_signature)),
                        TAG, "");

    if (writer.pos > MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = writer.pos;
    return ESP_OK;
}

/* --- Reader CBOR maison (même style que meshpay_tx) --- */

static esp_err_t reader_get(cbor_reader_t *reader, uint8_t *out)
{
    if (reader == NULL || out == NULL || reader->pos >= reader->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out = reader->buf[reader->pos++];
    return ESP_OK;
}

static esp_err_t cbor_read_type_value(cbor_reader_t *reader,
                                      uint8_t expected_major,
                                      uint64_t *out)
{
    if (reader == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t initial = 0;
    ESP_RETURN_ON_ERROR(reader_get(reader, &initial), TAG, "");
    uint8_t major = initial >> 5;
    uint8_t additional = initial & 0x1fU;
    if (major != expected_major || additional == 31U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (additional < 24U) {
        *out = additional;
        return ESP_OK;
    }

    size_t bytes = 0;
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

    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte = 0;
        ESP_RETURN_ON_ERROR(reader_get(reader, &byte), TAG, "");
        value = (value << 8) | byte;
    }
    *out = value;
    return ESP_OK;
}

static esp_err_t cbor_read_uint(cbor_reader_t *reader, uint64_t *out)
{
    return cbor_read_type_value(reader, 0, out);
}

/* Lit un bstr de longueur EXACTE attendue (rejet sinon). */
static esp_err_t cbor_read_bstr_fixed(cbor_reader_t *reader,
                                      uint8_t *out,
                                      size_t expected_len)
{
    uint64_t len = 0;
    ESP_RETURN_ON_ERROR(cbor_read_type_value(reader, 2, &len), TAG, "");
    if (len != expected_len ||
        len > reader->len ||
        reader->pos > reader->len - (size_t)len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, reader->buf + reader->pos, (size_t)len);
    reader->pos += (size_t)len;
    return ESP_OK;
}

/*
 * Lit un bstr de longueur VARIABLE (chaîne) dans un buffer fixe : rejette si la
 * longueur dépasse (max_capacity - 1) afin de garantir un terminateur nul. Le
 * buffer destination est entièrement remis à zéro au préalable.
 */
static esp_err_t cbor_read_string(cbor_reader_t *reader,
                                  char *out,
                                  size_t max_capacity)
{
    uint64_t len = 0;
    ESP_RETURN_ON_ERROR(cbor_read_type_value(reader, 2, &len), TAG, "");
    if (len >= max_capacity ||
        len > reader->len ||
        reader->pos > reader->len - (size_t)len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(out, 0, max_capacity);
    memcpy(out, reader->buf + reader->pos, (size_t)len);
    reader->pos += (size_t)len;
    return ESP_OK;
}

static esp_err_t cbor_read_map_len(cbor_reader_t *reader, uint64_t *out)
{
    return cbor_read_type_value(reader, 5, out);
}

esp_err_t meshpay_currency_descriptor_decode(const uint8_t *data,
                                             size_t len,
                                             meshpay_currency_descriptor_signed_t *out_signed)
{
    if (data == NULL || out_signed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cbor_reader_t reader = {
        .buf = data,
        .len = len,
        .pos = 0,
    };
    memset(out_signed, 0, sizeof(*out_signed));

    uint64_t map_len = 0;
    ESP_RETURN_ON_ERROR(cbor_read_map_len(&reader, &map_len), TAG, "");
    if (map_len != FULL_FIELD_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    meshpay_currency_descriptor_t *body = &out_signed->body;
    uint32_t present = 0;
    for (uint64_t i = 0; i < map_len; ++i) {
        uint64_t key = 0;
        ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &key), TAG, "");
        if (key > 31U) {
            return ESP_ERR_INVALID_ARG;
        }
        present |= (uint32_t)(1UL << key);

        uint64_t value = 0;
        switch (key) {
        case CBOR_KEY_FOUNDER:
            ESP_RETURN_ON_ERROR(cbor_read_bstr_fixed(&reader, body->founder_public,
                                                     sizeof(body->founder_public)),
                                TAG, "");
            break;
        case CBOR_KEY_NAME:
            ESP_RETURN_ON_ERROR(cbor_read_string(&reader, body->name,
                                                 sizeof(body->name)),
                                TAG, "");
            break;
        case CBOR_KEY_SYMBOL:
            ESP_RETURN_ON_ERROR(cbor_read_string(&reader, body->symbol,
                                                 sizeof(body->symbol)),
                                TAG, "");
            break;
        case CBOR_KEY_MAX_SUPPLY:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &body->max_supply),
                                TAG, "");
            break;
        case CBOR_KEY_TRANSFER_FEE:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            body->transfer_fee = (uint32_t)value;
            break;
        case CBOR_KEY_DEMURRAGE_ENABLED:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > 1U) {
                return ESP_ERR_INVALID_ARG;
            }
            body->demurrage_enabled = (value != 0U);
            break;
        case CBOR_KEY_DEMURRAGE_BPS:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT16_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            body->demurrage_bps = (uint16_t)value;
            break;
        case CBOR_KEY_INITIAL_CREDIT:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            body->initial_credit = (uint32_t)value;
            break;
        case CBOR_KEY_CREATED_AT:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &body->created_at_ms),
                                TAG, "");
            break;
        case CBOR_KEY_GENESIS:
            ESP_RETURN_ON_ERROR(cbor_read_bstr_fixed(&reader,
                                                     out_signed->genesis_hash,
                                                     sizeof(out_signed->genesis_hash)),
                                TAG, "");
            break;
        case CBOR_KEY_SIGNATURE:
            ESP_RETURN_ON_ERROR(cbor_read_bstr_fixed(&reader,
                                                     out_signed->founder_signature,
                                                     sizeof(out_signed->founder_signature)),
                                TAG, "");
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Toutes les clés 1..11 doivent être présentes, et la totalité du buffer
     * consommée (pas d'octets parasites en fin). */
    uint32_t required = 0;
    for (uint8_t key = CBOR_KEY_FOUNDER; key <= CBOR_KEY_SIGNATURE; ++key) {
        required |= (uint32_t)(1UL << key);
    }
    if ((present & required) != required || reader.pos != len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Dérive le currency_id depuis le genesis lu (4 octets de tête, big-endian).
     * Ne vérifie PAS la signature : l'appelant enchaîne avec verify(). */
    out_signed->currency_id = ((uint32_t)out_signed->genesis_hash[0] << 24) |
                              ((uint32_t)out_signed->genesis_hash[1] << 16) |
                              ((uint32_t)out_signed->genesis_hash[2] << 8) |
                              ((uint32_t)out_signed->genesis_hash[3]);
    return ESP_OK;
}

esp_err_t meshpay_currency_descriptor_founder_hash(const meshpay_currency_descriptor_signed_t *signed_desc,
                                                   uint8_t out_hash[RNS_IDENTITY_HASH_SIZE])
{
    if (signed_desc == NULL || out_hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* L'autorité MINT / le destinataire des frais est le COMPTE WALLET du
     * fondateur = son hash de destination meshpay.wallet (== son local_destination
     * runtime), PAS son hash d'identité. Sinon les frais de transfert et une
     * frappe fondateur future atterrissent sur un compte que le wallet n'interroge
     * jamais et depuis lequel il ne peut pas dépenser (constat HIGH revue Palier D).
     * Le hash de destination reste dérivable de founder_public SEUL (nom bien connu
     * meshpay.wallet) : la propriété « autorité vérifiable à distance » est
     * préservée, et le currency_id (SHA-256 du corps) est inchangé. */
    rns_identity_t founder;
    ESP_RETURN_ON_ERROR(rns_identity_load_public(&founder,
                                                 signed_desc->body.founder_public),
                        TAG, "");
    rns_destination_t wallet;
    ESP_RETURN_ON_ERROR(rns_destination_create_meshpay_wallet(&founder, &wallet),
                        TAG, "");
    memcpy(out_hash, wallet.hash, RNS_DESTINATION_HASH_SIZE);
    return ESP_OK;
}

/* ======================================================================== */
/* Palier B1 — code d'invitation (ancre base32 Crockford + checksum)        */
/* ======================================================================== */

/*
 * Alphabet base32 Crockford : 32 symboles, sans I, L, O, U. On garde les
 * chiffres 0 et 1, mais leurs sosies visuels (O, et I/L) sont absents → aucune
 * paire ambiguë à l'écran. (Exclure réellement 0 et 1 EN PLUS aurait laissé 31
 * symboles, insuffisant pour un base32 — choix verrouillé en session.)
 */
static const char INVITE_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Longueur de la charge utile binaire avant base32 : ancre + 1 octet checksum. */
#define INVITE_PAYLOAD_LEN (MESHPAY_CURRENCY_INVITE_ANCHOR_LEN + 1)
/* Toutes les 4 symboles, on insère un tiret de lisibilité (groupes 4-4-4-4-2). */
#define INVITE_GROUP_SIZE 4

/*
 * Octet de checksum = 1er octet de SHA-256(ancre). But : détecter une faute de
 * frappe (probabilité de non-détection ~1/256), pas une attaque — l'intégrité
 * cryptographique reste portée par la signature du fondateur (verify).
 */
static esp_err_t invite_checksum(const uint8_t *anchor, size_t anchor_len, uint8_t *out)
{
    uint8_t digest[RNS_CRYPTO_SHA256_SIZE];
    ESP_RETURN_ON_ERROR(rns_crypto_sha256(anchor, anchor_len, digest), TAG, "");
    *out = digest[0];
    return ESP_OK;
}

/*
 * Convertit un caractère saisi en valeur 0..31, ou -1 si hors alphabet. Applique
 * la normalisation Crockford : passage en majuscule, puis O→0, I/L→1, U→V. Le
 * '\0' est rejeté explicitement (sinon strchr le trouverait en fin de chaîne).
 */
static int invite_symbol_value(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    switch (c) {
    case 'O': c = '0'; break;
    case 'I':
    case 'L': c = '1'; break;
    case 'U': c = 'V'; break;
    default: break;
    }
    if (c == '\0') {
        return -1;
    }
    const char *p = strchr(INVITE_ALPHABET, c);
    if (p == NULL) {
        return -1;
    }
    return (int)(p - INVITE_ALPHABET);
}

esp_err_t meshpay_currency_invite_encode(const meshpay_currency_descriptor_signed_t *signed_desc,
                                         char *out,
                                         size_t out_size)
{
    if (signed_desc == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size < MESHPAY_CURRENCY_INVITE_CODE_BUF) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Charge utile = ancre (préfixe genèse) ‖ checksum. */
    uint8_t payload[INVITE_PAYLOAD_LEN];
    memcpy(payload, signed_desc->genesis_hash, MESHPAY_CURRENCY_INVITE_ANCHOR_LEN);
    ESP_RETURN_ON_ERROR(invite_checksum(payload, MESHPAY_CURRENCY_INVITE_ANCHOR_LEN,
                                        &payload[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN]),
                        TAG, "");

    /*
     * Encodage base32 « big-endian » : on consomme les bits du MSB du 1er octet
     * vers le LSB du dernier, par tranches de 5. 88 bits → 17 tranches pleines +
     * 3 bits restants bourrés à droite de 2 zéros → 18 symboles. Les tirets sont
     * insérés tous les INVITE_GROUP_SIZE symboles.
     */
    uint32_t buffer = 0;
    int bits = 0;
    size_t sym_count = 0;
    size_t pos = 0;
    for (size_t i = 0; i < INVITE_PAYLOAD_LEN; ++i) {
        buffer = (buffer << 8) | payload[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            uint8_t sym = (uint8_t)((buffer >> bits) & 0x1FU);
            if (sym_count > 0 && (sym_count % INVITE_GROUP_SIZE) == 0) {
                out[pos++] = '-';
            }
            out[pos++] = INVITE_ALPHABET[sym];
            ++sym_count;
        }
    }
    if (bits > 0) {
        /* Bits restants bourrés à droite avec des zéros (bourrage déterministe). */
        uint8_t sym = (uint8_t)((buffer << (5 - bits)) & 0x1FU);
        if (sym_count > 0 && (sym_count % INVITE_GROUP_SIZE) == 0) {
            out[pos++] = '-';
        }
        out[pos++] = INVITE_ALPHABET[sym];
        ++sym_count;
    }
    out[pos] = '\0';
    return ESP_OK;
}

esp_err_t meshpay_currency_invite_decode(const char *code,
                                         uint8_t *anchor_out,
                                         size_t anchor_cap,
                                         size_t *anchor_len)
{
    if (code == NULL || anchor_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Capacité vérifiée AVANT de parser : un appelant sous-dimensionné est une
     * erreur de programmation distincte d'un code mal formé. */
    if (anchor_cap < MESHPAY_CURRENCY_INVITE_ANCHOR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * 1) Décodage base32 : on ignore tirets et espaces, on normalise chaque
     *    symbole, on accumule les bits par tranches de 8 → INVITE_PAYLOAD_LEN
     *    octets. On compte les symboles pour valider la longueur exacte.
     */
    uint8_t payload[INVITE_PAYLOAD_LEN];
    uint32_t buffer = 0;
    int bits = 0;
    size_t byte_count = 0;
    size_t sym_count = 0;
    for (const char *p = code; *p != '\0'; ++p) {
        if (*p == '-' || *p == ' ' || *p == '\t') {
            continue; /* séparateurs de lisibilité ignorés */
        }
        int val = invite_symbol_value(*p);
        if (val < 0) {
            return ESP_ERR_INVALID_ARG; /* caractère hors alphabet */
        }
        ++sym_count;
        if (sym_count > MESHPAY_CURRENCY_INVITE_CODE_SYMBOLS) {
            return ESP_ERR_INVALID_SIZE; /* trop de symboles */
        }
        buffer = (buffer << 5) | (uint32_t)val;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (byte_count >= INVITE_PAYLOAD_LEN) {
                return ESP_ERR_INVALID_SIZE; /* garde-fou (ne devrait pas arriver) */
            }
            payload[byte_count++] = (uint8_t)((buffer >> bits) & 0xFFU);
        }
    }

    /* Nombre exact de symboles requis (sinon code tronqué/rallongé). */
    if (sym_count != MESHPAY_CURRENCY_INVITE_CODE_SYMBOLS ||
        byte_count != INVITE_PAYLOAD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Les bits de bourrage de fin (ici 2) doivent être nuls : sinon le code ne
     * provient pas d'un encodeur conforme. */
    if (bits > 0 && (buffer & ((1U << bits) - 1U)) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 2) Vérification du checksum sur l'ancre décodée. */
    uint8_t expected = 0;
    ESP_RETURN_ON_ERROR(invite_checksum(payload, MESHPAY_CURRENCY_INVITE_ANCHOR_LEN,
                                        &expected),
                        TAG, "");
    if (payload[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN] != expected) {
        return ESP_ERR_INVALID_CRC; /* faute de frappe détectée */
    }

    memcpy(anchor_out, payload, MESHPAY_CURRENCY_INVITE_ANCHOR_LEN);
    if (anchor_len != NULL) {
        *anchor_len = MESHPAY_CURRENCY_INVITE_ANCHOR_LEN;
    }
    return ESP_OK;
}

esp_err_t meshpay_currency_descriptor_matches_anchor(const meshpay_currency_descriptor_signed_t *signed_desc,
                                                     const uint8_t *anchor,
                                                     size_t anchor_len)
{
    if (signed_desc == NULL || anchor == NULL ||
        anchor_len == 0 || anchor_len > MESHPAY_CURRENCY_GENESIS_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Recalcule la genèse depuis le CORPS (pas de confiance au champ stocké) :
     * un descripteur dont le corps a été trafiqué produit une genèse différente
     * et ne peut donc pas usurper l'ancre d'une autre monnaie. */
    uint8_t computed_genesis[MESHPAY_CURRENCY_GENESIS_SIZE];
    ESP_RETURN_ON_ERROR(meshpay_currency_descriptor_compute_genesis(&signed_desc->body,
                                                                    computed_genesis,
                                                                    NULL),
                        TAG, "");

    /* Comparaison temps constant du préfixe : évite une fuite par timing sur le
     * nombre d'octets d'ancre concordants. */
    if (!rns_crypto_constant_equal(computed_genesis, anchor, anchor_len)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
