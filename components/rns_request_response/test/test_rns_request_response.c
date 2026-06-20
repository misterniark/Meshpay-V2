#include "meshpay/rns/rns_request_response.h"
#include "unity.h"
#include <string.h>

static void make_active_link(rns_link_t *link)
{
    rns_link_clear(link);
    link->status = RNS_LINK_STATUS_ACTIVE;
    link->mtu = RNS_PACKET_MTU;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        link->link_id[i] = (uint8_t)(0x30 + i);
    }
}

TEST_CASE("rns request response correlates matching response", "[rns_request_response]")
{
    rns_link_t link;
    make_active_link(&link);

    const uint8_t request_data[] = "need-dag-summary";
    rns_packet_t request_packet;
    rns_request_receipt_t receipt;
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_create(&link,
                                                 "/meshpay/dag/summary",
                                                 request_data,
                                                 sizeof(request_data) - 1,
                                                 1000,
                                                 5000,
                                                 &request_packet,
                                                 &receipt));
    TEST_ASSERT_TRUE(receipt.active);
    TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_REQUEST, request_packet.context);
    TEST_ASSERT_EQUAL_MEMORY(link.link_id,
                             request_packet.destination_hash,
                             RNS_DESTINATION_HASH_SIZE);

    rns_request_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_decode(&request_packet, &decoded));
    TEST_ASSERT_EQUAL_UINT64(1000, decoded.requested_at_ms);
    TEST_ASSERT_EQUAL_UINT32(sizeof(request_data) - 1, decoded.data_len);
    TEST_ASSERT_EQUAL_MEMORY(request_data, decoded.data, decoded.data_len);
    TEST_ASSERT_EQUAL_MEMORY(receipt.request_id, decoded.request_id, RNS_REQUEST_ID_SIZE);

    const uint8_t response_data[] = "summary-ok";
    rns_packet_t response_packet;
    TEST_ASSERT_EQUAL(ESP_OK, rns_response_create_for_request(&link,
                                                              &request_packet,
                                                              response_data,
                                                              sizeof(response_data) - 1,
                                                              &response_packet));
    TEST_ASSERT_EQUAL(RNS_PACKET_CONTEXT_RESPONSE, response_packet.context);
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_receipt_accept_response(&receipt,
                                                                  &response_packet,
                                                                  1100));
    TEST_ASSERT_EQUAL(RNS_REQUEST_RECEIPT_COMPLETE, receipt.status);
    TEST_ASSERT_FALSE(receipt.active);
    TEST_ASSERT_EQUAL_UINT32(sizeof(response_data) - 1, receipt.response_len);
    TEST_ASSERT_EQUAL_MEMORY(response_data, receipt.response, receipt.response_len);
}

TEST_CASE("rns request response rejects active link without link id",
          "[rns_request_response]")
{
    rns_link_t link;
    rns_link_clear(&link);
    link.status = RNS_LINK_STATUS_ACTIVE;
    link.mtu = RNS_PACKET_MTU;
    link.mode = RNS_LINK_MODE_AES256_CBC;

    rns_packet_t packet;
    rns_request_receipt_t receipt;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rns_request_create(&link,
                                         "/meshpay/dag/summary",
                                         NULL,
                                         0,
                                         1000,
                                         5000,
                                         &packet,
                                         &receipt));
}

TEST_CASE("rns request response rejects wrong correlation", "[rns_request_response]")
{
    rns_link_t link;
    make_active_link(&link);

    rns_packet_t request_a;
    rns_packet_t request_b;
    rns_request_receipt_t receipt;
    const uint8_t data_a[] = "a";
    const uint8_t data_b[] = "b";
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_create(&link,
                                                 "/meshpay/a",
                                                 data_a,
                                                 sizeof(data_a) - 1,
                                                 1000,
                                                 5000,
                                                 &request_a,
                                                 &receipt));
    rns_request_receipt_t ignored;
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_create(&link,
                                                 "/meshpay/b",
                                                 data_b,
                                                 sizeof(data_b) - 1,
                                                 1001,
                                                 5000,
                                                 &request_b,
                                                 &ignored));

    rns_packet_t wrong_response;
    const uint8_t response_data[] = "wrong";
    TEST_ASSERT_EQUAL(ESP_OK, rns_response_create_for_request(&link,
                                                              &request_b,
                                                              response_data,
                                                              sizeof(response_data) - 1,
                                                              &wrong_response));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      rns_request_receipt_accept_response(&receipt,
                                                          &wrong_response,
                                                          1100));
    TEST_ASSERT_EQUAL(RNS_REQUEST_RECEIPT_PENDING, receipt.status);
    TEST_ASSERT_TRUE(receipt.active);
}

TEST_CASE("rns request response times out pending receipt", "[rns_request_response]")
{
    rns_link_t link;
    make_active_link(&link);

    rns_packet_t request_packet;
    rns_request_receipt_t receipt;
    const uint8_t request_data[] = "slow";
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_create(&link,
                                                 "/meshpay/slow",
                                                 request_data,
                                                 sizeof(request_data) - 1,
                                                 2000,
                                                 50,
                                                 &request_packet,
                                                 &receipt));

    TEST_ASSERT_EQUAL(ESP_OK, rns_request_receipt_check_timeout(&receipt, 2049));
    TEST_ASSERT_EQUAL(RNS_REQUEST_RECEIPT_PENDING, receipt.status);
    TEST_ASSERT_EQUAL(ESP_OK, rns_request_receipt_check_timeout(&receipt, 1999));
    TEST_ASSERT_EQUAL(RNS_REQUEST_RECEIPT_PENDING, receipt.status);
    TEST_ASSERT_TRUE(receipt.active);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, rns_request_receipt_check_timeout(&receipt, 2050));
    TEST_ASSERT_EQUAL(RNS_REQUEST_RECEIPT_TIMEOUT, receipt.status);
    TEST_ASSERT_FALSE(receipt.active);
}
