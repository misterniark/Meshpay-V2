#include "meshpay/currency.h"

#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_identity.h"
#include <string.h>

static bool account_equal(const uint8_t a[MESHPAY_TX_DESTINATION_HASH_SIZE],
                          const uint8_t b[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    return rns_crypto_constant_equal(a, b, MESHPAY_TX_DESTINATION_HASH_SIZE);
}

static bool account_zero(const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    uint8_t acc = 0;
    for (size_t i = 0; i < MESHPAY_TX_DESTINATION_HASH_SIZE; ++i) {
        acc |= account[i];
    }
    return acc == 0;
}

void meshpay_currency_config_init(meshpay_currency_config_t *config,
                                  uint32_t currency_id)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->currency_id = currency_id;
}

esp_err_t meshpay_currency_add_mint_authority(
    meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (config == NULL || authority == NULL || account_zero(authority)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (meshpay_currency_is_mint_authority(config, authority)) {
        return ESP_OK;
    }
    if (config->mint_authority_count >= MESHPAY_CURRENCY_MAX_MINT_AUTHORITIES) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(config->mint_authorities[config->mint_authority_count], authority,
           MESHPAY_TX_DESTINATION_HASH_SIZE);
    config->mint_authority_count++;
    return ESP_OK;
}

bool meshpay_currency_is_mint_authority(
    const meshpay_currency_config_t *config,
    const uint8_t authority[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (config == NULL || authority == NULL || account_zero(authority)) {
        return false;
    }
    for (uint8_t i = 0; i < config->mint_authority_count; ++i) {
        if (account_equal(config->mint_authorities[i], authority)) {
            return true;
        }
    }
    return false;
}

esp_err_t meshpay_currency_config_from_descriptor(
    meshpay_currency_config_t *config,
    const meshpay_currency_descriptor_signed_t *descriptor)
{
    if (config == NULL || descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Hash d'identité 16 o du fondateur = autorité MINT unique. On le calcule
     * AVANT de toucher à config, pour ne rien laisser à moitié initialisé en
     * cas d'échec (descripteur sans clés publiques valides). */
    uint8_t founder_hash[MESHPAY_TX_DESTINATION_HASH_SIZE];
    esp_err_t err =
        meshpay_currency_descriptor_founder_hash(descriptor, founder_hash);
    if (err != ESP_OK) {
        return err;
    }

    /* Règles reprises telles quelles depuis le corps signé. currency_id est le
     * champ DÉRIVÉ du genesis (rempli par decode/sign du descripteur). */
    meshpay_currency_config_init(config, descriptor->currency_id);
    config->max_supply = descriptor->body.max_supply;
    config->transfer_fee = descriptor->body.transfer_fee;
    config->initial_credit = descriptor->body.initial_credit;
    config->demurrage_enabled = descriptor->body.demurrage_enabled;
    config->demurrage_bps = descriptor->body.demurrage_bps;

    /* Le fondateur est la SEULE autorité de frappe. */
    err = meshpay_currency_add_mint_authority(config, founder_hash);
    if (err != ESP_OK) {
        return err;
    }

    /* Clés publiques du fondateur : servent à vérifier la signature des MINT
     * (cf. meshpay_currency_validate_tx, durcissement Palier A). */
    memcpy(config->founder_public, descriptor->body.founder_public,
           sizeof(config->founder_public));
    config->has_descriptor = true;
    return ESP_OK;
}

esp_err_t meshpay_currency_total_minted(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t *total_minted)
{
    if (config == NULL || dag == NULL || total_minted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        if (tx == NULL || tx->currency_id != config->currency_id) {
            continue;
        }
        /* Offre créée = MINT frappé par une autorité + CLAIM (crédit initial
         * auto-frappé par un membre, réflexif from==to). Les deux comptent dans
         * total_minted, donc dans le plafond max_supply. */
        if (tx->type == MESHPAY_TX_TYPE_MINT &&
            meshpay_currency_is_mint_authority(config, tx->from)) {
            total += tx->amount;
        } else if (tx->type == MESHPAY_TX_TYPE_CLAIM &&
                   account_equal(tx->from, tx->to) &&
                   tx->amount == config->initial_credit) {
            /* Défense en profondeur : ne compter une CLAIM que si son montant est
             * EXACTEMENT le crédit initial (analogue au gate is_mint_authority du
             * MINT). Une CLAIM forgée à montant arbitraire, injectée via la sync
             * sans passer par validate_tx, est ainsi neutralisée comptablement. */
            total += tx->amount;
        }
    }
    *total_minted = total;
    return ESP_OK;
}

esp_err_t meshpay_currency_get_balance(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint32_t *balance)
{
    if (config == NULL || dag == NULL || account == NULL || balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t acc = 0;
    bool has_fee_recipient = config->mint_authority_count > 0;
    const uint8_t *fee_recipient =
        has_fee_recipient ? config->mint_authorities[0] : NULL;

    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        if (tx == NULL || tx->currency_id != config->currency_id) {
            continue;
        }

        if (tx->type == MESHPAY_TX_TYPE_MINT) {
            if (meshpay_currency_is_mint_authority(config, tx->from) &&
                account_equal(tx->to, account)) {
                acc += tx->amount;
            }
            continue;
        }

        if (tx->type == MESHPAY_TX_TYPE_CLAIM) {
            /* Crédit initial réflexif (from == to == membre) : pur crédit du
             * membre, comme un MINT, sans débit. DÉFENSE EN PROFONDEUR : on ne
             * crédite QUE si le montant vaut EXACTEMENT initial_credit — une CLAIM
             * forgée à montant arbitraire (chemin de sync non validé) crédite 0,
             * exactement comme un MINT forgé d'un non-autorité (gate ci-dessus). */
            if (account_equal(tx->from, tx->to) &&
                account_equal(tx->to, account) &&
                tx->amount == config->initial_credit) {
                acc += tx->amount;
            }
            continue;
        }

        if (tx->type == MESHPAY_TX_TYPE_TRANSFER) {
            if (account_equal(tx->to, account)) {
                acc += tx->amount;
            }
            if (account_equal(tx->from, account)) {
                acc -= (int64_t)tx->amount + (int64_t)tx->fee;
            }
            if (has_fee_recipient && tx->fee > 0 &&
                account_equal(fee_recipient, account)) {
                acc += tx->fee;
            }
        }
    }

    if (acc < 0) {
        acc = 0;
    }
    if (acc > UINT32_MAX) {
        acc = UINT32_MAX;
    }
    *balance = (uint32_t)acc;
    return ESP_OK;
}

meshpay_currency_result_t meshpay_currency_validate_tx(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const meshpay_tx_t *tx)
{
    if (config == NULL || dag == NULL || tx == NULL) {
        return MESHPAY_CURRENCY_ERR_INVALID;
    }
    if (tx->currency_id != config->currency_id) {
        return MESHPAY_CURRENCY_ERR_WRONG_ID;
    }

    if (tx->type == MESHPAY_TX_TYPE_MINT) {
        if (!meshpay_currency_is_mint_authority(config, tx->from)) {
            return MESHPAY_CURRENCY_ERR_NOT_AUTHORITY;
        }

        /* Durcissement Palier A : si la config est ancrée sur un descripteur
         * signé, la signature de la TX MINT est vérifiée INCONDITIONNELLEMENT
         * contre la clé publique embarquée du fondateur. Sans cela, un attaquant
         * pourrait forger un MINT avec from = hash fondateur (public) et une
         * signature bidon, accepté par un pair qui ne connaît pas encore
         * l'identité du fondateur (faille d'inflation). */
        if (config->has_descriptor) {
            rns_identity_t founder;
            if (rns_identity_load_public(&founder, config->founder_public) !=
                ESP_OK) {
                return MESHPAY_CURRENCY_ERR_INVALID;
            }
            if (meshpay_tx_verify(tx, &founder) != ESP_OK) {
                return MESHPAY_CURRENCY_ERR_BAD_SIGNATURE;
            }
        }

        if (config->max_supply > 0) {
            uint64_t total = 0;
            if (meshpay_currency_total_minted(config, dag, &total) != ESP_OK) {
                return MESHPAY_CURRENCY_ERR_INVALID;
            }
            if (total + tx->amount > config->max_supply) {
                return MESHPAY_CURRENCY_ERR_SUPPLY_EXCEEDED;
            }
        }
        return MESHPAY_CURRENCY_OK;
    }

    if (tx->type == MESHPAY_TX_TYPE_CLAIM) {
        /* CLAIM = crédit initial réflexif auto-frappé par un membre.
         * from == to == membre : invariant garanti par meshpay_tx, revérifié
         * ici en défense en profondeur. */
        if (!account_equal(tx->from, tx->to)) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        /* Montant EXACT = crédit initial figé dans le corps signé du descripteur.
         * Autorisation UNIVERSELLE : tout membre détenant le descripteur peut
         * réclamer une fois. Contrairement au MINT, PAS de vérif de la clé
         * fondateur — la signature du membre est vérifiée à l'ingestion (comme
         * un TRANSFER), pas ici. L'anti-inflation repose entièrement sur le
         * plafond max_supply + l'unicité (from, seq==0) du DAG. */
        if (tx->amount != config->initial_credit) {
            return MESHPAY_CURRENCY_ERR_BAD_AMOUNT;
        }
        if (config->max_supply > 0) {
            uint64_t total = 0;
            if (meshpay_currency_total_minted(config, dag, &total) != ESP_OK) {
                return MESHPAY_CURRENCY_ERR_INVALID;
            }
            if (total + tx->amount > config->max_supply) {
                return MESHPAY_CURRENCY_ERR_SUPPLY_EXCEEDED;
            }
        }
        return MESHPAY_CURRENCY_OK;
    }

    if (tx->type == MESHPAY_TX_TYPE_TRANSFER) {
        if (tx->fee != config->transfer_fee) {
            return MESHPAY_CURRENCY_ERR_BAD_FEE;
        }

        uint32_t sender_balance = 0;
        if (meshpay_currency_get_balance(config, dag, tx->from,
                                         &sender_balance) != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        uint64_t cost = (uint64_t)tx->amount + tx->fee;
        if (cost > sender_balance) {
            return MESHPAY_CURRENCY_ERR_INSUFFICIENT;
        }
        return MESHPAY_CURRENCY_OK;
    }

    return MESHPAY_CURRENCY_ERR_INVALID;
}

uint32_t meshpay_currency_apply_demurrage(
    const meshpay_currency_config_t *config,
    uint32_t balance,
    uint32_t ticks)
{
    if (config == NULL || !config->demurrage_enabled ||
        config->demurrage_bps == 0 || ticks == 0 || balance == 0) {
        return balance;
    }

    if (config->demurrage_bps >= MESHPAY_CURRENCY_BPS_SCALE) {
        return 0;
    }

    uint64_t current = balance;
    uint32_t factor = MESHPAY_CURRENCY_BPS_SCALE - config->demurrage_bps;
    for (uint32_t i = 0; i < ticks && current > 0; ++i) {
        current = (current * factor) / MESHPAY_CURRENCY_BPS_SCALE;
    }
    return (uint32_t)current;
}

/* Palier F2 — vrai ssi `tx` est une CLAIM VALIDE de cette monnaie : réflexive
 * (from == to) et au montant exact du crédit initial (une CLAIM forgée à un
 * autre montant ne crédite rien et ne confère pas l'appartenance). */
static bool currency_claim_valid(const meshpay_currency_config_t *config,
                                 const meshpay_tx_t *tx)
{
    return tx != NULL && tx->type == MESHPAY_TX_TYPE_CLAIM &&
           tx->currency_id == config->currency_id &&
           account_equal(tx->from, tx->to) &&
           tx->amount == config->initial_credit;
}

bool meshpay_currency_is_member(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    if (config == NULL || dag == NULL || account == NULL) {
        return false;
    }
    /* Le fondateur (autorité MINT) est membre par construction, même sans
     * CLAIM (monnaie à crédit initial nul). */
    if (meshpay_currency_is_mint_authority(config, account)) {
        return true;
    }
    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        if (currency_claim_valid(config, tx) &&
            account_equal(tx->from, account)) {
            return true;
        }
    }
    return false;
}

size_t meshpay_currency_member_count(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag)
{
    if (config == NULL || dag == NULL) {
        return 0;
    }
    /* Chaque CLAIM valide = un membre distinct : l'unicité (from, seq==0)
     * scopée par monnaie interdit deux CLAIM d'un même compte. */
    size_t count = 0;
    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        if (currency_claim_valid(config, meshpay_dag_at(dag, i))) {
            count++;
        }
    }
    /* + les autorités MINT qui n'ont pas de CLAIM (fondateur, crédit nul). */
    for (size_t a = 0; a < config->mint_authority_count; ++a) {
        bool has_claim = false;
        for (size_t i = 0; i < meshpay_dag_count(dag) && !has_claim; ++i) {
            const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
            if (currency_claim_valid(config, tx) &&
                account_equal(tx->from, config->mint_authorities[a])) {
                has_claim = true;
            }
        }
        if (!has_claim) {
            count++;
        }
    }
    return count;
}
