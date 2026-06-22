#include "meshpay/rns/rns_resource.h"
#include "unity.h"
#include <string.h>

static void make_active_link(rns_link_t *link)
{
    rns_link_clear(link);
    link->status = RNS_LINK_STATUS_ACTIVE;
    link->mtu = RNS_PACKET_MTU;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        link->link_id[i] = (uint8_t)(0x70 + i);
    }
}

static void fill_batch(uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        data[i] = (uint8_t)((i * 13u) ^ (i >> 2));
    }
}

TEST_CASE("rns resource transfers batch larger than mtu bit identically", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);

    uint8_t batch[900];
    fill_batch(batch, sizeof(batch));

    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(&link,
                                                          batch,
                                                          sizeof(batch),
                                                          packets,
                                                          RNS_RESOURCE_MAX_FRAGMENTS,
                                                          &packet_count));
    TEST_ASSERT_EQUAL_UINT32(3, packet_count);
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_TRUE(packets[i].data_len <= RNS_PACKET_MAX_DATA_SIZE);
        TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_RESOURCE, packets[i].context);
        TEST_ASSERT_EQUAL_MEMORY(link.link_id,
                                 packets[i].destination_hash,
                                 RNS_DESTINATION_HASH_SIZE);
    }

    rns_resource_reassembler_t reassembler;
    rns_resource_reassembler_init(&reassembler);
    uint8_t rebuilt[sizeof(batch)];
    size_t rebuilt_len = 0;
    bool complete = false;
    const size_t order[] = {2, 0, 1};
    for (size_t i = 0; i < packet_count; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_resource_reassembler_accept(&reassembler,
                                                          &packets[order[i]],
                                                          rebuilt,
                                                          sizeof(rebuilt),
                                                          &rebuilt_len,
                                                          &complete));
        if (i + 1 < packet_count) {
            TEST_ASSERT_FALSE(complete);
        }
    }

    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT32(sizeof(batch), rebuilt_len);
    TEST_ASSERT_EQUAL_MEMORY(batch, rebuilt, sizeof(batch));
}

TEST_CASE("rns resource rejects active link without link id", "[rns_resource]")
{
    rns_link_t link;
    rns_link_clear(&link);
    link.status = RNS_LINK_STATUS_ACTIVE;
    link.mtu = RNS_PACKET_MTU;
    link.mode = RNS_LINK_MODE_AES256_CBC;

    uint8_t batch[8];
    fill_batch(batch, sizeof(batch));
    rns_packet_t packet;
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_resource_create_packets(&link,
                                                  batch,
                                                  sizeof(batch),
                                                  &packet,
                                                  1,
                                                  &packet_count));
}

TEST_CASE("rns resource rejects corrupted checksum", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);

    uint8_t batch[600];
    fill_batch(batch, sizeof(batch));

    rns_packet_t packets[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t packet_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(&link,
                                                          batch,
                                                          sizeof(batch),
                                                          packets,
                                                          RNS_RESOURCE_MAX_FRAGMENTS,
                                                          &packet_count));
    packets[packet_count - 1].data[packets[packet_count - 1].data_len - 1] ^= 0x01;

    rns_resource_reassembler_t reassembler;
    rns_resource_reassembler_init(&reassembler);
    uint8_t rebuilt[sizeof(batch)];
    size_t rebuilt_len = 0;
    bool complete = false;
    for (size_t i = 0; i < packet_count - 1; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          rns_resource_reassembler_accept(&reassembler,
                                                          &packets[i],
                                                          rebuilt,
                                                          sizeof(rebuilt),
                                                          &rebuilt_len,
                                                          &complete));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      rns_resource_reassembler_accept(&reassembler,
                                                      &packets[packet_count - 1],
                                                      rebuilt,
                                                      sizeof(rebuilt),
                                                      &rebuilt_len,
                                                      &complete));
}


/* ------------------------------------------------------------------------- */
/* Pool de reassembleurs (fix non-convergence DAG sous fork)                  */
/* ------------------------------------------------------------------------- */

/* REGRESSION : deux Resource distincts, fragments STRICTEMENT entrelaces
 * (A0,B0,A1,B1,...). Avec un reassembleur unique, chaque alternance reinitialise
 * le reassemblage de l'autre -> aucun n'aboutit (bug observe au banc). Le pool
 * doit mener les DEUX a terme, identiques au contenu source. */
