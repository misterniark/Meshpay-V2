#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Descripteur de monnaie signé par le fondateur.
 *
 * Le corps (meshpay_currency_descriptor_t) porte EXCLUSIVEMENT les RÈGLES de la
 * monnaie + l'AUTORITÉ (clé publique du fondateur). C'est l'unique préimage du
 * « genesis » : SHA-256 de son encodage CBOR canonique. Le currency_id est
 * dérivé des 4 octets de tête du genesis (big-endian) et identifie la monnaie
 * de manière stable sur tout le réseau.
 *
 * Ce composant est volontairement indépendant du transport : le format wire est
 * un CBOR compact et borné (clés entières 1..11) destiné à être diffusé par
 * radio (ESP-NOW/LoRa). Aucune dépendance réseau ici.
 */

/* Longueur max (terminateur nul inclus) du nom lisible de la monnaie. */
#define MESHPAY_CURRENCY_NAME_MAX 24
/* Longueur max (terminateur nul inclus) du symbole/ticker de la monnaie. */
#define MESHPAY_CURRENCY_SYMBOL_MAX 8
/* Borne supérieure du buffer wire pour le descripteur signé encodé. */
#define MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX 384

/* Taille du genesis = SHA-256 du corps canonique. */
#define MESHPAY_CURRENCY_GENESIS_SIZE RNS_CRYPTO_SHA256_SIZE
/* Taille de la signature Ed25519 du fondateur sur le genesis. */
#define MESHPAY_CURRENCY_SIGNATURE_SIZE RNS_CRYPTO_ED25519_SIGNATURE_SIZE

/*
 * Corps signable : les RÈGLES de la monnaie + l'autorité (clé publique du
 * fondateur). C'est l'analogue des champs « signable » d'une meshpay_tx.
 *
 * Les champs texte name/symbol sont stockés bornés et toujours null-terminés
 * après décodage. La sérialisation canonique n'encode que les octets utiles
 * (jusqu'au premier '\0'), de sorte que deux corps logiquement identiques
 * produisent exactement les mêmes octets — condition indispensable au
 * déterminisme du genesis.
 */
typedef struct {
    uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE]; /* clé publique 64 o du fondateur */
    char name[MESHPAY_CURRENCY_NAME_MAX];             /* nom lisible, null-terminé */
    char symbol[MESHPAY_CURRENCY_SYMBOL_MAX];         /* ticker, null-terminé */
    uint64_t max_supply;                              /* offre maximale (0 = illimité) */
    uint32_t transfer_fee;                            /* frais de transfert (unités) */
    bool demurrage_enabled;                           /* fonte/demurrage actif ? */
    uint16_t demurrage_bps;                           /* taux de fonte en points de base */
    uint32_t initial_credit;                          /* crédit initial accordé à un nouveau membre */
    uint64_t created_at_ms;                           /* horodatage de création (ms) */
} meshpay_currency_descriptor_t;

/*
 * Descripteur signé prêt pour le wire : corps + champs DÉRIVÉS (genesis,
 * currency_id) + signature du fondateur. C'est l'analogue d'une meshpay_tx
 * complète (corps + id + signature).
 */
typedef struct {
    meshpay_currency_descriptor_t body;                    /* corps signable */
    uint32_t currency_id;                                  /* DÉRIVÉ : genesis_hash[0..3] big-endian */
    uint8_t genesis_hash[MESHPAY_CURRENCY_GENESIS_SIZE];   /* SHA-256(encode_body canonique) */
    uint8_t founder_signature[MESHPAY_CURRENCY_SIGNATURE_SIZE]; /* sig du fondateur sur le genesis */
} meshpay_currency_descriptor_signed_t;

/* Remet le corps entièrement à zéro. */
void meshpay_currency_descriptor_init(meshpay_currency_descriptor_t *body);

/*
 * Encode CANONIQUE du corps : map CBOR des champs RÈGLES uniquement, clés
 * entières en ordre croissant stable (1..9). C'est l'UNIQUE préimage du genesis.
 */
esp_err_t meshpay_currency_descriptor_encode_body(const meshpay_currency_descriptor_t *body,
                                                  uint8_t *out,
                                                  size_t out_size,
                                                  size_t *out_len);

/*
 * Calcule le genesis = SHA-256(encode_body) et le currency_id
 * (= (out[0]<<24)|(out[1]<<16)|(out[2]<<8)|out[3]).
 */
esp_err_t meshpay_currency_descriptor_compute_genesis(const meshpay_currency_descriptor_t *body,
                                                      uint8_t out_genesis[MESHPAY_CURRENCY_GENESIS_SIZE],
                                                      uint32_t *out_currency_id);

/*
 * Remplit out_signed : copie le corps, renseigne founder_public depuis
 * l'identité du fondateur, calcule genesis + currency_id, puis signe le genesis
 * (32 o). « founder » DOIT détenir la clé privée.
 */
esp_err_t meshpay_currency_descriptor_sign(meshpay_currency_descriptor_signed_t *out_signed,
                                           const meshpay_currency_descriptor_t *body,
                                           const rns_identity_t *founder);

/*
 * Vérifie un descripteur signé : recalcule le genesis depuis signed->body et le
 * compare au genesis stocké (rejet si différent), recharge l'identité publique
 * depuis body.founder_public, puis vérifie la signature sur le genesis.
 */
esp_err_t meshpay_currency_descriptor_verify(const meshpay_currency_descriptor_signed_t *signed_desc);

/*
 * Encode le wire complet : map CBOR (clés 1..9 du corps) + clé 10 = genesis
 * (bstr 32) + clé 11 = signature (bstr 64).
 */
esp_err_t meshpay_currency_descriptor_encode(const meshpay_currency_descriptor_signed_t *signed_desc,
                                             uint8_t *out,
                                             size_t out_size,
                                             size_t *out_len);

/*
 * Décode le wire vers out_signed ET DÉRIVE le currency_id depuis le genesis lu.
 * Ne vérifie PAS la signature (l'appelant enchaîne avec verify). Valide chaque
 * longueur de bstr et borne/termine name & symbol.
 */
esp_err_t meshpay_currency_descriptor_decode(const uint8_t *data,
                                             size_t len,
                                             meshpay_currency_descriptor_signed_t *out_signed);

/*
 * Hash d'identité (16 o) du fondateur = l'AUTORITÉ MINT. Recharge l'identité
 * publique depuis founder_public puis retourne son hash.
 */
esp_err_t meshpay_currency_descriptor_founder_hash(const meshpay_currency_descriptor_signed_t *signed_desc,
                                                   uint8_t out_hash[RNS_IDENTITY_HASH_SIZE]);

#ifdef __cplusplus
}
#endif
