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

#define MESHPAY_STORAGE_ALIAS_MAX 32
#define MESHPAY_STORAGE_CHECKPOINT_MAX 512
/* Blob CBOR opaque du descripteur de monnaie signé (Palier A). storage ne
 * connaît pas son contenu : il le stocke/relit tel quel, la vérification de
 * signature est faite par la couche currency. Borne alignée sur la borne wire
 * du descripteur (MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX = 384). */
#define MESHPAY_STORAGE_DESCRIPTOR_MAX 384
#define MESHPAY_STORAGE_MAGIC 0x4d505356u
/* Historique des schémas du blob persisté :
 *   v1 : struct C brute, avant le descripteur de monnaie (firmwares de juin).
 *   v2 : struct C brute + blob descripteur (Palier A). Fragile : sizeof
 *        changeait à chaque évolution -> storage mort (leçon P1 Palier E).
 *   v3 : chantier migration NVS (2026-07-15) — préfixe magic u32 LE +
 *        version u16 LE, puis map CBOR à clés entières, TAILLE VARIABLE :
 *          1: identity_private  bstr(64)      présent <=> has_identity
 *          2: alias             tstr(1..31)   absent  <=> alias vide
 *          3: pin_hash          bstr(32)      présent <=> has_pin_hash
 *          4: next_seq          uint(u32)     absent  <=> 0
 *          5: checkpoint_seq    uint(u32)   \
 *          6: checkpoint_hash   bstr(32)     | tous trois présents <=>
 *          7: checkpoint        bstr(1..512)/  has_checkpoint
 *          8: currency_descriptor bstr(1..384) présent <=> has_currency_descriptor
 *        Tolérance DANS LES DEUX SENS : une clé inconnue est ignorée au
 *        decode (un firmware v3 relit un record enrichi plus tard), une clé
 *        absente prend sa valeur par défaut (un firmware enrichi relit un
 *        vieux record). CONTRAINTE pour les ajouts futurs : toute nouvelle
 *        valeur doit être un scalaire CBOR (uint/nint) ou une chaîne
 *        (bstr/tstr) — le skip d'inconnu ne traverse pas les conteneurs ;
 *        un contenu structuré s'emballe dans un bstr opaque (comme le
 *        descripteur). Un blob v1/v2 (ou d'une version FUTURE après bump de
 *        format) est qualifié LEGACY par meshpay_storage_load_ex — jamais
 *        chargé tel quel, jamais traité comme absent, migrable/archivable
 *        (voir Doctech V2/chantier_migration_nvs.md). */
#define MESHPAY_STORAGE_VERSION 3u
#define MESHPAY_STORAGE_NVS_NAMESPACE "meshpay"
/* Clés NVS du record et de son backup (bornées à 15 caractères par NVS). */
#define MESHPAY_STORAGE_STATE_KEY "meshpay_state"
#define MESHPAY_STORAGE_BACKUP_KEY "meshpay_bak"
/* Préfixe stable en tête de TOUT blob record depuis v1 : magic u32 LE puis
 * version u16 LE (offsets 0 et 4 de la struct brute). C'est lui qui route les
 * schémas au load — il doit survivre à tous les futurs formats. */
#define MESHPAY_STORAGE_PREFIX_SIZE 6u
/* Borne dure de plausibilité d'un blob record, toutes versions confondues
 * (struct brute ~1,1 Ko aujourd'hui, CBOR v3 ensuite). Au-delà : CORRUPT,
 * jamais lu ni archivé (protège la pile — le buffer de travail du load fait
 * cette taille). */
#define MESHPAY_STORAGE_BLOB_MAX 1536u

typedef esp_err_t (*meshpay_storage_write_blob_fn_t)(void *ctx,
                                                     const char *key,
                                                     const void *data,
                                                     size_t len);
typedef esp_err_t (*meshpay_storage_read_blob_fn_t)(void *ctx,
                                                    const char *key,
                                                    void *data,
                                                    size_t *len);
