#pragma once

#include "esp_err.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Checkpoint d'élagage signé par le fondateur (Phase B).
 *
 * C'est un « re-genesis » : l'état monétaire COMPLET (soldes, planchers de
 * seq, annuaire des clés membres) refondé et signé par la racine de confiance
 * existante — la clé du fondateur, celle du descripteur de monnaie. À son
 * adoption, un nœud purge de sa fenêtre toute tx `seq <= seq_floor(from)`
 * (coupe TOTALE : la fenêtre repart vide, les parents pendants des premières
 * tx post-checkpoint sont tolérés depuis le chantier nettoyage N0).
 *
 * Le plancher de seq par compte joue TROIS rôles : il désigne la coupe par
 * CONTENU (indépendamment de l'ordre local des fenêtres), il est l'anti-rejeu
 * (toute tx re-livrée sous plancher est refusée), et il prolonge l'unicité
 * (from,seq) au-delà de la fenêtre.
 *
 * Contrairement au descripteur, le checkpoint ne porte PAS la clé publique du
 * fondateur : elle vient du descripteur déjà vérifié (l'unique racine). La
 * vérification prend donc cette clé en paramètre.
 *
 * Wire : préfixe magic u32 LE + version u16 LE (routable à vie, leçon du
 * chantier migration NVS), puis map CBOR canonique à clés entières. Comme le
 * descripteur et les tx, c'est un objet CANONIQUE (une seule forme d'octets
 * par contenu) — pas un record tolérant : toute clé inconnue est refusée.
 */

#define MESHPAY_CHECKPOINT_MAX_ACCOUNTS CONFIG_MESHPAY_CHECKPOINT_MAX_ACCOUNTS

#define MESHPAY_CHECKPOINT_MAGIC 0x504b4843u /* 'C','H','K','P' little-endian */
#define MESHPAY_CHECKPOINT_VERSION 1u
#define MESHPAY_CHECKPOINT_PREFIX_SIZE 6u

/* Digest court de la DAG du fondateur à l'instant de la coupe : traçabilité
 * et diagnostic (même largeur que le digest des SUMMARY) — la coupe elle-même
 * est désignée par les planchers, pas par ce digest. */
#define MESHPAY_CHECKPOINT_DIGEST_SIZE 8

#define MESHPAY_CHECKPOINT_SIGNATURE_SIZE RNS_CRYPTO_ED25519_SIGNATURE_SIZE

/* Borne wire : préfixe 6 + en-tête map ~30 + comptes (~96 CBOR chacun)
 * + signature 70. Dimensionnée pour MAX_ACCOUNTS = 128 (le pire Kconfig). */
#define MESHPAY_CHECKPOINT_CBOR_MAX \
    (6 + 40 + (size_t)MESHPAY_CHECKPOINT_MAX_ACCOUNTS * 96 + 70)

/* Un compte refondé. member_public == zéros a un sens réservé : « la clé de
 * ce compte est celle du descripteur » — c'est le compte wallet du FONDATEUR
 * (sa clé, racine de confiance, n'est jamais dupliquée ici). Tout autre
 * compte DOIT porter la clé publiée par sa CLAIM (l'annuaire survit à
 * l'élagage — exigence du chantier durcissement ingestion). */
typedef struct {
    uint8_t account[RNS_IDENTITY_HASH_SIZE]; /* hash destination wallet (16) */
    uint32_t balance;                        /* solde refondé */
    uint32_t seq_floor;                      /* dernier seq consommé (0 = seule
                                              * la CLAIM seq=0 est sous coupe) */
    uint8_t member_public[RNS_IDENTITY_PUBLIC_SIZE]; /* annuaire (64) */
} meshpay_checkpoint_account_t;

typedef struct {
    uint32_t currency_id;   /* monnaie concernée (dérivée du genesis) */
    uint32_t generation;    /* identité d'horizon, monotone, >= 1 (0 réservé :
                             * « aucun checkpoint », état genesis) */
    uint64_t created_at_ms; /* horodatage d'émission chez le fondateur */
    uint8_t horizon_digest[MESHPAY_CHECKPOINT_DIGEST_SIZE];
    uint16_t account_count;
    meshpay_checkpoint_account_t accounts[MESHPAY_CHECKPOINT_MAX_ACCOUNTS];
    uint8_t founder_signature[MESHPAY_CHECKPOINT_SIGNATURE_SIZE];
} meshpay_checkpoint_t;

void meshpay_checkpoint_init(meshpay_checkpoint_t *cp);

/* Encode CANONIQUE du corps signable (tout sauf la signature) : préfixe
 * magic+version puis map CBOR clés 1..5, comptes en array d'arrays fixes.
 * C'est l'unique préimage de la signature. */
esp_err_t meshpay_checkpoint_encode_body(const meshpay_checkpoint_t *cp,
                                         uint8_t *out,
                                         size_t out_size,
                                         size_t *out_len);

/* SHA-256 de l'encodage canonique du corps. */
esp_err_t meshpay_checkpoint_compute_hash(const meshpay_checkpoint_t *cp,
                                          uint8_t out_hash[RNS_CRYPTO_SHA256_SIZE]);

/* Signe le corps (hash) avec l'identité du FONDATEUR (clé privée requise).
 * Valide le corps d'abord (generation >= 1, comptes uniques, bornes). */
esp_err_t meshpay_checkpoint_sign(meshpay_checkpoint_t *cp,
                                  const rns_identity_t *founder);

/* Vérifie la signature contre la clé publique du fondateur — celle du
 * DESCRIPTEUR vérifié (la racine de confiance), jamais une clé reçue avec le
 * checkpoint. Recalcule le hash du corps puis vérifie Ed25519. */
esp_err_t meshpay_checkpoint_verify(const meshpay_checkpoint_t *cp,
                                    const uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE]);

/* Wire complet : corps canonique + clé 6 = signature. */
esp_err_t meshpay_checkpoint_encode(const meshpay_checkpoint_t *cp,
                                    uint8_t *out,
                                    size_t out_size,
                                    size_t *out_len);

/* Décode un wire complet (bornes strictes, comptes uniques exigés, clés
 * inconnues REFUSÉES — objet canonique). Ne vérifie pas la signature :
 * enchaîner avec meshpay_checkpoint_verify. */
esp_err_t meshpay_checkpoint_decode(const uint8_t *data,
                                    size_t len,
                                    meshpay_checkpoint_t *cp);

/* Cherche un compte ; NULL si absent. */
const meshpay_checkpoint_account_t *meshpay_checkpoint_find_account(
    const meshpay_checkpoint_t *cp,
    const uint8_t account[RNS_IDENTITY_HASH_SIZE]);

#ifdef __cplusplus
}
#endif
