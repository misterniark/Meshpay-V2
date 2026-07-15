#pragma once

#include "esp_err.h"
#include "meshpay/currency_descriptor.h"
#include "meshpay/dag.h"
#include "meshpay/rns/rns_identity.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES 4
#define MESHPAY_CURRENCY_BPS_SCALE 10000U

typedef enum {
    MESHPAY_CURRENCY_OK = 0,
    MESHPAY_CURRENCY_ERR_INVALID,
    MESHPAY_CURRENCY_ERR_WRONG_ID,
    MESHPAY_CURRENCY_ERR_NOT_AUTHORITY,
    MESHPAY_CURRENCY_ERR_SUPPLY_EXCEEDED,
    MESHPAY_CURRENCY_ERR_BAD_FEE,
    MESHPAY_CURRENCY_ERR_INSUFFICIENT,
    MESHPAY_CURRENCY_ERR_BAD_SIGNATURE,
    MESHPAY_CURRENCY_ERR_BAD_AMOUNT, /* CLAIM dont amount != initial_credit */
    /* Durcissement ingestion : la clé du compte émetteur n'est pas (encore)
     * dans l'annuaire local (ni fondateur ni CLAIM dans la DAG). Motif
     * TRANSITOIRE par nature : la CLAIM peut être en route par la sync —
     * l'appelant retient/re-tente (cf. rétention F1) au lieu de rejeter. */
    MESHPAY_CURRENCY_ERR_UNKNOWN_MEMBER,
    /* Phase B : tx d'AVANT l'horizon du checkpoint adopté (seq <= plancher du
     * compte) — son effet est déjà dans l'état refondé. Rejet DÉFINITIF :
     * re-livraison d'un pair en retard ou rejeu malveillant. */
    MESHPAY_CURRENCY_ERR_REPLAY,
} meshpay_currency_result_t;

typedef struct {
    uint32_t currency_id;
    uint64_t max_supply;
    uint32_t transfer_fee;
    uint32_t initial_credit; /* Palier C — crédit auto-frappé une fois par un nouveau membre (CLAIM) */
    uint8_t mint_authorities[MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES]
                            [MESHPAY_TX_DESTINATION_HASH_SIZE];
    uint8_t mint_authority_count;
    bool demurrage_enabled;
    uint16_t demurrage_bps;
    /* Palier A — ancrage sur le descripteur de monnaie signé. */
    uint8_t founder_public[RNS_IDENTITY_PUBLIC_SIZE]; /* clés pub fondateur (autorité MINT) */
    bool has_descriptor;                              /* config dérivée d'un descripteur signé */
} meshpay_currency_config_t;

void meshpay_currency_config_init(meshpay_currency_config_t *config,
                                  uint32_t currency_id);
esp_err_t meshpay_currency_add_mint_authority(
    meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE]);
bool meshpay_currency_is_mint_authority(
    const meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE]);

/*
 * Dérive la config runtime depuis un descripteur de monnaie signé :
 *  - règles (currency_id dérivé, max_supply, transfer_fee, demurrage) ;
 *  - autorité MINT UNIQUE = hash d'identité du fondateur ;
 *  - clés publiques du fondateur (pour vérifier la signature des MINT).
 * Positionne has_descriptor = true. Le descripteur DOIT déjà avoir été vérifié
 * par l'appelant (meshpay_currency_descriptor_verify).
 */
esp_err_t meshpay_currency_config_from_descriptor(
    meshpay_currency_config_t *config,
    const meshpay_currency_descriptor_signed_t *descriptor);

meshpay_currency_result_t meshpay_currency_validate_tx(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const meshpay_tx_t *tx);

esp_err_t meshpay_currency_get_balance(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint32_t *balance);
esp_err_t meshpay_currency_total_minted(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t *total_minted);

uint32_t meshpay_currency_apply_demurrage(
    const meshpay_currency_config_t *config,
    uint32_t balance,
    uint32_t ticks);

