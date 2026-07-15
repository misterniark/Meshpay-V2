#include "meshpay/storage.h"
#include "nvs_flash.h"
#include "unity.h"
#include <string.h>

static void fill_sequence(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(start + i);
    }
}

TEST_CASE("meshpay storage save load is idempotent with mock backend", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);

    uint8_t identity_private[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(identity_private, sizeof(identity_private), 0x10);
    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    fill_sequence(pin_hash, sizeof(pin_hash), 0x80);
    uint8_t checkpoint[64];
    fill_sequence(checkpoint, sizeof(checkpoint), 0xc0);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_identity(&record,
                                                                  identity_private));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&record, "Alice"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_pin_hash(&record, pin_hash));
    record.next_seq = 42;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_checkpoint(&record,
                                                                    7,
                                                                    checkpoint,
                                                                    sizeof(checkpoint)));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_count);

    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_EQUAL_UINT32(1, mock.read_count);
    TEST_ASSERT_EQUAL_UINT32(MESHPAY_STORAGE_MAGIC, loaded.magic);
    TEST_ASSERT_EQUAL_UINT16(MESHPAY_STORAGE_VERSION, loaded.version);
    TEST_ASSERT_TRUE(loaded.has_identity);
    TEST_ASSERT_TRUE(loaded.has_pin_hash);
    TEST_ASSERT_TRUE(loaded.has_checkpoint);
    TEST_ASSERT_EQUAL_STRING("Alice", loaded.alias);
    TEST_ASSERT_EQUAL_UINT32(42, loaded.next_seq);
    TEST_ASSERT_EQUAL_UINT32(7, loaded.checkpoint_seq);
    TEST_ASSERT_EQUAL_UINT32(sizeof(checkpoint), loaded.checkpoint_len);
    TEST_ASSERT_EQUAL_MEMORY(identity_private, loaded.identity_private,
                             sizeof(identity_private));
    TEST_ASSERT_EQUAL_MEMORY(pin_hash, loaded.pin_hash, sizeof(pin_hash));
    TEST_ASSERT_EQUAL_MEMORY(checkpoint, loaded.checkpoint, sizeof(checkpoint));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &loaded));
    meshpay_storage_record_t loaded_again;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded_again));
    TEST_ASSERT_EQUAL_MEMORY(&loaded, &loaded_again, sizeof(loaded));
}

TEST_CASE("meshpay storage detects corrupted checkpoint and erase clears mock", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t checkpoint[16];
    fill_sequence(checkpoint, sizeof(checkpoint), 0x22);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_checkpoint(&record,
                                                                    1,
                                                                    checkpoint,
                                                                    sizeof(checkpoint)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));

    /* v3 : le checkpoint (seule clé émise ici) termine le blob CBOR — flipper
     * le dernier octet casse son hash sans casser la forme CBOR. */
    mock.blob[mock.blob_len - 1] ^= 0x01;
    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, meshpay_storage_load(&backend, &loaded));

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_erase(&backend));
    TEST_ASSERT_EQUAL_UINT32(1, mock.erase_count);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, meshpay_storage_load(&backend, &loaded));
}

TEST_CASE("meshpay storage rejects inconsistent persisted flags", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    record.has_identity = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_storage_save(&backend, &record));

    meshpay_storage_record_init(&record);
    record.has_pin_hash = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_storage_save(&backend, &record));

    meshpay_storage_record_init(&record);
    record.has_checkpoint = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_storage_save(&backend, &record));

    uint8_t zero_identity[RNS_IDENTITY_PRIVATE_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_storage_record_set_identity(&record,
                                                          zero_identity));
    uint8_t half_zero_identity[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(half_zero_identity, sizeof(half_zero_identity), 0x44);
    memset(half_zero_identity, 0, RNS_CRYPTO_X25519_KEY_SIZE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_storage_record_set_identity(&record,
                                                          half_zero_identity));
    uint8_t zero_pin_hash[RNS_CRYPTO_SHA256_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_storage_record_set_pin_hash(&record,
                                                          zero_pin_hash));

    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    fill_sequence(pin_hash, sizeof(pin_hash), 0x30);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_record_set_pin_hash(&record,
                                                          pin_hash));
    record.has_checkpoint = false;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
}