TEST_CASE("rns resource pool reassembles two interleaved resources", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);

    uint8_t batch_a[900];
    uint8_t batch_b[900];
    for (size_t i = 0; i < sizeof(batch_a); ++i) {
        batch_a[i] = (uint8_t)((i * 13u) ^ (i >> 2));
        batch_b[i] = (uint8_t)((i * 7u) + 0x5au);
    }

    rns_packet_t pa[RNS_RESOURCE_MAX_FRAGMENTS];
    rns_packet_t pb[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t na = 0, nb = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
        &link, batch_a, sizeof(batch_a), pa, RNS_RESOURCE_MAX_FRAGMENTS, &na));
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
        &link, batch_b, sizeof(batch_b), pb, RNS_RESOURCE_MAX_FRAGMENTS, &nb));
    TEST_ASSERT_TRUE(na >= 2 && nb >= 2);

    rns_resource_reassembler_pool_t pool;
    rns_resource_reassembler_pool_init(&pool);

    uint8_t out_a[900];
    uint8_t out_b[900];
    size_t la = 0, lb = 0;
    bool ca = false, cb = false;
    size_t maxn = na > nb ? na : nb;
    for (size_t i = 0; i < maxn; ++i) {
        if (i < na) {
            TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
                &pool, &pa[i], out_a, sizeof(out_a), &la, &ca));
        }
        if (i < nb) {
            TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
                &pool, &pb[i], out_b, sizeof(out_b), &lb, &cb));
        }
    }
    TEST_ASSERT_TRUE(ca);
    TEST_ASSERT_TRUE(cb);
    TEST_ASSERT_EQUAL_UINT32(sizeof(batch_a), la);
    TEST_ASSERT_EQUAL_UINT32(sizeof(batch_b), lb);
    TEST_ASSERT_EQUAL_MEMORY(batch_a, out_a, sizeof(batch_a));
    TEST_ASSERT_EQUAL_MEMORY(batch_b, out_b, sizeof(batch_b));
}

/* Un resource incomplet (un fragment manquant) ne doit pas empecher un autre
 * resource concurrent d'aboutir. */
TEST_CASE("rns resource pool isolates an incomplete resource", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);
    uint8_t batch_a[900];
    uint8_t batch_b[900];
    for (size_t i = 0; i < 900; ++i) {
        batch_a[i] = (uint8_t)(i + 1u);
        batch_b[i] = (uint8_t)((i * 3u) ^ 0xa5u);
    }
    rns_packet_t pa[RNS_RESOURCE_MAX_FRAGMENTS];
    rns_packet_t pb[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t na = 0, nb = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
        &link, batch_a, sizeof(batch_a), pa, RNS_RESOURCE_MAX_FRAGMENTS, &na));
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
        &link, batch_b, sizeof(batch_b), pb, RNS_RESOURCE_MAX_FRAGMENTS, &nb));
    TEST_ASSERT_TRUE(na >= 2 && nb >= 2);

    rns_resource_reassembler_pool_t pool;
    rns_resource_reassembler_pool_init(&pool);
    uint8_t out[900];
    size_t outl = 0;
    bool complete = false;

    for (size_t i = 0; i + 1 < na; ++i) { /* A : tout sauf le dernier fragment */
        TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
            &pool, &pa[i], out, sizeof(out), &outl, &complete));
        TEST_ASSERT_FALSE(complete);
    }
    uint8_t out_b[900];
    size_t lb = 0;
    bool cb = false;
    for (size_t i = 0; i < nb; ++i) {      /* B : complet malgre A en cours */
        TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
            &pool, &pb[i], out_b, sizeof(out_b), &lb, &cb));
    }
    TEST_ASSERT_TRUE(cb);
    TEST_ASSERT_EQUAL_MEMORY(batch_b, out_b, sizeof(batch_b));
}

/* Surcharge : plus de Resource concurrents que de slots. Aucun partiel ne doit
 * se declarer complet a tort, et un resource neuf envoye en entier doit aboutir
 * (pas de corruption d'etat du pool apres evictions). */
TEST_CASE("rns resource pool stays correct when resources exceed slots", "[rns_resource]")
{
    rns_link_t link;
    make_active_link(&link);
    rns_resource_reassembler_pool_t pool;
    rns_resource_reassembler_pool_init(&pool);
    uint8_t out[900];
    size_t outl = 0;
    bool complete = false;

    const size_t overload = RNS_RESOURCE_REASSEMBLER_POOL_SIZE + 2;
    for (size_t r = 0; r < overload; ++r) {
        uint8_t batch[900];
        for (size_t i = 0; i < sizeof(batch); ++i) {
            batch[i] = (uint8_t)((i * (r + 1u)) ^ (r * 31u));
        }
        rns_packet_t pkts[RNS_RESOURCE_MAX_FRAGMENTS];
        size_t n = 0;
        TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
            &link, batch, sizeof(batch), pkts, RNS_RESOURCE_MAX_FRAGMENTS, &n));
        TEST_ASSERT_TRUE(n >= 2);
        TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
            &pool, &pkts[0], out, sizeof(out), &outl, &complete));
        TEST_ASSERT_FALSE(complete); /* 1er fragment seul : jamais complet */
    }

    uint8_t full[900];
    for (size_t i = 0; i < sizeof(full); ++i) {
        full[i] = (uint8_t)((i ^ 0x3c) + 9u);
    }
    rns_packet_t fp[RNS_RESOURCE_MAX_FRAGMENTS];
    size_t fn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, rns_resource_create_packets(
        &link, full, sizeof(full), fp, RNS_RESOURCE_MAX_FRAGMENTS, &fn));
    uint8_t out_f[900];
    size_t lf = 0;
    bool cf = false;
    for (size_t i = 0; i < fn; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, rns_resource_reassembler_pool_accept(
            &pool, &fp[i], out_f, sizeof(out_f), &lf, &cf));
    }
    TEST_ASSERT_TRUE(cf);
    TEST_ASSERT_EQUAL_MEMORY(full, out_f, sizeof(full));
}
