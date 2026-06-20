#include "meshpay/rns/rns_crypto.h"

#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include <string.h>

static rns_crypto_rng_fn_t s_rng = NULL;
static void *s_rng_ctx = NULL;

static bool bytes_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        acc |= data[i];
    }
    return acc == 0;
}

static esp_err_t default_rng(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return ESP_OK;
}

void rns_crypto_set_rng(rns_crypto_rng_fn_t rng, void *ctx)
{
    s_rng = rng;
    s_rng_ctx = ctx;
}

void rns_crypto_secure_zero(void *buf, size_t len)
{
    if (buf == NULL) {
        return;
    }
    crypto_wipe(buf, len);
}

bool rns_crypto_constant_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

esp_err_t rns_crypto_random(uint8_t *out, size_t len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rns_crypto_rng_fn_t rng = s_rng ? s_rng : default_rng;
    return rng(s_rng_ctx, out, len);
}

esp_err_t rns_crypto_sha256(const uint8_t *data, size_t len,
                            uint8_t out[RNS_CRYPTO_SHA256_SIZE])
{
    if (out == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_sha256(data, len, out, 0) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t rns_crypto_sha512(const uint8_t *data, size_t len,
                            uint8_t out[RNS_CRYPTO_SHA512_SIZE])
{
    if (out == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_sha512(data, len, out, 0) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t rns_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                 const uint8_t *data, size_t data_len,
                                 uint8_t out[RNS_CRYPTO_HMAC_SHA256_SIZE])
{
    if (out == NULL || (key == NULL && key_len > 0) || (data == NULL && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        return ESP_FAIL;
    }
    return mbedtls_md_hmac(md, key, key_len, data, data_len, out) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t rns_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                                 const uint8_t *salt, size_t salt_len,
                                 const uint8_t *context, size_t context_len,
                                 uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0 ||
        ikm == NULL || ikm_len == 0 ||
        (salt == NULL && salt_len > 0) ||
        (context == NULL && context_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len > 255u * RNS_CRYPTO_SHA256_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        return ESP_FAIL;
    }

    const uint8_t zero_salt[RNS_CRYPTO_SHA256_SIZE] = {0};
    const uint8_t *effective_salt = salt_len == 0 ? zero_salt : salt;
    size_t effective_salt_len = salt_len == 0 ? sizeof(zero_salt) : salt_len;

    uint8_t prk[RNS_CRYPTO_HMAC_SHA256_SIZE];
    int rc = mbedtls_md_hmac(md, effective_salt, effective_salt_len,
                             ikm, ikm_len, prk);
    if (rc != 0) {
        rns_crypto_secure_zero(prk, sizeof(prk));
        return ESP_FAIL;
    }

    uint8_t block[RNS_CRYPTO_HMAC_SHA256_SIZE];
    size_t block_len = 0;
    size_t written = 0;
    uint8_t counter = 1;

    while (written < out_len) {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        rc = mbedtls_md_setup(&ctx, md, 1);
        if (rc == 0) {
            rc = mbedtls_md_hmac_starts(&ctx, prk, sizeof(prk));
        }
        if (rc == 0 && block_len > 0) {
            rc = mbedtls_md_hmac_update(&ctx, block, block_len);
        }
        if (rc == 0 && context_len > 0) {
            rc = mbedtls_md_hmac_update(&ctx, context, context_len);
        }
        if (rc == 0) {
            rc = mbedtls_md_hmac_update(&ctx, &counter, sizeof(counter));
        }
        if (rc == 0) {
            rc = mbedtls_md_hmac_finish(&ctx, block);
        }
        mbedtls_md_free(&ctx);

        if (rc != 0) {
            rns_crypto_secure_zero(prk, sizeof(prk));
            rns_crypto_secure_zero(block, sizeof(block));
            return ESP_FAIL;
        }

        block_len = sizeof(block);
        size_t take = out_len - written;
        if (take > block_len) {
            take = block_len;
        }
        memcpy(out + written, block, take);
        written += take;
        counter++;
    }

    rns_crypto_secure_zero(prk, sizeof(prk));
    rns_crypto_secure_zero(block, sizeof(block));
    return ESP_OK;
}

esp_err_t rns_crypto_pbkdf2_sha256(const uint8_t *password, size_t password_len,
                                   const uint8_t *salt, size_t salt_len,
                                   uint32_t iterations,
                                   uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || iterations == 0 ||
        (password == NULL && password_len > 0) ||
        (salt == NULL && salt_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           password, password_len,
                                           salt, salt_len,
                                           iterations,
                                           out_len, out);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t aes256_cbc_crypt(int mode,
                                  const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE],
                                  const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE],
                                  const uint8_t *input, size_t len,
                                  uint8_t *output)
{
    if (key == NULL || iv == NULL || output == NULL || (input == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((len % RNS_CRYPTO_AES_BLOCK_SIZE) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int rc = (mode == MBEDTLS_AES_ENCRYPT)
                 ? mbedtls_aes_setkey_enc(&ctx, key, 256)
                 : mbedtls_aes_setkey_dec(&ctx, key, 256);
    if (rc == 0) {
        uint8_t iv_copy[RNS_CRYPTO_AES_BLOCK_SIZE];
        memcpy(iv_copy, iv, sizeof(iv_copy));
        rc = mbedtls_aes_crypt_cbc(&ctx, mode, len, iv_copy, input, output);
        rns_crypto_secure_zero(iv_copy, sizeof(iv_copy));
    }

    mbedtls_aes_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t rns_crypto_aes256_cbc_encrypt(
    const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE],
    const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *plain, size_t len,
    uint8_t *cipher)
{
    return aes256_cbc_crypt(MBEDTLS_AES_ENCRYPT, key, iv, plain, len, cipher);
}

esp_err_t rns_crypto_aes256_cbc_decrypt(
    const uint8_t key[RNS_CRYPTO_AES256_KEY_SIZE],
    const uint8_t iv[RNS_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *cipher, size_t len,
    uint8_t *plain)
{
    return aes256_cbc_crypt(MBEDTLS_AES_DECRYPT, key, iv, cipher, len, plain);
}

esp_err_t rns_crypto_ed25519_keypair_from_seed(
    const uint8_t seed[RNS_CRYPTO_ED25519_SEED_SIZE],
    uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE])
{
    if (seed == NULL || private_key == NULL || public_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t seed_copy[RNS_CRYPTO_ED25519_SEED_SIZE];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(private_key, public_key, seed_copy);
    rns_crypto_secure_zero(seed_copy, sizeof(seed_copy));
    return ESP_OK;
}

esp_err_t rns_crypto_ed25519_generate_keypair(
    uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE])
{
    if (private_key == NULL || public_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t seed[RNS_CRYPTO_ED25519_SEED_SIZE];
    esp_err_t err = rns_crypto_random(seed, sizeof(seed));
    if (err != ESP_OK) {
        return err;
    }

    err = rns_crypto_ed25519_keypair_from_seed(seed, private_key, public_key);
    rns_crypto_secure_zero(seed, sizeof(seed));
    return err;
}

esp_err_t rns_crypto_ed25519_sign(
    const uint8_t private_key[RNS_CRYPTO_ED25519_PRIVATE_SIZE],
    const uint8_t *message, size_t message_len,
    uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE])
{
    if (private_key == NULL || signature == NULL || (message == NULL && message_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    crypto_ed25519_sign(signature, private_key, message, message_len);
    return ESP_OK;
}

esp_err_t rns_crypto_ed25519_verify(
    const uint8_t public_key[RNS_CRYPTO_ED25519_PUBLIC_SIZE],
    const uint8_t *message, size_t message_len,
    const uint8_t signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE])
{
    if (public_key == NULL || signature == NULL || (message == NULL && message_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    int rc = crypto_ed25519_check(signature, public_key, message, message_len);
    return rc == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t rns_crypto_x25519_public_key(
    const uint8_t private_key[RNS_CRYPTO_X25519_KEY_SIZE],
    uint8_t public_key[RNS_CRYPTO_X25519_KEY_SIZE])
{
    if (private_key == NULL || public_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(private_key, RNS_CRYPTO_X25519_KEY_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    crypto_x25519_public_key(public_key, private_key);
    return ESP_OK;
}

esp_err_t rns_crypto_x25519_shared_secret(
    const uint8_t private_key[RNS_CRYPTO_X25519_KEY_SIZE],
    const uint8_t peer_public_key[RNS_CRYPTO_X25519_KEY_SIZE],
    uint8_t shared_secret[RNS_CRYPTO_X25519_SHARED_SIZE])
{
    if (private_key == NULL || peer_public_key == NULL || shared_secret == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_zero(private_key, RNS_CRYPTO_X25519_KEY_SIZE) ||
        bytes_zero(peer_public_key, RNS_CRYPTO_X25519_KEY_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }

    crypto_x25519(shared_secret, private_key, peer_public_key);
    if (bytes_zero(shared_secret, RNS_CRYPTO_X25519_SHARED_SIZE)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
