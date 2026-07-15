#include "meshpay/meshpay_tx.h"

#include "esp_check.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "meshpay_tx";

#define CBOR_KEY_TYPE 1
#define CBOR_KEY_FROM 2
#define CBOR_KEY_TO 3
#define CBOR_KEY_AMOUNT 4
#define CBOR_KEY_SEQ 5
#define CBOR_KEY_FEE 6
#define CBOR_KEY_PARENTS 7
#define CBOR_KEY_TIMESTAMP 8
#define CBOR_KEY_CURRENCY 9
#define CBOR_KEY_ID 10
#define CBOR_KEY_SIGNATURE 11
/* Wire v2 (durcissement ingestion) : clé publique du membre, CLAIM seulement. */
#define CBOR_KEY_MEMBER_PUBLIC 12

#define SIGNABLE_FIELD_COUNT 9
#define FULL_FIELD_COUNT 11

/* La map CBOR a un count FIXE par type : les CLAIM portent une clé de plus
 * (member_public). L'encodage reste canonique (un seul wire possible par tx). */
static size_t signable_field_count(const meshpay_tx_t *tx)
{
    return SIGNABLE_FIELD_COUNT +
           (tx->type == MESHPAY_TX_TYPE_CLAIM ? 1U : 0U);
}

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
    if (len > writer->size || writer->pos > writer->size - len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(writer->buf + writer->pos, data, len);
    writer->pos += len;
    return ESP_OK;
}

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

static esp_err_t cbor_write_array(cbor_writer_t *writer, size_t count)
{
    if (count > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return cbor_write_type_value(writer, 4, count);
}

static esp_err_t cbor_write_map(cbor_writer_t *writer, size_t count)
{
    if (count > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return cbor_write_type_value(writer, 5, count);
}

static esp_err_t encode_signable_fields(cbor_writer_t *writer,
                                        const meshpay_tx_t *tx)
{
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_TYPE), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, (uint64_t)tx->type), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_FROM), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, tx->from, sizeof(tx->from)),
                        TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_TO), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, tx->to, sizeof(tx->to)),
                        TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_AMOUNT), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, tx->amount), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_SEQ), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, tx->seq), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_FEE), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, tx->fee), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_PARENTS), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_array(writer, tx->parent_count), TAG, "");
    for (uint8_t i = 0; i < tx->parent_count; ++i) {
        ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, tx->parents[i],
                                            MESHPAY_TX_PARENT_ID_SIZE),
                            TAG, "");
    }

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_TIMESTAMP), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, tx->timestamp_ms), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_CURRENCY), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_uint(writer, tx->currency_id), TAG, "");

    /* CLAIM uniquement : la clé du membre fait partie du contenu SIGNÉ (aucune
     * malléabilité possible, même théorique). Absente du wire pour les autres
     * types (validate_common exige alors un champ nul). */
    if (tx->type == MESHPAY_TX_TYPE_CLAIM) {
        ESP_RETURN_ON_ERROR(cbor_write_uint(writer, CBOR_KEY_MEMBER_PUBLIC),
                            TAG, "");
        ESP_RETURN_ON_ERROR(cbor_write_bstr(writer, tx->member_public,
                                            sizeof(tx->member_public)),
                            TAG, "");
    }
    return ESP_OK;
}

static bool hash_is_zero(const uint8_t *hash, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= hash[i];
    }
    return acc == 0;
}

