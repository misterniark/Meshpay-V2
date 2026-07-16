#include "meshpay/rns/rns_announce.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

/* Annuaire des destinations connues. ÉCRIT par la tâche radio (poll →
 * verify_and_remember), LU par les tâches UI/core : tout accès passe sous
 * s_known_lock, et les lectures sortent des COPIES (jamais de pointeur vers la
 * table vivante). Un portMUX (initialisation statique, pas de fonction d'init
 * à séquencer) convient : les sections sont courtes — scan de 16 entrées et
 * memcpy ≤ ~450 octets, quelques µs — et le SHA-256 du paquet est calculé
 * HORS section critique. */
static rns_announce_known_destination_t s_known[RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX];
static portMUX_TYPE s_known_lock = portMUX_INITIALIZER_UNLOCKED;

/* Recherche du slot d'une destination. À appeler UNIQUEMENT sous
 * s_known_lock ; le pointeur renvoyé ne doit jamais sortir de la section
 * critique de l'appelant. */
static rns_announce_known_destination_t *find_known_locked(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE])
{
    for (size_t i = 0; i < RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX; ++i) {
        if (s_known[i].in_use &&
            rns_destination_hash_equal(s_known[i].destination_hash,
                                       destination_hash)) {
            return &s_known[i];
        }
    }
    return NULL;
}

/* Remplit une vue légère depuis un slot occupé (troncature d'app_data à la
 * capacité de la vue). À appeler UNIQUEMENT sous s_known_lock. */
static void fill_peer_info_locked(const rns_announce_known_destination_t *entry,
                                  rns_announce_peer_info_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->destination_hash, entry->destination_hash,
           RNS_DESTINATION_HASH_SIZE);
    memcpy(out->public_key, entry->public_key, RNS_ANNOUNCE_PUBLIC_KEY_SIZE);
    size_t len = entry->app_data_len;
    if (len > RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX) {
        len = RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX;
    }
    memcpy(out->app_data, entry->app_data, len);
    out->app_data_len = len;
}

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static esp_err_t compute_destination_hash(const uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE],
                                          const uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE],
                                          uint8_t out[RNS_DESTINATION_HASH_SIZE])
{
    rns_identity_t identity;
    esp_err_t err = rns_identity_load_public(&identity, public_key);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t identity_hash[RNS_IDENTITY_HASH_SIZE];
    err = rns_identity_get_hash(&identity, identity_hash);
    if (err != ESP_OK) {
        rns_identity_clear(&identity);
        return err;
    }

    uint8_t material[RNS_DESTINATION_NAME_HASH_SIZE + RNS_IDENTITY_HASH_SIZE];
    memcpy(material, name_hash, RNS_DESTINATION_NAME_HASH_SIZE);
    memcpy(material + RNS_DESTINATION_NAME_HASH_SIZE,
           identity_hash,
           RNS_IDENTITY_HASH_SIZE);

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    err = rns_crypto_sha256(material, sizeof(material), full_hash);
    if (err == ESP_OK) {
        memcpy(out, full_hash, RNS_DESTINATION_HASH_SIZE);
    }

    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    rns_crypto_secure_zero(material, sizeof(material));
    rns_crypto_secure_zero(identity_hash, sizeof(identity_hash));
    rns_identity_clear(&identity);
    return err;
}

