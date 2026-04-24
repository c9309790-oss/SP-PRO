#include "mbedtls/rsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "esp_log.h"
#include "RSA_sign.h"
#include "string.h"

static const char *TAG = "RSA_SIGN";


static int RSA_signature(const unsigned char *m,
                         size_t m_length,
                         unsigned char *sigret,
                         size_t sigret_size,
                         size_t *siglen,
                         const unsigned char *key,
                         size_t key_len)
{
    int ret = 0;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_pk_context pk;

    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_pk_init(&pk);

    if (m == NULL || sigret == NULL || siglen == NULL || key == NULL || key_len == 0 || sigret_size == 0) {
        ESP_LOGE(TAG, "Invalid RSA sign input, key_len=%u sigret_size=%u",
                 (unsigned)key_len, (unsigned)sigret_size);
        ret = -1;
        goto cleanup;
    }
    *siglen = 0;

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Random number generator initialization error, ret=%d", ret);
        ret = -2;
        goto cleanup;
    }

    ret = mbedtls_pk_parse_key(&pk, key, key_len, NULL, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse private key, ret=%d key_len=%u", ret, (unsigned)key_len);
        ret = -3;
        goto cleanup;
    }

    if (mbedtls_pk_get_len(&pk) > sigret_size) {
        ESP_LOGE(TAG,
                 "Signature buffer too small, required=%u provided=%u",
                 (unsigned)mbedtls_pk_get_len(&pk),
                 (unsigned)sigret_size);
        ret = -4;
        goto cleanup;
    }

    ret = mbedtls_pk_sign(&pk,
                          MBEDTLS_MD_MD5,
                          m,
                          m_length,
                          sigret,
                          sigret_size,
                          siglen,
                          mbedtls_ctr_drbg_random,
                          &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Signing error, ret=%d", ret);
        ret = -5;
    } else {
        ESP_LOGI(TAG, "RSA signature success, siglen=%u", (unsigned)*siglen);
    }

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return ret;
}

int rsa_sign(const char *data,
             size_t data_len,
             const unsigned char *private_key_dec,
             size_t key_len,
             unsigned char *output,
             size_t output_buf_len,
             size_t *output_len) {

    uint8_t md5[16] = {0};
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_string("MD5");
    int ret;

    if (data == NULL || private_key_dec == NULL || output == NULL || output_len == NULL) {
        ESP_LOGE(TAG, "rsa_sign input is NULL");
        return -1;
    }

    if (md_info == NULL) {
        ESP_LOGE(TAG, "Failed to get MD5 info");
        return -1;
    }

    ret = mbedtls_md(md_info, (const unsigned char *)data, data_len, md5);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to hash auth context, ret=%d", ret);
        return ret;
    }

    return RSA_signature(md5, sizeof(md5), output, output_buf_len, output_len, private_key_dec, key_len);
}