TEST_CASE("meshpay storage nvs backend exposes blob operations", "[storage]")
{
    meshpay_storage_backend_t backend = meshpay_storage_nvs_backend();
    TEST_ASSERT_NOT_NULL(backend.write_blob);
    TEST_ASSERT_NOT_NULL(backend.read_blob);
    TEST_ASSERT_NOT_NULL(backend.erase);
    TEST_ASSERT_NOT_NULL(backend.ctx);
}

static esp_err_t s_nvs_init_sequence[3];
static size_t s_nvs_init_calls;
static size_t s_nvs_erase_calls;
static esp_err_t s_nvs_erase_result;

static void reset_nvs_init_probe(esp_err_t first,
                                 esp_err_t second,
                                 esp_err_t erase_result)
{
    s_nvs_init_sequence[0] = first;
    s_nvs_init_sequence[1] = second;
    s_nvs_init_sequence[2] = second;
    s_nvs_init_calls = 0;
    s_nvs_erase_calls = 0;
    s_nvs_erase_result = erase_result;
}

static esp_err_t probe_nvs_init(void)
{
    size_t index = s_nvs_init_calls;
    if (index >= sizeof(s_nvs_init_sequence) / sizeof(s_nvs_init_sequence[0])) {
        index = sizeof(s_nvs_init_sequence) / sizeof(s_nvs_init_sequence[0]) - 1U;
    }
    ++s_nvs_init_calls;
    return s_nvs_init_sequence[index];
}

static esp_err_t probe_nvs_erase(void)
{
    ++s_nvs_erase_calls;
    return s_nvs_erase_result;
}

TEST_CASE("meshpay storage nvs init recovers from reusable partition errors",
          "[storage]")
{
    const meshpay_storage_nvs_init_ops_t ops = {
        .init = probe_nvs_init,
        .erase = probe_nvs_erase,
    };

    reset_nvs_init_probe(ESP_OK, ESP_OK, ESP_OK);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_nvs_init_with_ops(&ops));
    TEST_ASSERT_EQUAL_UINT32(1, s_nvs_init_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_nvs_erase_calls);

    reset_nvs_init_probe(ESP_ERR_NVS_NO_FREE_PAGES, ESP_OK, ESP_OK);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_nvs_init_with_ops(&ops));
    TEST_ASSERT_EQUAL_UINT32(2, s_nvs_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_nvs_erase_calls);

    reset_nvs_init_probe(ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_OK, ESP_OK);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_nvs_init_with_ops(&ops));
    TEST_ASSERT_EQUAL_UINT32(2, s_nvs_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_nvs_erase_calls);

    reset_nvs_init_probe(ESP_ERR_NVS_NO_FREE_PAGES, ESP_OK, ESP_FAIL);
    TEST_ASSERT_EQUAL(ESP_FAIL, meshpay_storage_nvs_init_with_ops(&ops));
    TEST_ASSERT_EQUAL_UINT32(1, s_nvs_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_nvs_erase_calls);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_storage_nvs_init_with_ops(NULL));
}

/* --- Palier A4 : persistance du descripteur de monnaie (blob opaque) --- */

TEST_CASE("meshpay storage persists currency descriptor blob", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);

    uint8_t descriptor[200];
    fill_sequence(descriptor, sizeof(descriptor), 0x05);
    TEST_ASSERT_EQUAL(ESP_OK,
        meshpay_storage_record_set_currency_descriptor(&record, descriptor,
                                                       sizeof(descriptor)));
    TEST_ASSERT_TRUE(record.has_currency_descriptor);
    TEST_ASSERT_EQUAL_UINT32(sizeof(descriptor), record.currency_descriptor_len);

    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_EQUAL_UINT16(MESHPAY_STORAGE_VERSION, loaded.version);
    TEST_ASSERT_TRUE(loaded.has_currency_descriptor);
    TEST_ASSERT_EQUAL_UINT32(sizeof(descriptor),
                             loaded.currency_descriptor_len);
    TEST_ASSERT_EQUAL_MEMORY(descriptor, loaded.currency_descriptor,
                             sizeof(descriptor));
}

