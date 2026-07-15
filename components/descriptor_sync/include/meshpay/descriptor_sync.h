#pragma once

#include "esp_err.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Protocole d'obtention du descripteur de monnaie par radio (Palier B3).
 *
 * Un nouveau membre qui rejoint connaît l'ANCRE (code d'invitation saisi
 * hors-bande) mais pas le descripteur signé (~242 o) qui, lui, voyage par
 * radio. Ce composant définit les deux messages qui l'acheminent :
 *
 *   - REQUEST (membre -> pairs) : « qui détient la monnaie <currency_id> ?
 *     réponds-moi à <source> ». Minuscule.
 *   - OFFER  (pair -> membres)  : le descripteur signé complet, en UN paquet
 *     (≤ 384 o de wire < MDU 464 o, donc pas de fragmentation/Resource).
 *
 * Les DEUX messages sont diffusés en PLAIN broadcast : le descripteur est une
 * donnée PUBLIQUE signée (aucun besoin de chiffrement) et un OFFER diffusé
 * profite à tous les membres qui rejoignent en même temps. Chaque récepteur
 * filtre par l'ancre (matches_anchor) — l'adressage ne sert que de provenance.
 *
 * C'est du CODEC WIRE PUR, transport-agnostique (ESP-NOW/LoRa), sur le même
 * patron « packet-level » que meshpay_dag_sync_build_summary : data[0] = type,
 * puis payload. AUCUNE logique métier ici (pas de verify, pas de matches_anchor,
 * pas de persistance) — c'est le rôle de la machine à états de rejointe (B4).
 */

/* Octets de type (data[0]). 0x31/0x32 = dag_sync, 0x01-0x03 = payment. */
#define MESHPAY_DESCRIPTOR_SYNC_MSG_REQUEST 0x33
#define MESHPAY_DESCRIPTOR_SYNC_MSG_OFFER 0x34
/* Palier E1 : découverte — « quiconque est membre d'une monnaie répond OFFER ».
 * Distinct de REQUEST (pas de currency_id : on ne cherche pas UNE monnaie mais
 * toutes celles à portée) pour ne pas créer de valeur « joker » ambiguë. */
#define MESHPAY_DESCRIPTOR_SYNC_MSG_DISCOVER 0x35

/* Taille exacte du payload REQUEST : type(1) + currency_id(4) + source(16). */
#define MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE \
    (1U + 4U + MESHPAY_TX_DESTINATION_HASH_SIZE)

/* Taille exacte du payload DISCOVER : type(1) + source(16). */
#define MESHPAY_DESCRIPTOR_SYNC_DISCOVER_SIZE \
    (1U + MESHPAY_TX_DESTINATION_HASH_SIZE)

/*
 * Construit un paquet REQUEST diffusé (PLAIN broadcast) : demande le descripteur
 * de la monnaie `currency_id` et indique `source` comme adresse de réponse.
 * `source` ne doit pas être nul (tout à zéro). Rejette les arguments NULL.
 */
esp_err_t meshpay_descriptor_sync_build_request(
    uint32_t currency_id,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);

/*
 * Décode un paquet REQUEST -> currency_id demandé + adresse de réponse `source`.
 * Rejette si data[0] != MSG_REQUEST ou si la taille n'est pas exactement
 * MESHPAY_DESCRIPTOR_SYNC_REQUEST_SIZE. `currency_id`/`source` optionnels (NULL
 * ignoré).
 */
esp_err_t meshpay_descriptor_sync_parse_request(
    const rns_packet_t *packet,
    uint32_t *currency_id,
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE]);

/*
 * Palier E1 — construit un paquet DISCOVER diffusé (PLAIN broadcast) : « que
 * chaque membre d'une monnaie réponde son OFFER ». `source` = adresse de
 * réponse du découvreur (non nulle). Le récepteur membre répond par le même
 * OFFER que pour un REQUEST ciblé.
 */
esp_err_t meshpay_descriptor_sync_build_discover(
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);

/*
 * Décode un paquet DISCOVER -> adresse de réponse `source` (NULL ignoré).
 * Rejette si data[0] != MSG_DISCOVER ou taille != DISCOVER_SIZE.
 */
esp_err_t meshpay_descriptor_sync_parse_discover(
    const rns_packet_t *packet,
    uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE]);

/*
 * Construit un paquet OFFER diffusé (PLAIN broadcast) portant le descripteur :
 * data[0] = MSG_OFFER, suivi de l'encodage wire du descripteur signé. `source`
 * est l'adresse de l'ÉMETTEUR (provenance), posée dans destination_hash comme
 * pour un SUMMARY dag_sync — la sélection se fait par l'ancre côté récepteur,
 * pas par l'adressage. Rejette si le descripteur ne s'encode pas, si `source`
 * est nul, ou args NULL.
 */
esp_err_t meshpay_descriptor_sync_build_offer(
    const meshpay_currency_descriptor_signed_t *signed_desc,
    const uint8_t source[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_packet_t *packet);

/*
 * Décode un paquet OFFER -> descripteur signé (via
 * meshpay_currency_descriptor_decode). Ne vérifie NI la signature NI l'ancre :
 * l'appelant (B4) enchaîne matches_anchor() puis verify(). Rejette si data[0]
 * != MSG_OFFER ou si le wire du descripteur est invalide.
 */
esp_err_t meshpay_descriptor_sync_parse_offer(
    const rns_packet_t *packet,
    meshpay_currency_descriptor_signed_t *out_signed);

#ifdef __cplusplus
}
#endif
