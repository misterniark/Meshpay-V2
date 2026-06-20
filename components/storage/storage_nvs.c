#include "meshpay/storage.h"

#include "nvs.h"
#include "nvs_flash.h"

static esp_err_t nvs_write_blob(void *ctx,
                                const char *key,
                                const void *data,
                                size_t len)
{
    const char *ns = (const char *)ctx;
    if (ns == NULL || key == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_read_blob(void *ctx,
                               const char *key,
                               void *data,
                               size_t *len)
{
    const char *ns = (const char *)ctx;
    if (ns == NULL || key == NULL || data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    err = nvs_get_blob(handle, key, data, len);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t nvs_erase_blob(void *ctx, const char *key)
{
    const char *ns = (const char *)ctx;
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t meshpay_storage_nvs_init(void)
{
    const meshpay_storage_nvs_init_ops_t ops = {
        .init = nvs_flash_init,
        .erase = nvs_flash_erase,
    };
    return meshpay_storage_nvs_init_with_ops(&ops);
}

esp_err_t meshpay_storage_nvs_init_with_ops(
    const meshpay_storage_nvs_init_ops_t *ops)
{
    if (ops == NULL || ops->init == NULL || ops->erase == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ops->init();
    if (err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }

    esp_err_t erase_err = ops->erase();
    if (erase_err != ESP_OK) {
        return erase_err;
    }
    return ops->init();
}

meshpay_storage_backend_t meshpay_storage_nvs_backend(void)
{
    return (meshpay_storage_backend_t){
        .write_blob = nvs_write_blob,
        .read_blob = nvs_read_blob,
        .erase = nvs_erase_blob,
        .ctx = (void *)MESHPAY_STORAGE_NVS_NAMESPACE,
    };
}