TEST_CASE("meshpay storage record without descriptor loads as absent", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&record, "Bob"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));

    meshpay_storage_record_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_load(&backend, &loaded));
    TEST_ASSERT_FALSE(loaded.has_currency_descriptor);
    TEST_ASSERT_EQUAL_UINT32(0, loaded.currency_descriptor_len);
}

/* --- Chantier migration NVS (M1) : motifs de load + backup --- */

/* Fabrique dans le mock un blob brut arbitraire sous la clé record : préfixe
 * magic+version little-endian, corps rempli d'un motif, taille au choix.
 * C'est la seule façon de simuler les schémas étrangers (v1, downgrade, ABI
 * différent) sans posséder leurs structs. */
static void mock_plant_raw_blob(meshpay_storage_mock_t *mock,
                                uint32_t magic,
                                uint16_t version,
                                size_t total_len)
{
    TEST_ASSERT_TRUE(total_len <= sizeof(mock->blob));
    memset(mock->blob, 0xA5, total_len);
    mock->blob[0] = (uint8_t)(magic & 0xFF);
    mock->blob[1] = (uint8_t)((magic >> 8) & 0xFF);
    mock->blob[2] = (uint8_t)((magic >> 16) & 0xFF);
    mock->blob[3] = (uint8_t)((magic >> 24) & 0xFF);
    mock->blob[4] = (uint8_t)(version & 0xFF);
    mock->blob[5] = (uint8_t)((version >> 8) & 0xFF);
    mock->blob_len = total_len;
    mock->present = true;
}

TEST_CASE("meshpay storage load_ex qualifies empty and current record", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    /* Mock vierge : premier boot -> EMPTY. */
    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_EMPTY, probe);

    /* Record courant sauvé -> OK, contenu intact. */
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&record, "Carol"));
    record.next_seq = 9;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_STRING("Carol", loaded.alias);
    TEST_ASSERT_EQUAL_UINT32(9, loaded.next_seq);
}

TEST_CASE("meshpay storage load_ex qualifies legacy schemas", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;

    /* v1 d'époque : préfixe sain, version 1, blob PLUS PETIT (le cas réel du
     * Palier E). Jamais chargé, jamais confondu avec un record neuf. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, 1,
                        sizeof(meshpay_storage_record_t) - 384);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_LEGACY, probe);

    /* Downgrade : version FUTURE à taille plausible -> LEGACY aussi (le
     * firmware ancien ne doit ni charger ni écraser un record plus neuf). */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC,
                        MESHPAY_STORAGE_VERSION + 7,
                        sizeof(meshpay_storage_record_t));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_LEGACY, probe);

    /* v2-struct réelle (la flotte d'avant ce chantier) : version 2, taille
     * de l'ancienne struct -> LEGACY, migrable au M3. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, 2,
                        sizeof(meshpay_storage_record_t));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_LEGACY, probe);
}

TEST_CASE("meshpay storage load_ex qualifies corrupt blobs", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;

    /* Magic inconnu -> CORRUPT. */
    mock_plant_raw_blob(&mock, 0xDEADBEEF, MESHPAY_STORAGE_VERSION,
                        sizeof(meshpay_storage_record_t));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Plus court que le préfixe : indéchiffrable -> CORRUPT. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, MESHPAY_STORAGE_VERSION, 6);
    mock.blob_len = 4;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Version courante mais corps qui n'est pas du CBOR : préfixe sain, blob
     * inexploitable -> CORRUPT (plus de notion de « bonne taille » en v3). */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, MESHPAY_STORAGE_VERSION,
                        160);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Schéma courant mais invariants violés (checkpoint altéré) : le code de
     * la vérification remonte, motif CORRUPT. Le checkpoint — seule clé
     * émise — termine le blob : flipper le dernier octet casse son hash. */
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    uint8_t checkpoint[16];
    fill_sequence(checkpoint, sizeof(checkpoint), 0x66);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_checkpoint(
                                  &record, 3, checkpoint, sizeof(checkpoint)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    mock.blob[mock.blob_len - 1] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);
}

