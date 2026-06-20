#include "meshpay/storage.h"

#include <string.h>

static esp_err_t mock_write_blob(void *ctx, const char *key, const void *data, size_t len)
{
    (void)key;
    meshpay_storage_mock_t *mock = (meshpay_storage_mock_t *)ctx;
    if (mock == NULL || data == NULL || len > sizeof(mock->blob)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(mock->blob, data, len);
    mock->blob_len = len;
    mock->present = true;
    mock->write_count++;
    return ESP_OK;
}

static esp_err_t mock_read_blob(void *ctx, const char *key, void *data, size_t *len)
{
    (void)key;
    meshpay_storage_mock_t *mock = (meshpay_storage_mock_t *)ctx;
    if (mock == NULL || data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mock->present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (*len < mock->blob_len) {
        *len = mock->blob_len;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(data, mock->blob, mock->blob_len);
    *len = mock->blob_len;
    mock->read_count++;
    return ESP_OK;
}

static esp_err_t mock_erase(void *ctx, const char *key)
{
    (void)key;
    meshpay_storage_mock_t *mock = (meshpay_storage_mock_t *)ctx;
    if (mock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(mock->blob, 0, sizeof(mock->blob));
    mock->blob_len = 0;
    mock->present = false;
    mock->erase_count++;
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
