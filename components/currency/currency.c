#include "meshpay/currency.h"

#include "esp_check.h"

#include "meshpay/meshpay_tx.h"
#include "meshpay/rns/rns_crypto.h"
#include "meshpay/rns/rns_destination.h"
#include "meshpay/rns/rns_identity.h"
#include <string.h>

/* Vrai si le tampon est entièrement nul (champ optionnel absent). */
static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

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
    /* Phase B : la masse frappée AVANT l'horizon a quitté la fenêtre mais pas
     * la circulation — dans ce modèle sans destruction (les frais vont à
     * l'autorité), elle vaut exactement la somme des soldes refondés du
     * checkpoint. Le plafond max_supply reste donc exact par récurrence. */
    if (dag->checkpoint.generation != 0 &&
        dag->checkpoint.currency_id == config->currency_id) {
        for (uint16_t i = 0; i < dag->checkpoint.account_count; ++i) {
            total += dag->checkpoint.accounts[i].balance;
        }
    }
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
    /* Phase B : le solde part du checkpoint adopté (l'état refondé signé du
     * fondateur), la fenêtre n'apporte que la contribution post-horizon. */
    if (dag->checkpoint.generation != 0 &&
        dag->checkpoint.currency_id == config->currency_id) {
        const meshpay_checkpoint_account_t *ca =
            meshpay_checkpoint_find_account(&dag->checkpoint, account);
        if (ca != NULL) {
            acc = (int64_t)ca->balance;
        }
    }
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
    /* Phase B : l'annuaire du checkpoint fait foi pour les membres dont la
     * CLAIM a été élaguée (clé non nulle = membre refondé ; un compte à clé
     * nulle est un solde orphelin conservé, PAS un membre). */
    if (dag->checkpoint.generation != 0 &&
        dag->checkpoint.currency_id == config->currency_id) {
        const meshpay_checkpoint_account_t *ca =
            meshpay_checkpoint_find_account(&dag->checkpoint, account);
        if (ca != NULL &&
            !bytes_zero(ca->member_public, RNS_IDENTITY_PUBLIC_SIZE)) {
            return true;
        }
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

esp_err_t meshpay_currency_member_key(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE],
    uint8_t out_public[RNS_IDENTITY_PUBLIC_SIZE])
{
    if (config == NULL || dag == NULL || account == NULL ||
        out_public == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Le fondateur publie sa clé dans le corps SIGNÉ du descripteur —
     * l'annuaire ne vaut que pour une monnaie à descripteur (le repli n'a ni
     * clé fondateur ni CLAIM authentifiables). */
    if (!config->has_descriptor) {
        return ESP_ERR_INVALID_STATE;
    }
    if (meshpay_currency_is_mint_authority(config, account)) {
        memcpy(out_public, config->founder_public, RNS_IDENTITY_PUBLIC_SIZE);
        return ESP_OK;
    }
    /* Phase B : l'annuaire du checkpoint d'abord — la CLAIM d'un membre
     * refondé a été élaguée (et son rejeu est refusé au gate), sa clé vit
     * dans l'état signé. Le lien clé<->compte a été vérifié par le fondateur
     * à la construction et la table est couverte par sa signature. */
    if (dag->checkpoint.generation != 0 &&
        dag->checkpoint.currency_id == config->currency_id) {
        const meshpay_checkpoint_account_t *ca =
            meshpay_checkpoint_find_account(&dag->checkpoint, account);
        if (ca != NULL &&
            !bytes_zero(ca->member_public, RNS_IDENTITY_PUBLIC_SIZE)) {
            memcpy(out_public, ca->member_public, RNS_IDENTITY_PUBLIC_SIZE);
            return ESP_OK;
        }
    }
    /* Membre ordinaire : sa clé est publiée par sa CLAIM (wire v2). Le lien
     * clé<->compte a été vérifié à l'INGESTION (ingest_check) — ici on ne fait
     * que la relire ; une CLAIM pré-v2 (clé nulle) ne compte pas. */
    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        if (currency_claim_valid(config, tx) &&
            account_equal(tx->from, account) &&
            !bytes_zero(tx->member_public, RNS_IDENTITY_PUBLIC_SIZE)) {
            memcpy(out_public, tx->member_public, RNS_IDENTITY_PUBLIC_SIZE);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Vérifie que `public_key` est bien la clé du compte `account` : la
 * destination meshpay.wallet dérivée de la clé doit redonner exactement le
 * hash `account`. C'est le verrou anti-usurpation de l'annuaire : publier la
 * clé d'autrui échoue ici (préimage), publier une clé à soi sous le hash d'un
 * autre aussi. */
static bool currency_key_binds_account(
    const uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE],
    const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE],
    rns_identity_t *out_identity)
{
    if (rns_identity_load_public(out_identity, public_key) != ESP_OK) {
        return false;
    }
    rns_destination_t wallet;
    if (rns_destination_create_meshpay_wallet(out_identity, &wallet) !=
        ESP_OK) {
        return false;
    }
    return memcmp(wallet.hash, account, MESHPAY_TX_DESTINATION_HASH_SIZE) == 0;
}

