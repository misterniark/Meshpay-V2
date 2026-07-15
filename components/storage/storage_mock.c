#include "meshpay/storage.h"

#include <string.h>

/* Vue uniforme d'un slot du mock (record ou backup) : le mock route la clé
 * NVS vers l'un des deux jeux de champs de meshpay_storage_mock_t. Les champs
 * du record restent à plat dans la struct publique (compat des tests
 * existants qui inspectent mock.blob / mock.write_count). */
typedef struct {
    bool *present;
    uint8_t *blob;
    size_t blob_cap;
    size_t *blob_len;
    uint32_t *write_count;
    uint32_t *read_count;
    uint32_t *erase_count;
} mock_slot_view_t;

static bool mock_slot_for_key(meshpay_storage_mock_t *mock,
                              const char *key,
                              mock_slot_view_t *out)
{
    if (mock == NULL || key == NULL || out == NULL) {
        return false;
    }
    if (strcmp(key, MESHPAY_STORAGE_STATE_KEY) == 0) {
        *out = (mock_slot_view_t){
            .present = &mock->present,
            .blob = mock->blob,
            .blob_cap = sizeof(mock->blob),
            .blob_len = &mock->blob_len,
            .write_count = &mock->write_count,
            .read_count = &mock->read_count,
            .erase_count = &mock->erase_count,
        };
        return true;
    }
    if (strcmp(key, MESHPAY_STORAGE_BACKUP_KEY) == 0) {
        *out = (mock_slot_view_t){
            .present = &mock->bak_present,
            .blob = mock->bak_blob,
            .blob_cap = sizeof(mock->bak_blob),
            .blob_len = &mock->bak_blob_len,
            .write_count = &mock->bak_write_count,
            .read_count = &mock->bak_read_count,
            .erase_count = &mock->bak_erase_count,
        };
        return true;
    }
    /* Clé inconnue : le mock est strict pour attraper une régression de clé. */
    return false;
}

static esp_err_t mock_write_blob(void *ctx, const char *key, const void *data, size_t len)
{
    mock_slot_view_t slot;
    if (!mock_slot_for_key((meshpay_storage_mock_t *)ctx, key, &slot) ||
        data == NULL || len > slot.blob_cap) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(slot.blob, data, len);
    *slot.blob_len = len;
    *slot.present = true;
    (*slot.write_count)++;
    return ESP_OK;
}

static esp_err_t mock_read_blob(void *ctx, const char *key, void *data, size_t *len)
{
    mock_slot_view_t slot;
    if (!mock_slot_for_key((meshpay_storage_mock_t *)ctx, key, &slot) ||
        len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!*slot.present) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Sonde de taille (contrat backend, aligné sur nvs_get_blob) :
     * data == NULL -> retourne la taille stockée sans lecture. */
    if (data == NULL) {
        *len = *slot.blob_len;
        return ESP_OK;
    }
    if (*len < *slot.blob_len) {
        *len = *slot.blob_len;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(data, slot.blob, *slot.blob_len);
    *len = *slot.blob_len;
    (*slot.read_count)++;
    return ESP_OK;
}

static esp_err_t mock_erase(void *ctx, const char *key)
{
    mock_slot_view_t slot;
    if (!mock_slot_for_key((meshpay_storage_mock_t *)ctx, key, &slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(slot.blob, 0, slot.blob_cap);
    *slot.blob_len = 0;
    *slot.present = false;
    (*slot.erase_count)++;
    return ESP_OK;
}

void meshpay_storage_mock_init(meshpay_storage_mock_t *mock)
{
    if (mock == NULL) {
        return;
    }
    memset(mock, 0, sizeof(*mock));
}

meshpay_storage_backend_t meshpay_storage_mock_backend(meshpay_storage_mock_t *mock)
{
    meshpay_storage_backend_t backend = {
        .write_blob = mock_write_blob,
        .read_blob = mock_read_blob,
        .erase = mock_erase,
        .ctx = mock,
    };
    return backend;
}
