#include "meshpay/rns/rns_request_response.h"

#include <string.h>

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)((value >> 8) & 0xff);
    out[1] = (uint8_t)(value & 0xff);
}

static uint16_t get_u16(const uint8_t *in)
{
    return ((uint16_t)in[0] << 8) | in[1];
}

static void put_u64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        out[i] = (uint8_t)((value >> (56 - (i * 8))) & 0xff);
    }
}

static uint64_t get_u64(const uint8_t *in)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

static bool link_is_active(const rns_link_t *link)
{
    if (link == NULL || link->status != RNS_LINK_STATUS_ACTIVE) {
        return false;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < RNS_DESTINATION_HASH_SIZE; ++i) {
        acc |= link->link_id[i];
    }
    return acc != 0;
}

static bool packet_is_for_link(const rns_link_t *link, const rns_packet_t *packet)
{
    return link != NULL && packet != NULL &&
           packet->destination_type == RNS_DESTINATION_TYPE_LINK &&
           rns_destination_hash_equal(link->link_id, packet->destination_hash);
}

esp_err_t rns_request_path_hash(const char *path,
                                uint8_t out[RNS_REQUEST_PATH_HASH_SIZE])
{
    if (path == NULL || out == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    esp_err_t err = rns_crypto_sha256((const uint8_t *)path, strlen(path), full_hash);
    if (err == ESP_OK) {
        memcpy(out, full_hash, RNS_REQUEST_PATH_HASH_SIZE);
    }
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return err;
}

esp_err_t rns_request_create(const rns_link_t *link,
                             const char *path,
                             const uint8_t *data,
                             size_t data_len,
                             uint64_t now_ms,
                             uint32_t timeout_ms,
                             rns_packet_t *packet,
                             rns_request_receipt_t *receipt)
{
    if (!link_is_active(link) || path == NULL || packet == NULL || receipt == NULL ||
        (data == NULL && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > RNS_REQUEST_MAX_DATA_SIZE || timeout_ms == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t path_hash[RNS_REQUEST_PATH_HASH_SIZE];
    esp_err_t err = rns_request_path_hash(path, path_hash);
    if (err != ESP_OK) {
        return err;
    }

    rns_packet_clear(packet);
    packet->destination_type = RNS_DESTINATION_TYPE_LINK;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    packet->context = RNS_PACKET_CONTEXT_REQUEST;
    memcpy(packet->destination_hash, link->link_id, RNS_DESTINATION_HASH_SIZE);

    size_t pos = 0;
    packet->data[pos++] = RNS_REQUEST_RESPONSE_VERSION;
    put_u64(packet->data + pos, now_ms);
    pos += 8;
    memcpy(packet->data + pos, path_hash, RNS_REQUEST_PATH_HASH_SIZE);
    pos += RNS_REQUEST_PATH_HASH_SIZE;
    put_u16(packet->data + pos, (uint16_t)data_len);
    pos += 2;
    if (data_len > 0) {
        memcpy(packet->data + pos, data, data_len);
        pos += data_len;
    }
    packet->data_len = pos;

    memset(receipt, 0, sizeof(*receipt));
    err = rns_packet_truncated_hash(packet, receipt->request_id);
    if (err != ESP_OK) {
        return err;
    }
    receipt->active = true;
    receipt->status = RNS_REQUEST_RECEIPT_PENDING;
    receipt->sent_at_ms = now_ms;
    receipt->timeout_ms = timeout_ms;
    return ESP_OK;
}

esp_err_t rns_request_decode(const rns_packet_t *packet,
                             rns_request_t *request)
{
    if (packet == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->packet_type != RNS_PACKET_TYPE_DATA ||
        packet->context != RNS_PACKET_CONTEXT_REQUEST ||
        packet->destination_type != RNS_DESTINATION_TYPE_LINK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet->data_len < RNS_REQUEST_HEADER_SIZE ||
        packet->data[0] != RNS_REQUEST_RESPONSE_VERSION) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pos = 1;
    uint64_t requested_at_ms = get_u64(packet->data + pos);
    pos += 8;
    const uint8_t *path_hash = packet->data + pos;
    pos += RNS_REQUEST_PATH_HASH_SIZE;
    size_t data_len = get_u16(packet->data + pos);
    pos += 2;
    if (data_len > RNS_REQUEST_MAX_DATA_SIZE || packet->data_len != pos + data_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(request, 0, sizeof(*request));
    request->requested_at_ms = requested_at_ms;
    memcpy(request->path_hash, path_hash, RNS_REQUEST_PATH_HASH_SIZE);
    if (data_len > 0) {
        memcpy(request->data, packet->data + pos, data_len);
    }
    request->data_len = data_len;
    return rns_packet_truncated_hash(packet, request->request_id);
}

esp_err_t rns_response_create(const rns_link_t *link,
                              const uint8_t request_id[RNS_REQUEST_ID_SIZE],
                              const uint8_t *response,
                              size_t response_len,
                              rns_packet_t *packet)
{
    if (!link_is_active(link) || request_id == NULL || packet == NULL ||
        (response == NULL && response_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response_len > RNS_RESPONSE_MAX_DATA_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    rns_packet_clear(packet);
    packet->destination_type = RNS_DESTINATION_TYPE_LINK;
    packet->packet_type = RNS_PACKET_TYPE_DATA;
    packet->context = RNS_PACKET_CONTEXT_RESPONSE;
    memcpy(packet->destination_hash, link->link_id, RNS_DESTINATION_HASH_SIZE);

    size_t pos = 0;
    packet->data[pos++] = RNS_REQUEST_RESPONSE_VERSION;
    memcpy(packet->data + pos, request_id, RNS_REQUEST_ID_SIZE);
    pos += RNS_REQUEST_ID_SIZE;
    put_u16(packet->data + pos, (uint16_t)response_len);
    pos += 2;
    if (response_len > 0) {
        memcpy(packet->data + pos, response, response_len);
        pos += response_len;
    }
    packet->data_len = pos;
    return ESP_OK;
}

esp_err_t rns_response_create_for_request(const rns_link_t *link,
                                          const rns_packet_t *request_packet,
                                          const uint8_t *response,
                                          size_t response_len,
                                          rns_packet_t *packet)
{
    if (!packet_is_for_link(link, request_packet)) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_request_t request;
    esp_err_t err = rns_request_decode(request_packet, &request);
    if (err != ESP_OK) {
        return err;
    }

    return rns_response_create(link,
                               request.request_id,
                               response,
                               response_len,
                               packet);
}

esp_err_t rns_request_receipt_accept_response(rns_request_receipt_t *receipt,
                                              const rns_packet_t *response_packet,
                                              uint64_t now_ms)
{
    if (receipt == NULL || response_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!receipt->active || receipt->status != RNS_REQUEST_RECEIPT_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = rns_request_receipt_check_timeout(receipt, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (receipt->status == RNS_REQUEST_RECEIPT_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }

    if (response_packet->packet_type != RNS_PACKET_TYPE_DATA ||
        response_packet->context != RNS_PACKET_CONTEXT_RESPONSE ||
        response_packet->destination_type != RNS_DESTINATION_TYPE_LINK ||
        response_packet->data_len < RNS_RESPONSE_HEADER_SIZE ||
        response_packet->data[0] != RNS_REQUEST_RESPONSE_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pos = 1;
    const uint8_t *request_id = response_packet->data + pos;
    pos += RNS_REQUEST_ID_SIZE;
    size_t response_len = get_u16(response_packet->data + pos);
    pos += 2;
    if (response_len > RNS_RESPONSE_MAX_DATA_SIZE ||
        response_packet->data_len != pos + response_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!rns_crypto_constant_equal(receipt->request_id, request_id, RNS_REQUEST_ID_SIZE)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (response_len > 0) {
        memcpy(receipt->response, response_packet->data + pos, response_len);
    }
    receipt->response_len = response_len;
    receipt->status = RNS_REQUEST_RECEIPT_COMPLETE;
    receipt->active = false;
    return ESP_OK;
}

esp_err_t rns_request_receipt_check_timeout(rns_request_receipt_t *receipt,
                                            uint64_t now_ms)
{
    if (receipt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!receipt->active || receipt->status != RNS_REQUEST_RECEIPT_PENDING) {
        return ESP_OK;
    }
    if (now_ms < receipt->sent_at_ms) {
        return ESP_OK;
    }
    if (now_ms - receipt->sent_at_ms >= receipt->timeout_ms) {
        receipt->status = RNS_REQUEST_RECEIPT_TIMEOUT;
        receipt->active = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