static esp_err_t validate_common(const meshpay_tx_t *tx, bool require_signature)
{
    if (tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx->type != MESHPAY_TX_TYPE_TRANSFER && tx->type != MESHPAY_TX_TYPE_MINT &&
        tx->type != MESHPAY_TX_TYPE_CLAIM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hash_is_zero(tx->from, sizeof(tx->from)) ||
        hash_is_zero(tx->to, sizeof(tx->to))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx->amount == 0 || tx->parent_count > MESHPAY_TX_MAX_PARENTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx->type == MESHPAY_TX_TYPE_TRANSFER && tx->fee >= tx->amount) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx->type == MESHPAY_TX_TYPE_MINT && tx->fee != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx->type == MESHPAY_TX_TYPE_CLAIM) {
        /* CLAIM réflexive : from == to == membre, pas de frais. */
        if (tx->fee != 0 ||
            memcmp(tx->from, tx->to, sizeof(tx->from)) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        /* seq == 0 est RÉSERVÉ à la CLAIM : c'est le pivot de l'unicité
         * (from, seq==0) dans le DAG. Un seq != 0 permettrait un double crédit
         * (deux CLAIM non conflictuelles) — refus catégorique, y compris au
         * décodage d'un paquet reçu. */
        if (tx->seq != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        /* Wire v2 : la CLAIM publie la clé du membre — champ obligatoire. */
        if (hash_is_zero(tx->member_public, sizeof(tx->member_public))) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (!hash_is_zero(tx->member_public, sizeof(tx->member_public))) {
        /* Forme stricte : hors CLAIM, aucune clé embarquée (pas de canal
         * caché, encodage canonique par type). */
        return ESP_ERR_INVALID_ARG;
    }
    if (require_signature &&
        (hash_is_zero(tx->id, sizeof(tx->id)) ||
         hash_is_zero(tx->signature, sizeof(tx->signature)))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void meshpay_tx_clear(meshpay_tx_t *tx)
{
    if (tx != NULL) {
        memset(tx, 0, sizeof(*tx));
    }
}

esp_err_t meshpay_tx_encode_signable(const meshpay_tx_t *tx,
                                     uint8_t *out,
                                     size_t out_size,
                                     size_t *out_len)
{
    if (tx == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, false), TAG, "");

    cbor_writer_t writer = {
        .buf = out,
        .size = out_size,
        .pos = 0,
    };
    ESP_RETURN_ON_ERROR(cbor_write_map(&writer, signable_field_count(tx)),
                        TAG, "");
    ESP_RETURN_ON_ERROR(encode_signable_fields(&writer, tx), TAG, "");
    *out_len = writer.pos;
    return ESP_OK;
}

esp_err_t meshpay_tx_encode(const meshpay_tx_t *tx,
                            uint8_t *out,
                            size_t out_size,
                            size_t *out_len)
{
    if (tx == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, true), TAG, "");

    cbor_writer_t writer = {
        .buf = out,
        .size = out_size,
        .pos = 0,
    };
    ESP_RETURN_ON_ERROR(cbor_write_map(&writer, signable_field_count(tx) + 2U),
                        TAG, "");
    ESP_RETURN_ON_ERROR(encode_signable_fields(&writer, tx), TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(&writer, CBOR_KEY_ID), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(&writer, tx->id, sizeof(tx->id)),
                        TAG, "");

    ESP_RETURN_ON_ERROR(cbor_write_uint(&writer, CBOR_KEY_SIGNATURE), TAG, "");
    ESP_RETURN_ON_ERROR(cbor_write_bstr(&writer, tx->signature,
                                        sizeof(tx->signature)),
                        TAG, "");

    if (writer.pos > MESHPAY_TX_CBOR_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = writer.pos;
    return ESP_OK;
}

esp_err_t meshpay_tx_compute_id(const meshpay_tx_t *tx,
                                uint8_t out_id[MESHPAY_TX_ID_SIZE])
{
    if (tx == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t signable[MESHPAY_TX_CBOR_MAX_SIZE];
    size_t signable_len = 0;
    ESP_RETURN_ON_ERROR(meshpay_tx_encode_signable(tx, signable,
                                                   sizeof(signable),
                                                   &signable_len),
                        TAG, "");
    return rns_crypto_sha256(signable, signable_len, out_id);
}

esp_err_t meshpay_tx_sign(meshpay_tx_t *tx, const rns_identity_t *signer)
{
    if (tx == NULL || signer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(meshpay_tx_compute_id(tx, tx->id), TAG, "");
    return rns_identity_sign(signer, tx->id, sizeof(tx->id), tx->signature);
}

esp_err_t meshpay_tx_create_transfer(meshpay_tx_t *tx,
                                     const rns_identity_t *signer,
                                     const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                     const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                     uint32_t amount,
                                     uint32_t seq,
                                     uint32_t fee,
                                     uint32_t currency_id,
                                     const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                                     uint8_t parent_count,
                                     uint64_t timestamp_ms)
{
    if (tx == NULL || signer == NULL || from == NULL || to == NULL ||
        (parent_count > 0 && parents == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_TRANSFER;
    memcpy(tx->from, from, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = fee;
    tx->currency_id = currency_id;
    tx->timestamp_ms = timestamp_ms;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count && i < MESHPAY_TX_MAX_PARENTS; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, false), TAG, "");
    return meshpay_tx_sign(tx, signer);
}

esp_err_t meshpay_tx_create_mint(meshpay_tx_t *tx,
                                 const rns_identity_t *signer,
                                 const uint8_t from[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 const uint8_t to[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                 uint32_t amount,
                                 uint32_t seq,
                                 uint32_t currency_id,
                                 const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                                 uint8_t parent_count,
                                 uint64_t timestamp_ms)
{
    if (tx == NULL || signer == NULL || from == NULL || to == NULL ||
        (parent_count > 0 && parents == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_MINT;
    memcpy(tx->from, from, sizeof(tx->from));
    memcpy(tx->to, to, sizeof(tx->to));
    tx->amount = amount;
    tx->seq = seq;
    tx->fee = 0;
    tx->currency_id = currency_id;
    tx->timestamp_ms = timestamp_ms;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count && i < MESHPAY_TX_MAX_PARENTS; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, false), TAG, "");
    return meshpay_tx_sign(tx, signer);
}

esp_err_t meshpay_tx_create_claim(meshpay_tx_t *tx,
                                  const rns_identity_t *signer,
                                  const uint8_t member[MESHPAY_TX_DESTINATION_HASH_SIZE],
                                  uint32_t amount,
                                  uint32_t currency_id,
                                  const uint8_t parents[][MESHPAY_TX_PARENT_ID_SIZE],
                                  uint8_t parent_count,
                                  uint64_t timestamp_ms)
{
    if (tx == NULL || signer == NULL || member == NULL ||
        (parent_count > 0 && parents == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    meshpay_tx_clear(tx);
    tx->type = MESHPAY_TX_TYPE_CLAIM;
    /* Réflexivité imposée : from == to == membre. */
    memcpy(tx->from, member, sizeof(tx->from));
    memcpy(tx->to, member, sizeof(tx->to));
    /* Wire v2 : publie la clé du signataire — l'acte de rejointe est aussi
     * l'enregistrement de la clé dans l'annuaire que constitue la DAG. La
     * cohérence clé↔compte (wallet-hash == member) est vérifiée à l'ingestion
     * par la couche currency ; ici on garantit seulement clé == signataire. */
    ESP_RETURN_ON_ERROR(rns_identity_get_public_key(signer, tx->member_public),
                        TAG, "");
    tx->amount = amount;
    tx->seq = 0; /* réservé : pivot de l'unicité (from, 0) */
    tx->fee = 0; /* aucun frais sur un crédit */
    tx->currency_id = currency_id;
    tx->timestamp_ms = timestamp_ms;
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count && i < MESHPAY_TX_MAX_PARENTS; ++i) {
        memcpy(tx->parents[i], parents[i], MESHPAY_TX_PARENT_ID_SIZE);
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, false), TAG, "");
    return meshpay_tx_sign(tx, signer);
}

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

static esp_err_t cbor_read_bstr(cbor_reader_t *reader,
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

static esp_err_t cbor_read_array_len(cbor_reader_t *reader, uint64_t *out)
{
    return cbor_read_type_value(reader, 4, out);
}

static esp_err_t cbor_read_map_len(cbor_reader_t *reader, uint64_t *out)
{
    return cbor_read_type_value(reader, 5, out);
}

esp_err_t meshpay_tx_decode(const uint8_t *data,
                            size_t data_len,
                            meshpay_tx_t *tx)
{
    if (data == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cbor_reader_t reader = {
        .buf = data,
        .len = data_len,
        .pos = 0,
    };
    meshpay_tx_clear(tx);

    uint64_t map_len = 0;
    ESP_RETURN_ON_ERROR(cbor_read_map_len(&reader, &map_len), TAG, "");
    /* 11 champs, +1 (member_public) pour une CLAIM. La cohérence type↔champ
     * est tranchée par validate_common en sortie (l'ordre des clés d'une map
     * hostile ne permet pas de la vérifier au fil de l'eau). */
    if (map_len != FULL_FIELD_COUNT && map_len != FULL_FIELD_COUNT + 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t present = 0;
    for (uint64_t i = 0; i < map_len; ++i) {
        uint64_t key = 0;
        ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &key), TAG, "");
        if (key > 31U) {
            return ESP_ERR_INVALID_ARG;
        }
        /* Clé dupliquée = wire non canonique (et, à 12 entrées, un moyen de
         * maquiller l'absence de member_public) : refus. */
        if (present & (uint32_t)(1UL << key)) {
            return ESP_ERR_INVALID_ARG;
        }
        present |= (uint32_t)(1UL << key);

        uint64_t value = 0;
        switch (key) {
        case CBOR_KEY_TYPE:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value != MESHPAY_TX_TYPE_TRANSFER && value != MESHPAY_TX_TYPE_MINT &&
                value != MESHPAY_TX_TYPE_CLAIM) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->type = (meshpay_tx_type_t)value;
            break;
        case CBOR_KEY_FROM:
            ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->from, sizeof(tx->from)),
                                TAG, "");
            break;
        case CBOR_KEY_TO:
            ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->to, sizeof(tx->to)),
                                TAG, "");
            break;
        case CBOR_KEY_AMOUNT:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->amount = (uint32_t)value;
            break;
        case CBOR_KEY_SEQ:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->seq = (uint32_t)value;
            break;
        case CBOR_KEY_FEE:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->fee = (uint32_t)value;
            break;
        case CBOR_KEY_PARENTS: {
            uint64_t parent_count = 0;
            ESP_RETURN_ON_ERROR(cbor_read_array_len(&reader, &parent_count),
                                TAG, "");
            if (parent_count > MESHPAY_TX_MAX_PARENTS) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->parent_count = (uint8_t)parent_count;
            for (uint8_t parent = 0; parent < tx->parent_count; ++parent) {
                ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->parents[parent],
                                                   MESHPAY_TX_PARENT_ID_SIZE),
                                    TAG, "");
            }
            break;
        }
        case CBOR_KEY_TIMESTAMP:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &tx->timestamp_ms),
                                TAG, "");
            break;
        case CBOR_KEY_CURRENCY:
            ESP_RETURN_ON_ERROR(cbor_read_uint(&reader, &value), TAG, "");
            if (value > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->currency_id = (uint32_t)value;
            break;
        case CBOR_KEY_ID:
            ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->id, sizeof(tx->id)),
                                TAG, "");
            break;
        case CBOR_KEY_SIGNATURE:
            ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->signature,
                                               sizeof(tx->signature)),
                                TAG, "");
            break;
        case CBOR_KEY_MEMBER_PUBLIC:
            ESP_RETURN_ON_ERROR(cbor_read_bstr(&reader, tx->member_public,
                                               sizeof(tx->member_public)),
                                TAG, "");
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint32_t required = 0;
    for (uint8_t key = CBOR_KEY_TYPE; key <= CBOR_KEY_SIGNATURE; ++key) {
        required |= (uint32_t)(1UL << key);
    }
    if ((present & required) != required || reader.pos != data_len) {
        return ESP_ERR_INVALID_ARG;
    }

    return validate_common(tx, true);
}

esp_err_t meshpay_tx_verify(const meshpay_tx_t *tx,
                            const rns_identity_t *from_identity)
{
    if (tx == NULL || from_identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(validate_common(tx, true), TAG, "");

    uint8_t computed_id[MESHPAY_TX_ID_SIZE];
    ESP_RETURN_ON_ERROR(meshpay_tx_compute_id(tx, computed_id), TAG, "");
    if (!rns_crypto_constant_equal(computed_id, tx->id, sizeof(computed_id))) {
        return ESP_ERR_INVALID_STATE;
    }
    return rns_identity_verify(from_identity, tx->id, sizeof(tx->id), tx->signature);
}
