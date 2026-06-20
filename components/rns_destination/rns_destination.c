#include "meshpay/rns/rns_destination.h"

#include <stdio.h>
#include <string.h>

static bool token_is_valid(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        return false;
    }

    for (const char *p = token; *p != '\0'; ++p) {
        if (*p == '.') {
            return false;
        }
    }
    return true;
}

esp_err_t rns_destination_build_full_name(const char *app_name,
                                          const char *const *aspects,
                                          size_t aspect_count,
                                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || !token_is_valid(app_name) ||
        (aspect_count > 0 && aspects == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out, out_len, "%s", app_name);
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t used = (size_t)written;
    for (size_t i = 0; i < aspect_count; ++i) {
        if (!token_is_valid(aspects[i])) {
            return ESP_ERR_INVALID_ARG;
        }

        written = snprintf(out + used, out_len - used, ".%s", aspects[i]);
        if (written < 0 || (size_t)written >= out_len - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        used += (size_t)written;
    }

    return ESP_OK;
}

esp_err_t rns_destination_name_hash(const char *app_name,
                                    const char *const *aspects,
                                    size_t aspect_count,
                                    uint8_t out[RNS_DESTINATION_NAME_HASH_SIZE])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_name[RNS_DESTINATION_MAX_FULL_NAME];
    esp_err_t err = rns_destination_build_full_name(app_name, aspects, aspect_count,
                                                   full_name, sizeof(full_name));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    err = rns_crypto_sha256((const uint8_t *)full_name, strlen(full_name), full_hash);
    if (err != ESP_OK) {
        rns_crypto_secure_zero(full_hash, sizeof(full_hash));
        return err;
    }

    memcpy(out, full_hash, RNS_DESTINATION_NAME_HASH_SIZE);
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return ESP_OK;
}

static esp_err_t destination_fill_common(const char *app_name,
                                         const char *const *aspects,
                                         size_t aspect_count,
                                         rns_destination_t *out)
{
    esp_err_t err = rns_destination_build_full_name(app_name, aspects, aspect_count,
                                                   out->full_name,
                                                   sizeof(out->full_name));
    if (err != ESP_OK) {
        return err;
    }

    return rns_destination_name_hash(app_name, aspects, aspect_count, out->name_hash);
}

esp_err_t rns_destination_create_single(const rns_identity_t *identity,
                                        const char *app_name,
                                        const char *const *aspects,
                                        size_t aspect_count,
                                        rns_destination_t *out)
{
    if (identity == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->type = RNS_DESTINATION_TYPE_SINGLE;

    esp_err_t err = destination_fill_common(app_name, aspects, aspect_count, out);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    uint8_t identity_hash[RNS_IDENTITY_HASH_SIZE];
    err = rns_identity_get_hash(identity, identity_hash);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    uint8_t material[RNS_DESTINATION_NAME_HASH_SIZE + RNS_IDENTITY_HASH_SIZE];
    memcpy(material, out->name_hash, RNS_DESTINATION_NAME_HASH_SIZE);
    memcpy(material + RNS_DESTINATION_NAME_HASH_SIZE,
           identity_hash,
           RNS_IDENTITY_HASH_SIZE);

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    err = rns_crypto_sha256(material, sizeof(material), full_hash);
    rns_crypto_secure_zero(material, sizeof(material));
    rns_crypto_secure_zero(identity_hash, sizeof(identity_hash));
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        rns_crypto_secure_zero(full_hash, sizeof(full_hash));
        return err;
    }

    memcpy(out->hash, full_hash, RNS_DESTINATION_HASH_SIZE);
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return ESP_OK;
}

esp_err_t rns_destination_create_plain(const char *app_name,
                                       const char *const *aspects,
                                       size_t aspect_count,
                                       rns_destination_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->type = RNS_DESTINATION_TYPE_PLAIN;

    esp_err_t err = destination_fill_common(app_name, aspects, aspect_count, out);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    uint8_t full_hash[RNS_CRYPTO_SHA256_SIZE];
    err = rns_crypto_sha256(out->name_hash, RNS_DESTINATION_NAME_HASH_SIZE, full_hash);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        rns_crypto_secure_zero(full_hash, sizeof(full_hash));
        return err;
    }

    memcpy(out->hash, full_hash, RNS_DESTINATION_HASH_SIZE);
    rns_crypto_secure_zero(full_hash, sizeof(full_hash));
    return ESP_OK;
}

esp_err_t rns_destination_create_link(const uint8_t link_hash[RNS_DESTINATION_HASH_SIZE],
                                      rns_destination_t *out)
{
    if (link_hash == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->type = RNS_DESTINATION_TYPE_LINK;
    memcpy(out->hash, link_hash, RNS_DESTINATION_HASH_SIZE);
    return ESP_OK;
}

esp_err_t rns_destination_create_meshpay_wallet(const rns_identity_t *identity,
                                                rns_destination_t *out)
{
    const char *aspects[] = {RNS_MESHPAY_WALLET_ASPECT};
    return rns_destination_create_single(identity, RNS_MESHPAY_APP_NAME,
                                         aspects, 1, out);
}

bool rns_destination_hash_equal(const uint8_t a[RNS_DESTINATION_HASH_SIZE],
                                const uint8_t b[RNS_DESTINATION_HASH_SIZE])
{
    return rns_crypto_constant_equal(a, b, RNS_DESTINATION_HASH_SIZE);
}

