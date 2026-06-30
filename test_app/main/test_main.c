#include "unity.h"
#include "test_pool.h"

void app_main(void)
{
    /* Le pool de grosses structures (meshpay_dag_t / meshpay_app_t / runtimes)
     * doit être alloué AVANT tout test : les suites l'empruntent via
     * test_pool_dag()/test_pool_app() au lieu de placer ces ~28-58 Ko sur la
     * pile (débordement) ou de les calloc par test (fragmentation + fuite). */
    test_pool_init();
    unity_run_menu();
}