typedef esp_err_t (*meshpay_storage_erase_fn_t)(void *ctx,
                                                const char *key);

typedef struct {
    meshpay_storage_write_blob_fn_t write_blob;
    meshpay_storage_read_blob_fn_t read_blob;
    meshpay_storage_erase_fn_t erase;
    void *ctx;
} meshpay_storage_backend_t;

typedef esp_err_t (*meshpay_storage_nvs_noarg_fn_t)(void);

typedef struct {
    meshpay_storage_nvs_noarg_fn_t init;
    meshpay_storage_nvs_noarg_fn_t erase;
} meshpay_storage_nvs_init_ops_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    bool has_identity;
    bool has_pin_hash;
    bool has_checkpoint;
    uint8_t identity_private[RNS_IDENTITY_PRIVATE_SIZE];
    char alias[MESHPAY_STORAGE_ALIAS_MAX];
    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    uint32_t next_seq;
    uint32_t checkpoint_seq;
    uint8_t checkpoint_hash[RNS_CRYPTO_SHA256_SIZE];
    uint8_t checkpoint[MESHPAY_STORAGE_CHECKPOINT_MAX];
    size_t checkpoint_len;
    /* Palier A — descripteur de monnaie signé, stocké en blob CBOR opaque. */
    bool has_currency_descriptor;
    uint8_t currency_descriptor[MESHPAY_STORAGE_DESCRIPTOR_MAX];
    size_t currency_descriptor_len;
} meshpay_storage_record_t;

void meshpay_storage_record_init(meshpay_storage_record_t *record);
esp_err_t meshpay_storage_record_set_identity(meshpay_storage_record_t *record,
                                              const uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE]);
esp_err_t meshpay_storage_record_set_alias(meshpay_storage_record_t *record,
                                           const char *alias);
esp_err_t meshpay_storage_record_set_pin_hash(meshpay_storage_record_t *record,
                                              const uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE]);
esp_err_t meshpay_storage_record_set_checkpoint(meshpay_storage_record_t *record,
                                                uint32_t checkpoint_seq,
                                                const uint8_t *checkpoint,
                                                size_t checkpoint_len);
/* Stocke le blob CBOR opaque du descripteur de monnaie signé (1..MAX octets).
 * storage reste agnostique : aucune validation de contenu ici (la signature est
 * vérifiée par la couche currency au chargement). */
esp_err_t meshpay_storage_record_set_currency_descriptor(
    meshpay_storage_record_t *record,
    const uint8_t *descriptor,
    size_t descriptor_len);
esp_err_t meshpay_storage_save(const meshpay_storage_backend_t *backend,
                               const meshpay_storage_record_t *record);
esp_err_t meshpay_storage_load(const meshpay_storage_backend_t *backend,
                               meshpay_storage_record_t *record);
esp_err_t meshpay_storage_erase(const meshpay_storage_backend_t *backend);

/* Chantier migration NVS (M1) — qualification du blob au load.
 * Le motif permet à l'appelant (boot) de router : EMPTY → init neuf, LEGACY →
 * migration (M3) ou archivage, CORRUPT → archivage + mode dégradé VISIBLE,
 * ERROR → dégradé SANS geste destructif (l'E/S peut être transitoire). */
typedef enum {
    MESHPAY_STORAGE_PROBE_OK = 0,   /* record du schéma courant chargé */
    MESHPAY_STORAGE_PROBE_EMPTY,    /* aucun blob : premier boot */
    MESHPAY_STORAGE_PROBE_LEGACY,   /* préfixe magic reconnu, schéma non
                                     * courant (v1/v2 d'un autre layout, ou
                                     * version FUTURE après downgrade) */
    MESHPAY_STORAGE_PROBE_CORRUPT,  /* préfixe inconnu, taille implausible ou
                                     * contenu invalide (CRC, invariants) */
    MESHPAY_STORAGE_PROBE_ERROR,    /* erreur d'E/S backend : indéterminé */
} meshpay_storage_probe_t;

