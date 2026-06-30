#pragma once

#include "meshpay/dag.h"
#include "meshpay/app_main_logic.h"

/*
 * Pool partagé de structures volumineuses pour les tests on-device.
 *
 * meshpay_dag_t (~56 Ko, 250 transactions) et meshpay_app_t (~58 Ko, contient
 * une DAG) sont :
 *   - trop grosses pour la pile du main task (16 Ko) -> débordement -> crash ;
 *   - trop coûteuses en allocation PAR TEST sur le tas : la région heap contiguë
 *     ne fait que 221 Ko et se fragmente au fil des suites ; une calloc(56 Ko)
 *     finit par échouer (NULL), et le longjmp d'Unity sur l'assertion d'échec
 *     saute les free() -> fuite en cascade qui affame les suites suivantes.
 *
 * Solution : on alloue UNE SEULE FOIS, au tout début de app_main() (tas encore
 * vierge, région de 221 Ko contiguë), un petit pool de slots réutilisés d'un
 * test à l'autre. Jamais libéré -> jamais fragmenté, jamais de fuite, état
 * déterministe (chaque emprunt remet le slot à zéro).
 *
 * 2 slots suffisent : aucun test n'a besoin de plus de 2 de ces structures
 * simultanément. Un meshpay_app_t contenant un meshpay_dag_t, les tests DAG
 * empruntent &slot.dag et les tests « app » empruntent le slot entier.
 */

#define TEST_POOL_SLOTS 2

/* Alloue le pool. À appeler une fois au tout début de app_main(), avant tout
 * test. Abort (avec log) si l'allocation échoue : les tests ne peuvent pas
 * tourner sans le pool. */
void test_pool_init(void);

/* Emprunte le slot i (0..TEST_POOL_SLOTS-1) comme meshpay_app_t remis à zéro.
 * Le test appelle ensuite sa propre fonction d'init/setup, comme avant. */
meshpay_app_t *test_pool_app(unsigned i);

/* Emprunte la DAG du slot i, remise à zéro puis initialisée (meshpay_dag_init).
 * Équivalent direct de l'ancien motif `meshpay_dag_t dag; meshpay_dag_init(&dag);`. */
meshpay_dag_t *test_pool_dag(unsigned i);
