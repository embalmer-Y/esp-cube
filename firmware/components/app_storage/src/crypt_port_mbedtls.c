/* crypt_port_mbedtls.c —— EPSCV2 密码学 port: PSA Crypto 实现(T14 目标构建)
 *
 * IDF 6.1 = mbedtls 4.x: mbedtls/gcm.h 公开头已移除, GCM 经 PSA AEAD。
 * 首次调用 psa_crypto_init() 一次。
 * 存储布局 cipher||tag 连续, 与 psa_aead_decrypt 输入格式直接吻合。
 */
#include <string.h>

#include "psa/crypto.h"
#include "esp_random.h"

#include "store_crypt.h"

/* app_storage.c raw 层(T14; 公开 read/write 在其上加解密) */
char *app_storage_raw_read(const char *rel, size_t *len);
esp_err_t app_storage_raw_write(const char *rel, const void *data, size_t len);
void app_storage_crypt_bind(store_crypt_t *c);

static bool s_psa_inited;

static bool psa_ready(void)
{
    if (!s_psa_inited) {
        if (psa_crypto_init() != PSA_SUCCESS) return false;
        s_psa_inited = true;
    }
    return true;
}

static int mb_gcm_enc(void *ud, const uint8_t key[32],
                      const uint8_t gcm_nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *plain, size_t len,
                      uint8_t *out, uint8_t tag[16])
{
    (void)ud; (void)tag;                          /* PSA 输出=密文||tag 连续 */
    if (!psa_ready()) return -1;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_key_id_t kid = 0;
    if (psa_import_key(&attr, key, 32, &kid) != PSA_SUCCESS) return -1;

    size_t out_len = 0;
    psa_status_t st = psa_aead_encrypt(kid, PSA_ALG_GCM,
                                       gcm_nonce, 12,
                                       aad, aad_len,
                                       plain, len,
                                       out, len + 16, &out_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || out_len != len + 16) return -1;
    return 0;
}

static int mb_gcm_dec(void *ud, const uint8_t key[32],
                      const uint8_t gcm_nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *cipher, size_t len,
                      const uint8_t tag[16], uint8_t *out)
{
    (void)ud;
    if (!psa_ready()) return -1;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_key_id_t kid = 0;
    if (psa_import_key(&attr, key, 32, &kid) != PSA_SUCCESS) return -1;

    /* PSA 输入 = 密文||tag; 存储布局天然连续 → 传 len+16, tag 尾部生效 */
    (void)tag;
    size_t out_len = 0;
    psa_status_t st = psa_aead_decrypt(kid, PSA_ALG_GCM,
                                       gcm_nonce, 12,
                                       aad, aad_len,
                                       cipher, len + 16,
                                       out, len + 16, &out_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || out_len != len) return -1;   /* tag 不符 = 非 0 */
    return 0;
}

static int mb_hkdf(void *ud, const uint8_t ikm[32],
                   const uint8_t *salt, size_t salt_len,
                   const char *info, size_t info_len, uint8_t out[32])
{
    (void)ud;
    if (!psa_ready()) return -1;
    struct psa_key_derivation_s op;
    op = psa_key_derivation_operation_init();
    if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) !=
        PSA_SUCCESS) {
        return -1;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                       salt, salt_len) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET,
                                       ikm, 32) != PSA_SUCCESS ||
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
                                       (const uint8_t *)info,
                                       info_len) != PSA_SUCCESS) {
        psa_key_derivation_abort(&op);
        return -1;
    }
    psa_status_t st = psa_key_derivation_output_bytes(&op, out, 32);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS ? 0 : -1;
}

static int mb_rng(void *ud, uint8_t *out, size_t len)
{
    (void)ud;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t r = esp_random();
        memcpy(out + i, &r, 4);
    }
    for (size_t i = len - len % 4; i < len; i++) {
        out[i] = (uint8_t)esp_random();
    }
    return 0;
}

static const sc_port_t s_mb_port = {
    mb_gcm_enc, mb_gcm_dec, mb_hkdf, mb_rng, NULL,
};

const sc_port_t *app_storage_crypt_port(void) { return &s_mb_port; }

/* 装配入口: 以 PSA port + trust sd_key 启用透明容器(raw 绑定) */
#include "esp_random.h"
#include "trust.h"

void app_storage_crypt_enable(void)
{
    trust_ctx_t tc;
    trust_rng_t tr = { mb_rng, NULL };
    if (trust_init(&tc, &tr) != 0) return;
    uint8_t key[32];
    if (trust_sd_key(&tc, key) != 0) return;

    sc_port_t port = s_mb_port;
    store_crypt_t *c = malloc(sizeof(*c));
    if (c == NULL) return;
    /* 绑定 RAW 层(防递归: 公开 read/write 会再进 crypt) */
    if (store_crypt_init(c, &port, key,
                         app_storage_raw_read, app_storage_raw_write) == 0) {
        app_storage_crypt_bind(c);
    } else {
        free(c);
    }
}