/* Comme meshpay_storage_load, mais qualifie le motif d'échec dans *probe
 * (NULL toléré). ESP_OK ⇔ probe OK. LEGACY → ESP_ERR_INVALID_VERSION ;
 * CORRUPT → code de la vérification en cause (CRC/état/taille) ; EMPTY →
 * ESP_ERR_NOT_FOUND. Ne modifie JAMAIS la flash. */
esp_err_t meshpay_storage_load_ex(const meshpay_storage_backend_t *backend,
                                  meshpay_storage_record_t *record,
                                  meshpay_storage_probe_t *probe);

/* Archive le blob record TEL QUEL sous la clé de backup, sans l'interpréter.
 * Invariant du chantier : ne JAMAIS réécrire un blob illisible sans backup
 * préalable — appeler ceci AVANT tout save par-dessus un load non-EMPTY en
 * échec. Un backup existant n'est JAMAIS écrasé (le plus ancien témoin est
 * le plus précieux) : dans ce cas ESP_OK avec *archived=false. NOT_FOUND si
 * aucun record à archiver ; INVALID_SIZE si le blob dépasse la borne (alors
 * NE PAS écraser le record — l'invariant tient par le refus). */
esp_err_t meshpay_storage_archive(const meshpay_storage_backend_t *backend,
                                  bool *archived);

/* Charge le record en migrant au besoin un blob v2-struct de la flotte
 * d'avant le format CBOR (M3). Comportement par motif :
 *   OK      -> record chargé, *migrated=false ;
 *   LEGACY v2 (taille du gel) -> conversion champ à champ, ARCHIVE du blob
 *              original (invariant : jamais de save par-dessus un blob non
 *              sauvegardé — si l'archive échoue, PAS de save), save v3,
 *              *migrated=true. Idempotent : au boot suivant le load est OK ;
 *   LEGACY autre (v1, bump futur) -> archive best-effort, INVALID_VERSION ;
 *   CORRUPT -> archive best-effort, code du load ;
 *   EMPTY   -> NOT_FOUND (à traiter comme premier boot).
 * Le motif détaillé est disponible via *probe (NULL toléré). */
esp_err_t meshpay_storage_migrate(const meshpay_storage_backend_t *backend,
                                  meshpay_storage_record_t *record,
                                  bool *migrated,
                                  meshpay_storage_probe_t *probe);

/* Mock deux clés : les champs historiques (present/blob/…/counts) restent le
 * slot du record (MESHPAY_STORAGE_STATE_KEY) — compat avec les tests
 * existants qui les inspectent — et les champs bak_* portent le slot backup
 * (MESHPAY_STORAGE_BACKUP_KEY). Les buffers font BLOB_MAX (pas sizeof(record))
 * pour pouvoir simuler les blobs d'un AUTRE schéma, y compris plus grands. */
typedef struct {
    bool present;
    uint8_t blob[MESHPAY_STORAGE_BLOB_MAX];
    size_t blob_len;
    uint32_t write_count;
    uint32_t read_count;
    uint32_t erase_count;
    bool bak_present;
    uint8_t bak_blob[MESHPAY_STORAGE_BLOB_MAX];
    size_t bak_blob_len;
    uint32_t bak_write_count;
    uint32_t bak_read_count;
    uint32_t bak_erase_count;
} meshpay_storage_mock_t;

void meshpay_storage_mock_init(meshpay_storage_mock_t *mock);
meshpay_storage_backend_t meshpay_storage_mock_backend(meshpay_storage_mock_t *mock);

esp_err_t meshpay_storage_nvs_init(void);
esp_err_t meshpay_storage_nvs_init_with_ops(
    const meshpay_storage_nvs_init_ops_t *ops);
meshpay_storage_backend_t meshpay_storage_nvs_backend(void);

#ifdef __cplusplus
}
#endif