TEST_CASE("meshpay storage archive copies once and never overwrites", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    /* Rien à archiver sur un mock vierge. */
    bool archived = true;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_storage_archive(&backend, &archived));
    TEST_ASSERT_FALSE(archived);

    /* Un blob legacy planté est archivé TEL QUEL, sans interprétation. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, 1, 640);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_archive(&backend, &archived));
    TEST_ASSERT_TRUE(archived);
    TEST_ASSERT_TRUE(mock.bak_present);
    TEST_ASSERT_EQUAL_UINT32(640, mock.bak_blob_len);
    TEST_ASSERT_EQUAL_MEMORY(mock.blob, mock.bak_blob, 640);
    /* Le record source est INTACT (l'archive ne modifie jamais l'original). */
    TEST_ASSERT_TRUE(mock.present);
    TEST_ASSERT_EQUAL_UINT32(640, mock.blob_len);

    /* Le record change (autre schéma), on re-archive : le PREMIER témoin est
     * conservé, jamais écrasé. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, 9, 720);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_archive(&backend, &archived));
    TEST_ASSERT_FALSE(archived);
    TEST_ASSERT_EQUAL_UINT32(640, mock.bak_blob_len);
    TEST_ASSERT_EQUAL_UINT32(1, mock.bak_write_count);

    /* L'effacement du record (bouton « réinitialiser », M4) NE TOUCHE PAS le
     * backup : le geste reste réversible. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_erase(&backend));
    TEST_ASSERT_FALSE(mock.present);
    TEST_ASSERT_TRUE(mock.bak_present);
    TEST_ASSERT_EQUAL_UINT32(640, mock.bak_blob_len);
}

/* --- Chantier migration NVS (M2) : format CBOR v3, taille variable --- */

/* Écrit le préfixe magic+version courant en tête du blob du mock ; le corps
 * CBOR est fourni octet à octet par le test (petites valeurs -> encodage
 * direct, lisible en hexa). */
static void mock_plant_v3_body(meshpay_storage_mock_t *mock,
                               const uint8_t *body,
                               size_t body_len)
{
    mock_plant_raw_blob(mock, MESHPAY_STORAGE_MAGIC, MESHPAY_STORAGE_VERSION,
                        MESHPAY_STORAGE_PREFIX_SIZE + body_len);
    memcpy(mock->blob + MESHPAY_STORAGE_PREFIX_SIZE, body, body_len);
}

TEST_CASE("meshpay storage v3 blob size tracks content not struct", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    /* Record minimal (un alias) : le blob doit être MINUSCULE — c'est la fin
     * de la fragilité sizeof (un v1 « plus petit » redevient lisible demain). */
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&record, "Al"));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &record));
    TEST_ASSERT_TRUE(mock.blob_len < 32);
    TEST_ASSERT_TRUE(mock.blob_len != sizeof(meshpay_storage_record_t));

    /* Le même record enrichi grossit le blob SANS invalider l'ancien format :
     * on relit chaque état sans erreur. */
    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_STRING("Al", loaded.alias);

    uint8_t descriptor[128];
    fill_sequence(descriptor, sizeof(descriptor), 0x31);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_currency_descriptor(
                                  &loaded, descriptor, sizeof(descriptor)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_save(&backend, &loaded));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_STRING("Al", loaded.alias);
    TEST_ASSERT_EQUAL_MEMORY(descriptor, loaded.currency_descriptor,
                             sizeof(descriptor));
}

