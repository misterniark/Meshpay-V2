#include "test_pool.h"

#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "test_pool";

/* Slots alloués une fois au boot, jamais libérés (cf. test_pool.h). */
static meshpay_app_t *s_slots[TEST_POOL_SLOTS];

void test_pool_init(void)
{
    for (unsigned i = 0; i < TEST_POOL_SLOTS; ++i) {
        if (s_slots[i] != NULL) {
            continue; /* idempotent : déjà alloué */
        }
        /* malloc au tas vierge : ~58 Ko, garanti par la région contiguë de
         * 221 Ko disponible au démarrage. */
        s_slots[i] = malloc(sizeof(meshpay_app_t));
        if (s_slots[i] == NULL) {
            ESP_LOGE(TAG, "échec alloc slot %u (%u o) — pool de test indisponible",
                     i, (unsigned)sizeof(meshpay_app_t));
            abort();
        }
    }
    ESP_LOGI(TAG, "pool prêt : %u slots de %u o", (unsigned)TEST_POOL_SLOTS,
             (unsigned)sizeof(meshpay_app_t));
}

meshpay_app_t *test_pool_app(unsigned i)
{
    /* Garde-fou : un dépassement d'indice est une erreur de test, pas un cas à
     * masquer. */
    if (i >= TEST_POOL_SLOTS || s_slots[i] == NULL) {
        ESP_LOGE(TAG, "test_pool_app(%u) hors borne ou pool non initialisé", i);
        abort();
    }
    memset(s_slots[i], 0, sizeof(meshpay_app_t)); /* slot propre à chaque emprunt */
    return s_slots[i];
}

meshpay_dag_t *test_pool_dag(unsigned i)
{
    if (i >= TEST_POOL_SLOTS || s_slots[i] == NULL) {
        ESP_LOGE(TAG, "test_pool_dag(%u) hors borne ou pool non initialisé", i);
        abort();
    }
    meshpay_dag_t *d = &s_slots[i]->dag;
    memset(d, 0, sizeof(*d));
    meshpay_dag_init(d);
    return d;
}
