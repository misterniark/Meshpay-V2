#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_destination.h"
#include "meshpay/rns/rns_identity.h"
#include "meshpay/rns/rns_packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_ANNOUNCE_RANDOM_HASH_SIZE 10
#define RNS_ANNOUNCE_PUBLIC_KEY_SIZE RNS_IDENTITY_PUBLIC_SIZE
#define RNS_ANNOUNCE_SIGNATURE_SIZE RNS_CRYPTO_ED25519_SIGNATURE_SIZE
#define RNS_ANNOUNCE_BASE_SIZE \
    (RNS_ANNOUNCE_PUBLIC_KEY_SIZE + RNS_DESTINATION_NAME_HASH_SIZE + \
     RNS_ANNOUNCE_RANDOM_HASH_SIZE + RNS_ANNOUNCE_SIGNATURE_SIZE)
#define RNS_ANNOUNCE_MAX_APP_DATA_SIZE (RNS_PACKET_MAX_DATA_SIZE - RNS_ANNOUNCE_BASE_SIZE)
#define RNS_ANNOUNCE_KNOWN_DESTINATIONS_MAX 16

typedef struct {
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    uint8_t name_hash[RNS_DESTINATION_NAME_HASH_SIZE];
    uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE];
    uint8_t signature[RNS_ANNOUNCE_SIGNATURE_SIZE];
    uint8_t app_data[RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t app_data_len;
} rns_announce_t;

typedef struct {
    bool in_use;
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t packet_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    uint8_t app_data[RNS_ANNOUNCE_MAX_APP_DATA_SIZE];
    size_t app_data_len;
} rns_announce_known_destination_t;

/* Vue légère d'un pair de l'annuaire, COPIÉE sous verrou : sûre à conserver et
 * à lire sans synchronisation. app_data y est tronquée à 32 octets — assez
 * pour les usages réels (alias : MESHPAY_STORAGE_ALIAS_MAX = 32, labels UI :
 * MESHPAY_UI_PEER_LABEL_MAX = 32) tout en restant compatible avec les piles
 * firmware serrées (~120 octets contre ~450 pour la copie intégrale). */
#define RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX 32

typedef struct {
    uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE];
    uint8_t public_key[RNS_ANNOUNCE_PUBLIC_KEY_SIZE];
    uint8_t app_data[RNS_ANNOUNCE_PEER_INFO_APP_DATA_MAX];
    size_t app_data_len; /* longueur COPIÉE (déjà tronquée au max ci-dessus) */
} rns_announce_peer_info_t;

esp_err_t rns_announce_encode(const rns_destination_t *destination,
                              const rns_identity_t *identity,
                              const uint8_t random_hash[RNS_ANNOUNCE_RANDOM_HASH_SIZE],
                              const uint8_t *app_data,
                              size_t app_data_len,
                              uint8_t *out,
                              size_t out_len,
                              size_t *written);
esp_err_t rns_announce_decode(const rns_packet_t *packet,
                              rns_announce_t *out);
esp_err_t rns_announce_verify(const rns_packet_t *packet,
                              rns_announce_t *out);
esp_err_t rns_announce_verify_and_remember(const rns_packet_t *packet,
                                           rns_announce_t *out);

/* Annuaire des destinations connues (table interne s_known).
 *
 * MODÈLE DE CONCURRENCE : la table est écrite par la tâche radio (via
 * verify_and_remember) et lue par les tâches UI/core. Tous les accès sont
 * sérialisés par un verrou interne au composant, et les lectures renvoient des
 * COPIES — jamais de pointeur vers la table vivante, qui exposerait des slots
 * en cours de réécriture (course de données C11 constatée le 2026-07-16). */
void rns_announce_known_reset(void);
size_t rns_announce_known_count(void);

/* Copie intégrale du slot correspondant à destination_hash (y compris
 * packet_hash et l'app_data complète). ESP_ERR_NOT_FOUND si inconnu. */
esp_err_t rns_announce_recall_copy(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
    rns_announce_known_destination_t *out);

/* Vue légère du pair correspondant à destination_hash.
 * ESP_ERR_NOT_FOUND si inconnu. */
esp_err_t rns_announce_recall_info(
    const uint8_t destination_hash[RNS_DESTINATION_HASH_SIZE],
    rns_announce_peer_info_t *out);

/* Vue légère du index-ième pair OCCUPÉ (index ordinal, même sémantique que
 * l'itération historique). ESP_ERR_NOT_FOUND au-delà du dernier occupé.
 * NB : entre deux appels la table peut évoluer (annonce reçue en parallèle) —
 * une itération peut donc voir un pair bouger d'index ; chaque copie reste en
 * revanche individuellement cohérente. */
esp_err_t rns_announce_known_info(size_t index, rns_announce_peer_info_t *out);

#ifdef __cplusplus
}
#endif
