/*
 * Backend esp_partition pour dag_store (firmware). Mappe les E/S brutes du
 * composant sur une partition flash dédiée (type data). La partition est
 * marquée `encrypted` dans la table : avec la flash encryption (obligatoire sur
 * S3), le contenu est chiffré au repos de façon transparente — aucune crypto
 * applicative ici.
 */
#include "meshpay/dag_store.h"

#include "esp_partition.h"

static esp_err_t part_read(void *ctx, size_t offset, void *buf, size_t len)
{
    return esp_partition_read((const esp_partition_t *)ctx, offset, buf, len);
}

static esp_err_t part_write(void *ctx, size_t offset, const void *data,
                            size_t len)
{
    return esp_partition_write((const esp_partition_t *)ctx, offset, data, len);
}

static esp_err_t part_erase(void *ctx, size_t offset, size_t len)
{
    return esp_partition_erase_range((const esp_partition_t *)ctx, offset, len);
}

esp_err_t meshpay_dag_store_partition_backend(const char *label,
                                              meshpay_dag_store_backend_t *out)
{
    if (label == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    out->read = part_read;
    out->write = part_write;
    out->erase = part_erase;
    out->size = part->size;
    out->erase_size = part->erase_size; /* secteur flash (typiquement 4096) */
    out->ctx = (void *)part;
    return ESP_OK;
}