TEST_CASE("meshpay storage v3 ignores unknown keys (forward tolerant)", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    /* Record « v4 simulé » écrit à la main : alias + next_seq + DEUX champs
     * inconnus (42: uint, 43: bstr(3)) intercalés. Un lecteur v3 doit relire
     * les champs connus et ignorer le reste — la promesse anti-brick. */
    static const uint8_t body[] = {
        0xA4,             /* map(4) */
        0x02, 0x63, 'Z', 'o', 'e',        /* 2: tstr(3) "Zoe" */
        0x18, 0x2A, 0x05,                 /* 42: uint 5 (clé inconnue) */
        0x04, 0x07,                       /* 4: next_seq = 7 */
        0x18, 0x2B, 0x43, 0x01, 0x02, 0x03 /* 43: bstr(3) (clé inconnue) */
    };
    mock_plant_v3_body(&mock, body, sizeof(body));

    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_STRING("Zoe", loaded.alias);
    TEST_ASSERT_EQUAL_UINT32(7, loaded.next_seq);
    TEST_ASSERT_FALSE(loaded.has_identity);
    TEST_ASSERT_FALSE(loaded.has_pin_hash);
    TEST_ASSERT_FALSE(loaded.has_checkpoint);
    TEST_ASSERT_FALSE(loaded.has_currency_descriptor);
}

TEST_CASE("meshpay storage v3 rejects malformed bodies", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t loaded;
    meshpay_storage_probe_t probe;

    /* Doublon d'une clé CONNUE : ambigu, refusé. */
    static const uint8_t dup[] = {
        0xA2,
        0x04, 0x07,   /* 4: next_seq = 7 */
        0x04, 0x08,   /* 4: next_seq = 8 (doublon) */
    };
    mock_plant_v3_body(&mock, dup, sizeof(dup));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Octets orphelins après la map : blob suspect, refusé. */
    static const uint8_t trailing[] = {
        0xA1, 0x04, 0x07,   /* map(1) 4:7 */
        0xFF,               /* orphelin */
    };
    mock_plant_v3_body(&mock, trailing, sizeof(trailing));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Alias avec NUL embarqué : casserait la C-string, refusé. */
    static const uint8_t embedded_nul[] = {
        0xA1, 0x02, 0x63, 'A', 0x00, 'B',   /* 2: tstr(3) "A\0B" */
    };
    mock_plant_v3_body(&mock, embedded_nul, sizeof(embedded_nul));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);

    /* Checkpoint sans son hash (clé 7 sans clé 6) : l'intégrité ne peut pas
     * être établie -> INVALID_CRC via les invariants. */
    static const uint8_t chk_no_hash[] = {
        0xA1, 0x07, 0x44, 0x10, 0x11, 0x12, 0x13,   /* 7: bstr(4) */
    };
    mock_plant_v3_body(&mock, chk_no_hash, sizeof(chk_no_hash));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_storage_load_ex(&backend, &loaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);
}

/* --- Chantier migration NVS (M3) : lecteur legacy v2 + migration --- */

/* Fabrique un blob v2-struct AUTHENTIQUE : la struct RAM courante est
 * layout-identique au gel v2 (static_assert dans storage.c), donc écrire la
 * struct brute avec version=2 reproduit exactement ce que la flotte
 * persistait avant le format CBOR. */
static void mock_plant_v2_record(meshpay_storage_backend_t *backend,
                                 meshpay_storage_record_t *v2_out)
{
    meshpay_storage_record_t rec;
    meshpay_storage_record_init(&rec);

    uint8_t identity_private[RNS_IDENTITY_PRIVATE_SIZE];
    fill_sequence(identity_private, sizeof(identity_private), 0x51);
    uint8_t pin_hash[RNS_CRYPTO_SHA256_SIZE];
    fill_sequence(pin_hash, sizeof(pin_hash), 0x91);
    uint8_t checkpoint[48];
    fill_sequence(checkpoint, sizeof(checkpoint), 0xD1);
    uint8_t descriptor[96];
    fill_sequence(descriptor, sizeof(descriptor), 0x71);

    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_record_set_identity(&rec,
                                                          identity_private));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_alias(&rec, "Dave"));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_record_set_pin_hash(&rec, pin_hash));
    rec.next_seq = 17;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_checkpoint(
                                  &rec, 4, checkpoint, sizeof(checkpoint)));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_record_set_currency_descriptor(
                                  &rec, descriptor, sizeof(descriptor)));

    rec.version = 2; /* wire v2 : struct brute, version 2 au préfixe */
    TEST_ASSERT_EQUAL(ESP_OK,
                      backend->write_blob(backend->ctx,
                                          MESHPAY_STORAGE_STATE_KEY,
                                          &rec, sizeof(rec)));
    if (v2_out != NULL) {
        memcpy(v2_out, &rec, sizeof(*v2_out));
    }
}