static esp_err_t build_signed_data(const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
                                   const uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE],
                                   const uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE],
                                   const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE],
                                   const uint8_t *app_data,
                                   size_t app_data_len,
                                   uint8_t *out,
                                   size_t out_len,
                                   size_t *written)
{
    if (out == NULL || written == NULL ||
        destination_hash == NULL || public_key == NULL ||
        name_hash == NULL || random_hash == NULL ||
        (app_data == NULL && app_data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t needed = RNS_DESTINATION_HASH_SIZE +
                    RNS_ANNOUNCE_PUBLIC_KEY_SIZE +
                    RNS_DESTINATION_NAME_HASH_SIZE +
                    RNS_ANNOUNCE_RANDOM_HASH_SIZE +
                    app_data_len;
    if (out_len < needed) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 0;
    memcpy(out + pos, destination_hash, RNS_DESTINATION_HASH_SIZE);
    pos += RNS_DESTINATION_HASH_SIZE;
    memcpy(out + pos, public_key, RNS_ANNOUNCE_PUBLIC_KEY_SIZE);
    pos += RNS_ANNOUNCE_PUBLIC_KEY_SIZE;
    memcpy(out + pos, name_hash, RNS_DESTINATION_NAME_HASH_SIZE);
    pos += RNS_DESTINATION_NAME_HASH_SIZE;
    memcpy(out + pos, random_hash, RNS_ANNOUNCE_RANDOM_HASH_SIZE);
    pos += RNS_ANNOUNCE_RANDOM_HASH_SIZE;
    if (app_data_len > 0) {
        memcpy(out + pos, app_data, app_data_len);
        pos += app_data_len;
    }

    *written = pos;
    return ESP_OK;
}

esp_err_t rns_announce_encode(const rns_destination_t *destination,
                              const rns_identity_t *identity,
                              const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE],
                              const uint8_t *app_data,
                              size_t app_data_len,
                              uint8_t *out,
                              size_t out_len,
                              size_t *written)
{
    if (destination == NULL || identity == NULL || random_hash == NULL || out == NULL ||
        (app_data == NULL && app_data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(random_hash, RNS_ANNOUNCE_RANDOM_HASH_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (destination->type != RNS_DESTINATION_TYPE_SINGLE ||
        !identity->has_private ||
        app_data_len > RNS_ANNOUNCE_MAX_APP_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t needed = RNS_ANNOUNCE_BASE_SIZE + app_data_len;
    if (out_len < needed || needed > RNS_PACKET_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    esp_err_t err = rns_identity_get_public_key(identity, public_key);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t signed_data[RNS_DESTINATION_HASH_SIZE +
                        RNS_ANNOUNCE_PUBLIC_KEY_SIZE +
                        RNS_DESTINATION_NAME_HASH_SIZE +
                        RNS_ANNOUNCE_RANDOM_HASH_SIZE +
                        RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t signed_len = 0;
    err = build_signed_data(destination->hash,
                            public_key,
                            destination->name_hash,
                            random_hash,
                            app_data,
                            app_data_len,
                            signed_data,
                            sizeof(signed_data),
                            &signed_len);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(public_key, sizeof(public_key));
        rns_crypto_secure_zero(signed_data, sizeof(signed_data));
        return err;
    }

    size_t pos = 0;
    memcpy(out + pos, public_key, RNS_ANNOUNCE_PUBLIC_KEY_SIZE);
    pos += RNS_ANNOUNCE_PUBLIC_KEY_SIZE;
    memcpy(out + pos, destination->name_hash, RNS_DESTINATION_NAME_HASH_SIZE);
    pos += RNS_DESTINATION_NAME_HASH_SIZE;
    memcpy(out + pos, random_hash, RNS_ANNOUNCE_RANDOM_HASH_SIZE);
    pos += RNS_ANNOUNCE_RANDOM_HASH_SIZE;

    err = rns_identity_sign(identity, signed_data, signed_len, out + pos);
    if (err == ESP_OK) {
        pos += RNS_ANNOUNCE_SIGNATURE_SIZE;
        if (app_data_len > 0) {
            memcpy(out + pos, app_data, app_data_len);
            pos += app_data_len;
        }
        if (written != NULL) {
            *written = pos;
        }
    }

    rns_crypto_secure_zero(public_key, sizeof(public_key));
    rns_crypto_secure_zero(signed_data, sizeof(signed_data));
    return err;
}

esp_err_t rns_announce_decode(const rns_packet_t *packet,
                              rns_announce_t *out)
{
    if (packet == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->packet_type != RNS_PACKET_TYPE_ANNOUNCE ||
        packet->destination_type != RNS_DESTINATION_TYPE_SINGLE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->context_flag) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (packet->data_len < RNS_ANNOUNCE_BASE_SIZE ||
        packet->data_len > RNS_PACKET_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->destination_hash, packet->destination_hash, RNS_DESTINATION_HASH_SIZE);

    size_t pos = 0;
    memcpy(out->public_key, packet->data + pos, RNS_ANNOUNCE_PUBLIC_KEY_SIZE);
    pos += RNS_ANNOUNCE_PUBLIC_KEY_SIZE;
    memcpy(out->name_hash, packet->data + pos, RNS_DESTINATION_NAME_HASH_SIZE);
    pos += RNS_DESTINATION_NAME_HASH_SIZE;
    memcpy(out->random_hash, packet->data + pos, RNS_ANNOUNCE_RANDOM_HASH_SIZE);
    pos += RNS_ANNOUNCE_RANDOM_HASH_SIZE;
    memcpy(out->signature, packet->data + pos, RNS_ANNOUNCE_SIGNATURE_SIZE);
    pos += RNS_ANNOUNCE_SIGNATURE_SIZE;

    out->app_data_len = packet->data_len - pos;
    if (out->app_data_len > RNS_ANNOUNCE_MAX_APP_DATA_SIZE) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->app_data_len > 0) {
        memcpy(out->app_data, packet->data + pos, out->app_data_len);
    }

    return ESP_OK;
}

esp_err_t rns_announce_verify(const rns_packet_t *packet,
                              rns_announce_t *out)
{
    rns_announce_t decoded;
    esp_err_t err = rns_announce_decode(packet, &decoded);
    if (err != ESP_OK) {
        return err;
    }
    if (bytes_zero(decoded.random_hash, sizeof(decoded.random_hash))) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t expected_hash[RNS_DESTINATION_HASH_SIZE];
    err = compute_destination_hash(decoded.name_hash,
                                   decoded.public_key,
                                   expected_hash);
    if (err != ESP_OK) {
        return err;
    }
    if (!rns_destination_hash_equal(packet->destination_hash, expected_hash)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t signed_data[RNS_DESTINATION_HASH_SIZE +
                        RNS_ANNOUNCE_PUBLIC_KEY_SIZE +
                        RNS_DESTINATION_NAME_HASH_SIZE +
                        RNS_ANNOUNCE_RANDOM_HASH_SIZE +
                        RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t signed_len = 0;
    err = build_signed_data(packet->destination_hash,
                            decoded.public_key,
                            decoded.name_hash,
                            decoded.random_hash,
                            decoded.app_data,
                            decoded.app_data_len,
                            signed_data,
                            sizeof(signed_data),
                            &signed_len);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(signed_data, sizeof(signed_data));
        return err;
    }

    rns_identity_t announced_identity;
    err = rns_identity_load_public(&announced_identity, decoded.public_key);
    if (err == ESP_OK) {
        err = rns_identity_verify(&announced_identity,
                                  signed_data,
                                  signed_len,
                                  decoded.signature);
    }

    rns_identity_clear(&announced_identity);
    rns_crypto_secure_zero(signed_data, sizeof(signed_data));
    if (err != ESP_OK) {
        return err;
    }

    if (out != NULL) {
        memcpy(out, &decoded, sizeof(decoded));
    }
    return ESP_OK;
}

esp_err_t rns_announce_verify_and_remember(const rns_packet_t *packet,
                                           rns_announce_t *out)
{
    rns_announce_t decoded;
    esp_err_t err = rns_announce_verify(packet, &decoded);
    if (err != ESP_OK) {
        return err;
    }

    /* Le hash du paquet (SHA-256) est calculé AVANT la section critique :
     * jamais de crypto sous verrou. En cas d'échec, la table n'est pas
     * touchée — plus besoin du rollback historique. */
    uint8_t packet_hash[RNS_CRYPTO_SHA256_SIZE];
    err = rns_packet_hash(packet, packet_hash);
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&s_known_lock);
    size_t slot = RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX;
    for (size_t i = 0; i < RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX; ++i) {
        if (s_known[i].in_use &&
            rns_destination_hash_equal(s_known[i].destination_hash, decoded.destination_hash)) {
            if (!rns_crypto_constant_equal(s_known[i].public_key,
                                           decoded.public_key,
                                           RNS_ANNOUNCE_PUBLIC_KEY_SIZE)) {
                /* Collision destination/clé : on refuse SANS modifier le slot
                 * existant (usurpation d'identité annoncée). */
                taskEXIT_CRITICAL(&s_known_lock);
                return ESP_ERR_INVALID_STATE;
            }
            slot = i;
            break;
        }
        if (!s_known[i].in_use && slot == RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX) {
            slot = i;
        }
    }
    if (slot == RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX) {
        taskEXIT_CRITICAL(&s_known_lock);
        return ESP_ERR_NO_MEM;
    }

    rns_announce_known_destination_t *entry = &s_known[slot];
    memset(entry, 0, sizeof(*entry));
    entry->in_use = true;
    memcpy(entry->destination_hash, decoded.destination_hash, RNS_DESTINATION_HASH_SIZE);
    memcpy(entry->public_key, decoded.public_key, RNS_ANNOUNCE_PUBLIC_KEY_SIZE);
    memcpy(entry->app_data, decoded.app_data, decoded.app_data_len);
    entry->app_data_len = decoded.app_data_len;
    memcpy(entry->packet_hash, packet_hash, sizeof(packet_hash));
    taskEXIT_CRITICAL(&s_known_lock);

    if (out != NULL) {
        memcpy(out, &decoded, sizeof(decoded));
    }
    return ESP_OK;
}

void rns_announce_known_reset(void)
{
    taskENTER_CRITICAL(&s_known_lock);
    rns_crypto_secure_zero(s_known, sizeof(s_known));
    taskEXIT_CRITICAL(&s_known_lock);
}

size_t rns_announce_known_count(void)
{
    size_t count = 0;
    taskENTER_CRITICAL(&s_known_lock);
    for (size_t i = 0; i < RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX; ++i) {
        if (s_known[i].in_use) {
            count++;
        }
    }
    taskEXIT_CRITICAL(&s_known_lock);
    return count;
}

esp_err_t rns_announce_recall_copy(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
    rns_announce_known_destination_t *out)
{
    if (destination_hash == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_known_lock);
    const rns_announce_known_destination_t *entry =
        find_known_locked(destination_hash);
    if (entry != NULL) {
        memcpy(out, entry, sizeof(*out));
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_known_lock);
    return err;
}

esp_err_t rns_announce_recall_info(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
    rns_announce_peer_info_t *out)
{
    if (destination_hash == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_known_lock);
    const rns_announce_known_destination_t *entry =
        find_known_locked(destination_hash);
    if (entry != NULL) {
        fill_peer_info_locked(entry, out);
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_known_lock);
    return err;
}

esp_err_t rns_announce_known_info(size_t index, rns_announce_peer_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_known_lock);
    size_t seen = 0;
    for (size_t i = 0; i < RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX; ++i) {
        if (s_known[i].in_use) {
            if (seen == index) {
                fill_peer_info_locked(&s_known[i], out);
                err = ESP_OK;
                break;
            }
            seen++;
        }
    }
    taskEXIT_CRITICAL(&s_known_lock);
    return err;
}