meshpay_currency_result_t meshpay_currency_ingest_check(
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
    /* Le gate exige l'ancrage descripteur : sans lui, aucune racine de
     * confiance (l'appelant ne gate pas la config de repli). */
    if (!config->has_descriptor) {
        return MESHPAY_CURRENCY_ERR_INVALID;
    }
    /* Phase B — anti-rejeu d'avant-horizon : une tx dont le compte figure au
     * checkpoint avec seq <= plancher a DÉJÀ été comptée dans l'état refondé.
     * Sa re-livraison (pair en retard, ou malveillant qui rejoue l'histoire)
     * est un REJET DÉFINITIF, avant même la crypto. Couvre aussi la re-CLAIM
     * (seq 0 <= plancher pour tout compte refondé). */
    if (meshpay_dag_below_floor(dag, tx)) {
        return MESHPAY_CURRENCY_ERR_REPLAY;
    }

    /* RÈGLES STATELESS UNIQUEMENT (déterministes quel que soit l'ordre
     * d'application) : signature, lien clé<->compte, autorité, montants
     * statiques. Le solde et le plafond de frappe dépendent de l'état au
     * moment de l'application (ordre, forks) : les gater ici ferait diverger
     * les noeuds — ils restent à la défense comptable (get_balance,
     * total_minted) et au futur consensus/checkpoint (Phase B). */

    if (tx->type == MESHPAY_TX_TYPE_MINT) {
        if (!meshpay_currency_is_mint_authority(config, tx->from)) {
            return MESHPAY_CURRENCY_ERR_NOT_AUTHORITY;
        }
        rns_identity_t founder;
        if (rns_identity_load_public(&founder, config->founder_public) !=
            ESP_OK) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        if (meshpay_tx_verify(tx, &founder) != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_BAD_SIGNATURE;
        }
        return MESHPAY_CURRENCY_OK;
    }

    if (tx->type == MESHPAY_TX_TYPE_CLAIM) {
        /* Réflexivité et seq==0 déjà imposés par la forme (validate_common au
         * décodage) ; re-vérifiés en défense (le gate peut recevoir une struct
         * construite autrement). */
        if (!account_equal(tx->from, tx->to) || tx->seq != 0) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        if (tx->amount != config->initial_credit) {
            return MESHPAY_CURRENCY_ERR_BAD_AMOUNT;
        }
        /* Le coeur du wire v2 : la clé publiée DOIT redonner le compte, et la
         * signature DOIT être la sienne. Une CLAIM forgée échoue ici quel que
         * soit son maquillage. */
        rns_identity_t member;
        if (!currency_key_binds_account(tx->member_public, tx->from,
                                        &member)) {
            return MESHPAY_CURRENCY_ERR_BAD_SIGNATURE;
        }
        if (meshpay_tx_verify(tx, &member) != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_BAD_SIGNATURE;
        }
        return MESHPAY_CURRENCY_OK;
    }

    if (tx->type == MESHPAY_TX_TYPE_TRANSFER) {
        if (tx->fee != config->transfer_fee) {
            return MESHPAY_CURRENCY_ERR_BAD_FEE;
        }
        /* L'émetteur doit être au registre (fondateur ou CLAIM déjà ingérée) :
         * sa clé s'y lit, la signature se vérifie contre elle. L'absence est
         * un motif TRANSITOIRE (sa CLAIM peut être en route). */
        uint8_t sender_public[RNS_IDENTITY_PUBLIC_SIZE];
        esp_err_t err = meshpay_currency_member_key(config, dag, tx->from,
                                                    sender_public);
        if (err == ESP_ERR_NOT_FOUND) {
            return MESHPAY_CURRENCY_ERR_UNKNOWN_MEMBER;
        }
        if (err != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        rns_identity_t sender;
        if (rns_identity_load_public(&sender, sender_public) != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_INVALID;
        }
        if (meshpay_tx_verify(tx, &sender) != ESP_OK) {
            return MESHPAY_CURRENCY_ERR_BAD_SIGNATURE;
        }
        return MESHPAY_CURRENCY_OK;
    }

    return MESHPAY_CURRENCY_ERR_INVALID;
}

size_t meshpay_currency_member_count(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag)
{
    if (config == NULL || dag == NULL) {
        return 0;
    }
    /* Phase B : les membres refondés d'abord — comptes de l'annuaire du
     * checkpoint à clé non nulle (les soldes orphelins sans clé ne sont pas
     * des membres), hors autorités (comptées à part, comme avant). */
    size_t count = 0;
    bool has_checkpoint = dag->checkpoint.generation != 0 &&
                          dag->checkpoint.currency_id == config->currency_id;
    if (has_checkpoint) {
        for (uint16_t i = 0; i < dag->checkpoint.account_count; ++i) {
            const meshpay_checkpoint_account_t *ca =
                &dag->checkpoint.accounts[i];
            if (!bytes_zero(ca->member_public, RNS_IDENTITY_PUBLIC_SIZE) &&
                !meshpay_currency_is_mint_authority(config, ca->account)) {
                count++;
            }
        }
    }
    /* Chaque CLAIM valide de la FENÊTRE = un membre distinct : l'unicité
     * (from, seq==0) scopée par monnaie interdit deux CLAIM d'un même compte,
     * et le gate anti-rejeu interdit la CLAIM d'un compte déjà refondé (pas
     * de double compte possible avec l'annuaire ci-dessus). */
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

/* --- Phase B : construction du checkpoint côté fondateur --- */

/* Ajoute `account` au set du checkpoint s'il n'y est pas ; rend son index ou
 * -1 si la table est pleine. */
static int checkpoint_intern_account(meshpay_checkpoint_t *cp,
                                     const uint8_t account[MESHPAY_TX_DESTINATION_HASH_SIZE])
{
    for (uint16_t i = 0; i < cp->account_count; ++i) {
        if (memcmp(cp->accounts[i].account, account,
                   MESHPAY_TX_DESTINATION_HASH_SIZE) == 0) {
            return (int)i;
        }
    }
    if (cp->account_count >= MESHPAY_CHECKPOINT_MAX_ACCOUNTS) {
        return -1;
    }
    int idx = (int)cp->account_count;
    memcpy(cp->accounts[idx].account, account,
           MESHPAY_TX_DESTINATION_HASH_SIZE);
    cp->account_count++;
    return idx;
}

esp_err_t meshpay_currency_build_checkpoint(
    const meshpay_currency_config_t *config,
    const meshpay_dag_t *dag,
    uint64_t created_at_ms,
    meshpay_checkpoint_t *out_cp)
{
    if (config == NULL || dag == NULL || out_cp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Le checkpoint n'a de sens que sous une monnaie à descripteur (c'est la
     * clé du descripteur qui le signera et le fera vérifier partout). */
    if (!config->has_descriptor || config->mint_authority_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    meshpay_checkpoint_init(out_cp);
    out_cp->currency_id = config->currency_id;
    out_cp->generation = dag->checkpoint.generation + 1;
    out_cp->created_at_ms = created_at_ms;
    uint8_t digest[RNS_CRYPTO_SHA256_SIZE];
    ESP_RETURN_ON_ERROR(meshpay_dag_digest(dag, digest), "currency", "");
    memcpy(out_cp->horizon_digest, digest, MESHPAY_CHECKPOINT_DIGEST_SIZE);

    /* 1) Le set des comptes : ceux déjà refondés (récurrence), l'autorité
     * (les frais la créditent même sans tx à son nom), puis tout compte
     * touché par la fenêtre (from et to du registre actif). */
    if (dag->checkpoint.generation != 0 &&
        dag->checkpoint.currency_id == config->currency_id) {
        for (uint16_t i = 0; i < dag->checkpoint.account_count; ++i) {
            if (checkpoint_intern_account(
                    out_cp, dag->checkpoint.accounts[i].account) < 0) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }
    if (checkpoint_intern_account(out_cp, config->mint_authorities[0]) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < meshpay_dag_count(dag); ++i) {
        const meshpay_tx_t *tx = meshpay_dag_at(dag, i);
        if (tx == NULL || tx->currency_id != config->currency_id) {
            continue;
        }
        if (checkpoint_intern_account(out_cp, tx->from) < 0 ||
            checkpoint_intern_account(out_cp, tx->to) < 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    /* 2) Refonte par compte : solde (récurrence via get_balance), plancher
     * (max du plancher hérité et des seq de la fenêtre), annuaire (clé
     * héritée ou publiée par la CLAIM en fenêtre ; autorité = clé nulle). */
    for (uint16_t i = 0; i < out_cp->account_count; ++i) {
        meshpay_checkpoint_account_t *a = &out_cp->accounts[i];
        ESP_RETURN_ON_ERROR(meshpay_currency_get_balance(config, dag,
                                                         a->account,
                                                         &a->balance),
                            "currency", "");
        const meshpay_checkpoint_account_t *prev =
            (dag->checkpoint.generation != 0)
                ? meshpay_checkpoint_find_account(&dag->checkpoint,
                                                  a->account)
                : NULL;
        a->seq_floor = (prev != NULL) ? prev->seq_floor : 0;
        for (size_t j = 0; j < meshpay_dag_count(dag); ++j) {
            const meshpay_tx_t *tx = meshpay_dag_at(dag, j);
            if (tx == NULL || tx->currency_id != config->currency_id ||
                !account_equal(tx->from, a->account)) {
                continue;
            }
            if (tx->seq > a->seq_floor) {
                a->seq_floor = tx->seq;
            }
        }
        if (!meshpay_currency_is_mint_authority(config, a->account)) {
            /* member_key rend la clé héritée du checkpoint ou celle de la
             * CLAIM en fenêtre ; NOT_FOUND = solde orphelin (clé nulle). */
            uint8_t key[RNS_IDENTITY_PUBLIC_SIZE];
            if (meshpay_currency_member_key(config, dag, a->account, key) ==
                ESP_OK) {
                memcpy(a->member_public, key, sizeof(a->member_public));
            }
        }
    }
    return ESP_OK;
}
