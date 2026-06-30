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
} meshpay_currency_result_t;

typedef struct {
    uint32_t currency_id;
    uint64_t max_supply;
    uint32_t transfer_fee;
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

#ifdef __cplusplus
}
#endif