TEST_CASE("meshpay storage migrates a real v2 record intact", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t v2;
    mock_plant_v2_record(&backend, &v2);
    TEST_ASSERT_EQUAL_UINT32(sizeof(meshpay_storage_record_t), mock.blob_len);

    /* Migration : contenu INTACT champ à champ, témoin v2 archivé tel quel. */
    meshpay_storage_record_t migrated_rec;
    bool migrated = false;
    meshpay_storage_probe_t probe;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_migrate(&backend, &migrated_rec,
                                                      &migrated, &probe));
    TEST_ASSERT_TRUE(migrated);
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_MEMORY(v2.identity_private,
                             migrated_rec.identity_private,
                             sizeof(v2.identity_private));
    TEST_ASSERT_EQUAL_STRING("Dave", migrated_rec.alias);
    TEST_ASSERT_EQUAL_MEMORY(v2.pin_hash, migrated_rec.pin_hash,
                             sizeof(v2.pin_hash));
    TEST_ASSERT_EQUAL_UINT32(17, migrated_rec.next_seq);
    TEST_ASSERT_EQUAL_UINT32(4, migrated_rec.checkpoint_seq);
    TEST_ASSERT_EQUAL_UINT32(v2.checkpoint_len, migrated_rec.checkpoint_len);
    TEST_ASSERT_EQUAL_MEMORY(v2.checkpoint, migrated_rec.checkpoint,
                             v2.checkpoint_len);
    TEST_ASSERT_EQUAL_UINT32(v2.currency_descriptor_len,
                             migrated_rec.currency_descriptor_len);
    TEST_ASSERT_EQUAL_MEMORY(v2.currency_descriptor,
                             migrated_rec.currency_descriptor,
                             v2.currency_descriptor_len);

    /* Le backup EST le blob v2 original (préfixe version 2 compris). */
    TEST_ASSERT_TRUE(mock.bak_present);
    TEST_ASSERT_EQUAL_UINT32(sizeof(meshpay_storage_record_t),
                             mock.bak_blob_len);
    TEST_ASSERT_EQUAL_MEMORY(&v2, mock.bak_blob, sizeof(v2));

    /* Le record persiste maintenant en v3 : relecture directe OK. */
    meshpay_storage_record_t reloaded;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_storage_load_ex(&backend, &reloaded, &probe));
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_OK, probe);
    TEST_ASSERT_EQUAL_UINT16(MESHPAY_STORAGE_VERSION, reloaded.version);
    TEST_ASSERT_EQUAL_STRING("Dave", reloaded.alias);

    /* Idempotence : le boot suivant charge sans re-migrer. */
    migrated = true;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_storage_migrate(&backend, &migrated_rec,
                                                      &migrated, &probe));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL_UINT32(1, mock.bak_write_count);
}

