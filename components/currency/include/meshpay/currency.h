#pragma once

#include "esp_err.h"
#include "meshpay/dag.h"
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
} meshpay_currency_config_t;

void meshpay_currency_config_init(meshpay_currency_config_t *config,
                                  uint32_t currency_id);
esp_err_t meshpay_currency_add_mint_authority(
    meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE]);
bool meshpay_currency_is_mint_authority(
    const meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE]);

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
