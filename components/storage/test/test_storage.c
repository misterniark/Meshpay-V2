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

    meshpay_storage_record_t *stored = (meshpay_storage_record_t *)mock.blob;
    stored->checkpoint[0] ^= 0x01;
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