/*
 * Palier F2 — appartenance à la monnaie, dérivée de la DAG (vérité durable,
 * indépendante des announces radio) : un compte est MEMBRE s'il détient une
 * CLAIM valide (crédit initial réclamé à la rejointe/création) OU s'il est
 * autorité MINT (le fondateur d'une monnaie à crédit nul n'émet pas de CLAIM).
 * Une CLAIM forgée (montant != initial_credit) ne confère PAS l'appartenance,
 * exactement comme elle ne crédite rien (défense en profondeur du Palier C).
 */
bool meshpay_currency_is_member(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE]);

/*
 * Palier F2 — nombre de membres de la monnaie : une CLAIM valide par compte
 * (garanti par l'unicité (from, seq==0) scopée par monnaie, cf. Palier C) +
 * les autorités MINT sans CLAIM. 0 si arguments NULL.
 */
size_t meshpay_currency_member_count(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag);

/*
 * Durcissement ingestion — annuaire des clés dérivé de la DAG : la clé
 * publique d'un compte est celle publiée par sa CLAIM (wire v2), ou celle du
 * descripteur pour le fondateur. ESP_ERR_NOT_FOUND si le compte n'est pas au
 * registre (sa CLAIM n'est peut-être pas encore arrivée : motif transitoire),
 * ESP_ERR_INVALID_STATE sans descripteur (le repli n'a pas d'annuaire).
 */
esp_err_t meshpay_currency_member_key(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint8_t out_public[RNS_IDENTITY_PUBLIC_SIZE]);

/*
 * Durcissement ingestion — gate CRYPTO + règles STATIQUES d'une tx reçue du
 * réseau (batch de sync ou paiement direct), à passer AVANT tout merge DAG :
 *  - MINT : autorité + signature vérifiée contre la clé fondateur (descripteur) ;
 *  - CLAIM : réflexivité, amount == initial_credit, lien clé<->compte
 *    (wallet-hash(member_public) == from) et signature vérifiée contre la clé
 *    publiée — une CLAIM forgée est indistinguable d'un refus de préimage ;
 *  - TRANSFER : fee == transfer_fee, émetteur au registre (sinon
 *    ERR_UNKNOWN_MEMBER, transitoire) et signature vérifiée contre sa clé.
 * VOLONTAIREMENT ABSENTS (dépendants de l'état, donc de l'ordre d'application
 * — les gater ferait diverger les noeuds) : solde et max_supply, qui restent
 * à la défense comptable et au futur consensus (Phase B). Coût : une vérif
 * Ed25519 (~ms) par tx — une fois à l'ingestion, jamais dans la comptabilité.
 */
meshpay_currency_result_t meshpay_currency_ingest_check(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const meshpay_tx_t *tx);

/*
 * Phase B — construit (SANS signer) le checkpoint N+1 côté FONDATEUR depuis
 * l'état complet courant : génération = adoptée + 1, soldes par compte
 * (checkpoint précédent + fenêtre, par récurrence), planchers de seq
 * (max(plancher précédent, seq observés en fenêtre)), annuaire (clé du
 * checkpoint précédent ou de la CLAIM en fenêtre ; l'autorité garde une clé
 * NULLE — la sienne est au descripteur), digest d'horizon (8 o du digest DAG).
 * Le set des comptes = comptes du checkpoint précédent ∪ comptes touchés par
 * la fenêtre ∪ l'autorité — il ne fait que croître ; au-delà de
 * MESHPAY_CHECKPOINT_MAX_ACCOUNTS : ESP_ERR_INVALID_SIZE (l'émission échoue,
 * la fenêtre continue jusqu'à saturation — augmenter le Kconfig).
 * L'appelant signe ensuite (meshpay_checkpoint_sign) puis adopte/diffuse.
 * out_cp fait ~3 Ko : JAMAIS sur une pile de tâche (leçons des chantiers I/M).
 */
esp_err_t meshpay_currency_build_checkpoint(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t created_at_ms,
    meshpay_checkpoint_t *out_cp);

#ifdef __cplusplus
}
#endif
