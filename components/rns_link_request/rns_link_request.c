#include "meshpay/rns/rns_link_request.h"

#include <string.h>

void rns_link_clear(rns_link_t *link)
{
    if (link == NULL) {
        return;
    }
    rns_crypto_secure_zero(link, sizeof(*link));
}

esp_err_t rns_link_signalling_bytes(uint32_t mtu,
                                    uint8_t mode,
                                    uint8_t out[RNS_LINK_MTU_SIGNAL_SIZE])
{
    if (out == NULL || mtu == 0 || mtu > RNS_PACKET_MTU ||
        mode != RNS_LINK_MODE_AES256_CBC ||
        (mtu & ~RNS_LINK_MTU_BYTEMASK) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t signalling_value = (mtu & RNS_LINK_MTU_BYTEMASK) |
                                ((((uint32_t)mode << 5) & RNS_LINK_MODE_BYTEMASK) << 16);
    out[0] = (uint8_t)((signalling_value >> 16) & 0xff);
    out[1] = (uint8_t)((signalling_value >> 8) & 0xff);
    out[2] = (uint8_t)(signalling_value & 0xff);
    return ESP_OK;
}

static uint32_t mtu_from_signalling(const uint8_t signalling[RNS_LINK_MTU_SIGNAL_SIZE])
{
    uint32_t value = ((uint32_t)signalling[0] << 16) |
                     ((uint32_t)signalling[1] << 8) |
                     signalling[2];
    return value & RNS_LINK_MTU_BYTEMASK;
}

static uint8_t mode_from_signalling(const uint8_t signalling[RNS_LINK_MTU_SIGNAL_SIZE])
{
    return (signalling[0] & RNS_LINK_MODE_BYTEMASK) >> 5;
}

esp_err_t rns_link_request_link_id(const rns_packet_t *request_packet,
                                   uint8_t out[RNS_DESTINATION_HASH_SIZE])
{
    if (request_packet == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request_packet->packet_type != RNS_PACKET_TYPE_LINK_REQUEST ||
        request_packet->destination_type != RNS_DESTINATION_TYPE_SINGLE ||
        request_packet->data_len < RNS_LINK_REQUEST_MIN_SIZE ||
        request_packet->data_len > RNS_LINK_REQUEST_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_packet_t hash_packet = *request_packet;
    hash_packet.data_len = RNS_LINK_REQUEST_MIN_SIZE;
    return rns_packet_truncated_hash(&hash_packet, out);
}

esp_err_t rns_link_request_create(const rns_destination_t *destination,
                                  uint32_t mtu,
                                  rns_link_t *link,
                                  rns_packet_t *request_packet)
{
    if (destination == NULL || link == NULL || request_packet == NULL ||
        destination->type != RNS_DESTINATION_TYPE_SINGLE) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_link_clear(link);
    esp_err_t err = rns_identity_generate(&link->local_identity);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    err = rns_identity_get_public_key(&link->local_identity, public_key);
    if (err != ESP_OK) {
        rns_link_clear(link);
        return err;
    }

    uint8_t signalling[RNS_LINK_MTU_SIGNAL_SIZE];
    err = rns_link_signalling_bytes(mtu, RNS_LINK_MODE_AES256_CBC, signalling);
    if (err != ESP_OK) {
        rns_link_clear(link);
        return err;
    }

    rns_packet_clear(request_packet);
    request_packet->destination_type = RNS_DESTINATION_TYPE_SINGLE;
    request_packet->packet_type = RNS_PACKET_TYPE_LINK_REQUEST;
    memcpy(request_packet->destination_hash, destination->hash, RNS_DESTINATION_HASH_SIZE);
    memcpy(request_packet->data, public_key, RNS_LINK_PUBLIC_KEY_SIZE);
    memcpy(request_packet->data + RNS_LINK_PUBLIC_KEY_SIZE,
           signalling,
           RNS_LINK_MTU_SIGNAL_SIZE);
    request_packet->data_len = RNS_LINK_REQUEST_MAX_SIZE;

    err = rns_link_request_link_id(request_packet, link->link_id);
    if (err != ESP_OK) {
        rns_link_clear(link);
        return err;
    }

    link->initiator = true;
    link->status = RNS_LINK_STATUS_PENDING;
    link->mtu = mtu;
    link->mode = RNS_LINK_MODE_AES256_CBC;
    rns_crypto_secure_zero(public_key, sizeof(public_key));
    return ESP_OK;
}

esp_err_t rns_link_request_accept(const rns_identity_t *owner_identity,
                                  const rns_packet_t *request_packet,
                                  uint32_t mtu,
                                  rns_link_t *link,
                                  rns_packet_t *proof_packet)
{
    if (owner_identity == NULL || request_packet == NULL ||
        link == NULL || proof_packet == NULL || !owner_identity->has_private) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request_packet->packet_type != RNS_PACKET_TYPE_LINK_REQUEST ||
        request_packet->destination_type != RNS_DESTINATION_TYPE_SINGLE ||
        request_packet->data_len < RNS_LINK_REQUEST_MIN_SIZE ||
        request_packet->data_len > RNS_LINK_REQUEST_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    rns_link_clear(link);
    esp_err_t err = rns_identity_generate(&link->local_identity);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(link->peer_x25519_public, request_packet->data, RNS_LINK_EC_PUBLIC_SIZE);
    err = rns_link_request_link_id(request_packet, link->link_id);
    if (err != ESP_OK) {
        rns_link_clear(link);
        return err;
    }

    uint8_t peer_public[RNS_IDENTITY_PUBLIC_SIZE];
    memcpy(peer_public, request_packet->data, RNS_LINK_PUBLIC_KEY_SIZE);
    err = rns_identity_shared_secret(&link->local_identity, peer_public, link->shared_key);
    rns_crypto_secure_zero(peer_public, sizeof(peer_public));
    if (err != ESP_OK) {
        rns_link_clear(link);
        return err;
    }

    uint8_t owner_public[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t link_public[RNS_IDENTITY_PUBLIC_SIZE];
    uint8_t signalling[RNS_LINK_MTU_SIGNAL_SIZE];
    err = rns_identity_get_public_key(owner_identity, owner_public);
    if (err == ESP_OK) {
        err = rns_identity_get_public_key(&link->local_identity, link_public);
    }
    if (err == ESP_OK) {
        err = rns_link_signalling_bytes(mtu, RNS_LINK_MODE_AES256_CBC, signalling);
    }
    if (err != ESP_OK) {
        rns_link_clear(link);
        rns_crypto_secure_zero(owner_public, sizeof(owner_public));
        rns_crypto_secure_zero(link_public, sizeof(link_public));
        return err;
    }

    uint8_t signed_data[RNS_DESTINATION_HASH_SIZE +
                        RNS_LINK_EC_PUBLIC_SIZE +
                        RNS_LINK_SIGN_PUBLIC_SIZE +
                        RNS_LINK_MTU_SIGNAL_SIZE];
    size_t pos = 0;
    memcpy(signed_data + pos, link->link_id, RNS_DESTINATION_HASH_SIZE);
    pos += RNS_DESTINATION_HASH_SIZE;
    memcpy(signed_data + pos, link_public, RNS_LINK_EC_PUBLIC_SIZE);
    pos += RNS_LINK_EC_PUBLIC_SIZE;
    memcpy(signed_data + pos,
           owner_public + RNS_LINK_EC_PUBLIC_SIZE,
           RNS_LINK_SIGN_PUBLIC_SIZE);
    pos += RNS_LINK_SIGN_PUBLIC_SIZE;
    memcpy(signed_data + pos, signalling, RNS_LINK_MTU_SIGNAL_SIZE);
    pos += RNS_LINK_MTU_SIGNAL_SIZE;

    rns_packet_clear(proof_packet);
    proof_packet->destination_type = RNS_DESTINATION_TYPE_LINK;
    proof_packet->packet_type = RNS_PACKET_TYPE_PROOF;
    proof_packet->context = RNS_PACKET_CONTEXT_LRPROOF;
    memcpy(proof_packet->destination_hash, link->link_id, RNS_DESTINATION_HASH_SIZE);

    err = rns_identity_sign(owner_identity,
                            signed_data,
                            pos,
                            proof_packet->data);
    if (err == ESP_OK) {
        memcpy(proof_packet->data + RNS_CRYPTO_ED25519_SIGNATURE_SIZE,
               link_public,
               RNS_LINK_EC_PUBLIC_SIZE);
        memcpy(proof_packet->data + RNS_LINK_PROOF_MIN_SIZE,
               signalling,
               RNS_LINK_MTU_SIGNAL_SIZE);
        proof_packet->data_len = RNS_LINK_PROOF_MAX_SIZE;
        link->initiator = false;
        link->status = RNS_LINK_STATUS_ACTIVE;
        link->mtu = mtu;
        link->mode = RNS_LINK_MODE_AES256_CBC;
    } else {
        rns_link_clear(link);
    }

    rns_crypto_secure_zero(owner_public, sizeof(owner_public));
    rns_crypto_secure_zero(link_public, sizeof(link_public));
    rns_crypto_secure_zero(signed_data, sizeof(signed_data));
    return err;
}

esp_err_t rns_link_request_validate_proof(const rns_identity_t *destination_identity,
                                          const rns_packet_t *proof_packet,
                                          rns_link_t *link)
{
    if (destination_identity == NULL || proof_packet == NULL || link == NULL ||
        !destination_identity->has_public || !link->local_identity.has_private) {
        return ESP_ERR_INVALID_ARG;
    }
    if (proof_packet->packet_type != RNS_PACKET_TYPE_PROOF ||
        proof_packet->destination_type != RNS_DESTINATION_TYPE_LINK ||
        proof_packet->context != RNS_PACKET_CONTEXT_LRPROOF ||
        proof_packet->data_len < RNS_LINK_PROOF_MIN_SIZE ||
        proof_packet->data_len > RNS_LINK_PROOF_MAX_SIZE ||
        !rns_destination_hash_equal(proof_packet->destination_hash, link->link_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *signature = proof_packet->data;
    const uint8_t *peer_x_public = proof_packet->data + RNS_CRYPTO_ED25519_SIGNATURE_SIZE;
    const uint8_t *signalling = NULL;
    uint8_t default_signalling[RNS_LINK_MTU_SIGNAL_SIZE];
    esp_err_t err = ESP_OK;
    if (proof_packet->data_len == RNS_LINK_PROOF_MAX_SIZE) {
        signalling = proof_packet->data + RNS_LINK_PROOF_MIN_SIZE;
    } else {
        err = rns_link_signalling_bytes(RNS_PACKET_MTU,
                                        RNS_LINK_MODE_AES256_CBC,
                                        default_signalling);
        signalling = default_signalling;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t destination_public[RNS_IDENTITY_PUBLIC_SIZE];
    err = rns_identity_get_public_key(destination_identity, destination_public);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t signed_data[RNS_DESTINATION_HASH_SIZE +
                        RNS_LINK_EC_PUBLIC_SIZE +
                        RNS_LINK_SIGN_PUBLIC_SIZE +
                        RNS_LINK_MTU_SIGNAL_SIZE];
    size_t pos = 0;
    memcpy(signed_data + pos, link->link_id, RNS_DESTINATION_HASH_SIZE);
    pos += RNS_DESTINATION_HASH_SIZE;
    memcpy(signed_data + pos, peer_x_public, RNS_LINK_EC_PUBLIC_SIZE);
    pos += RNS_LINK_EC_PUBLIC_SIZE;
    memcpy(signed_data + pos,
           destination_public + RNS_LINK_EC_PUBLIC_SIZE,
           RNS_LINK_SIGN_PUBLIC_SIZE);
    pos += RNS_LINK_SIGN_PUBLIC_SIZE;
    memcpy(signed_data + pos, signalling, RNS_LINK_MTU_SIGNAL_SIZE);
    pos += RNS_LINK_MTU_SIGNAL_SIZE;

    err = rns_identity_verify(destination_identity, signed_data, pos, signature);
    if (err == ESP_OK) {
        uint8_t peer_public[RNS_IDENTITY_PUBLIC_SIZE] = {0};
        memcpy(peer_public, peer_x_public, RNS_LINK_EC_PUBLIC_SIZE);
        err = rns_identity_shared_secret(&link->local_identity,
                                         peer_public,
                                         link->shared_key);
        rns_crypto_secure_zero(peer_public, sizeof(peer_public));
    }
    if (err == ESP_OK) {
        uint32_t mtu = mtu_from_signalling(signalling);
        uint8_t mode = mode_from_signalling(signalling);
        if (mtu == 0 || mtu > RNS_PACKET_MTU ||
            mode != RNS_LINK_MODE_AES256_CBC) {
            rns_crypto_secure_zero(destination_public, sizeof(destination_public));
            rns_crypto_secure_zero(signed_data, sizeof(signed_data));
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(link->peer_x25519_public, peer_x_public, RNS_LINK_EC_PUBLIC_SIZE);
        link->status = RNS_LINK_STATUS_ACTIVE;
        link->mtu = mtu;
        link->mode = mode;
    }

    rns_crypto_secure_zero(destination_public, sizeof(destination_public));
    rns_crypto_secure_zero(signed_data, sizeof(signed_data));
    return err;
}
