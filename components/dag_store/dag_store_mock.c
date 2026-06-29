/*
 * Backend mock RAM pour dag_store : simule une partition flash en mémoire.
 * - erase(offset,len) : met la zone à 0xFF (état flash effacé) ;
 * - write(offset,len) : copie les octets (pas de contrôle 1->0, suffisant ici) ;
 * - read(offset,len)  : copie les octets.
 * Toutes les opérations sont bornées à la taille du buffer.
 */
#include "meshpay/dag_store.h"

#include <string.h>

static esp_err_t mock_read(void *ctx, size_t offset, void *buf, size_t len)
{
    meshpay_dag_store_mock_t *m = (meshpay_dag_store_mock_t *)ctx;
    if (m == NULL || buf == NULL || offset + len > m->size) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, m->buf + offset, len);
    return ESP_OK;
}

static esp_err_t mock_write(void *ctx, size_t offset, const void *data,
                            size_t len)
{
    meshpay_dag_store_mock_t *m = (meshpay_dag_store_mock_t *)ctx;
    if (m == NULL || data == NULL || offset + len > m->size) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Contrainte flash encryption : offset ET longueur alignés sur 16 o.
     * Imposé ici pour que les tests détectent toute écriture non alignée. */
    if ((offset % 16U) != 0 || (len % 16U) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(m->buf + offset, data, len);
    m->write_count++;
    return ESP_OK;
}

static esp_err_t mock_erase(void *ctx, size_t offset, size_t len)
{
    meshpay_dag_store_mock_t *m = (meshpay_dag_store_mock_t *)ctx;
    if (m == NULL || offset + len > m->size) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Contrainte flash : effacement aligné sur le secteur. */
    if ((offset % m->erase_size) != 0 || (len % m->erase_size) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(m->buf + offset, 0xFF, len);
    m->erase_count++;
    return ESP_OK;
}

void meshpay_dag_store_mock_init(meshpay_dag_store_mock_t *mock,
                                 uint8_t *buf, size_t size)
{
    if (mock == NULL) {
        return;
    }
    mock->buf = buf;
    mock->size = size;
    mock->erase_size = 4096; /* secteur flash typique */
    mock->write_count = 0;
    mock->erase_count = 0;
    if (buf != NULL) {
        memset(buf, 0xFF, size); /* partition vierge = effacée */
    }
}

meshpay_dag_store_backend_t
meshpay_dag_store_mock_backend(meshpay_dag_store_mock_t *mock)
{
    meshpay_dag_store_backend_t backend = {0};
    backend.read = mock_read;
    backend.write = mock_write;
    backend.erase = mock_erase;
    backend.size = mock != NULL ? mock->size : 0;
    backend.erase_size = mock != NULL ? mock->erase_size : 4096;
    backend.ctx = mock;
    return backend;
}