TEST_CASE("meshpay storage migrate archives what it cannot read", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t rec;
    bool migrated = true;
    meshpay_storage_probe_t probe;

    /* Vierge : premier boot, rien à archiver. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      meshpay_storage_migrate(&backend, &rec, &migrated,
                                              &probe));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_EMPTY, probe);
    TEST_ASSERT_FALSE(mock.bak_present);

    /* v1 (pas de lecteur) : archivé, laissé EN PLACE, INVALID_VERSION. */
    mock_plant_raw_blob(&mock, MESHPAY_STORAGE_MAGIC, 1, 704);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION,
                      meshpay_storage_migrate(&backend, &rec, &migrated,
                                              &probe));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_LEGACY, probe);
    TEST_ASSERT_TRUE(mock.bak_present);
    TEST_ASSERT_EQUAL_UINT32(704, mock.bak_blob_len);
    TEST_ASSERT_TRUE(mock.present);
    TEST_ASSERT_EQUAL_UINT32(704, mock.blob_len);

    /* Corrompu (magic inconnu) : archivé au premier passage seulement (le
     * témoin v1 déjà en backup n'est jamais écrasé), erreur remontée. */
    mock_plant_raw_blob(&mock, 0xBADC0FFE, 3, 128);
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_storage_migrate(&backend, &rec, &migrated,
                                                  &probe));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);
    TEST_ASSERT_EQUAL_UINT32(704, mock.bak_blob_len);
    TEST_ASSERT_EQUAL_UINT32(1, mock.bak_write_count);
}

TEST_CASE("meshpay storage migrate refuses inconsistent v2 content", "[storage]")
{
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);

    meshpay_storage_record_t v2;
    mock_plant_v2_record(&backend, &v2);
    /* Casse le hash du checkpoint DANS le blob v2 : contenu incohérent. */
    meshpay_storage_record_t *stored = (meshpay_storage_record_t *)mock.blob;
    stored->checkpoint[0] ^= 0x01;

    meshpay_storage_record_t rec;
    bool migrated = true;
    meshpay_storage_probe_t probe;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC,
                      meshpay_storage_migrate(&backend, &rec, &migrated,
                                              &probe));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL(MESHPAY_STORAGE_PROBE_CORRUPT, probe);
    /* Témoin archivé, record d'origine PAS remplacé (toujours version 2). */
    TEST_ASSERT_TRUE(mock.bak_present);
    TEST_ASSERT_TRUE(mock.present);
    TEST_ASSERT_EQUAL_UINT32(sizeof(meshpay_storage_record_t), mock.blob_len);
}

TEST_CASE("meshpay storage v2 wire size is frozen", "[storage]")
{
    /* Documentation vivante du gel v2 : la taille observée sur cible est
     * celle que migrate exige d'un blob v2. Si la struct RAM évolue un jour,
     * le static_assert de storage.c casse la compilation AVANT que ce test ne
     * puisse mentir. La valeur loggée sert au doc du chantier. */
    printf("v2 wire size on target: %u bytes\n",
           (unsigned)sizeof(meshpay_storage_record_t));
    TEST_ASSERT_TRUE(sizeof(meshpay_storage_record_t) <
                     MESHPAY_STORAGE_BLOB_MAX);
    TEST_ASSERT_EQUAL_UINT32(0, sizeof(meshpay_storage_record_t) % 4);
}

TEST_CASE("meshpay storage rejects invalid currency descriptor", "[storage]")
{
    meshpay_storage_record_t record;
    meshpay_storage_record_init(&record);

    uint8_t descriptor[8];
    fill_sequence(descriptor, sizeof(descriptor), 0x09);
    /* NULL / longueur nulle / surdimensionné -> rejet avant toute copie. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        meshpay_storage_record_set_currency_descriptor(&record, NULL, 4));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
        meshpay_storage_record_set_currency_descriptor(&record, descriptor, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
        meshpay_storage_record_set_currency_descriptor(
            &record, descriptor, MESHPAY_STORAGE_DESCRIPTOR_MAX + 1));

    /* Flag présent mais longueur 0 -> incohérent -> rejeté à la sauvegarde. */
    meshpay_storage_mock_t mock;
    meshpay_storage_mock_init(&mock);
    meshpay_storage_backend_t backend = meshpay_storage_mock_backend(&mock);
    meshpay_storage_record_init(&record);
    record.has_currency_descriptor = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_storage_save(&backend, &record));
}
