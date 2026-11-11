/*
 * ncm_weapi.c —— 网易云 weapi 请求加密(AES-128-CBC×2 + RSA-1024 裸模幂)
 *
 * 与 api-enhanced util/crypto.js 的 weapi() 逐位对齐(2026-11 裸调验证):
 *   params    = B64( AES-CBC( B64( AES-CBC(json, presetKey, iv) ), secKey, iv ) )
 *   encSecKey = hex( RSA_raw(secKey 反转字符串) )   ← 教科书式裸 RSA
 *   POST 表单: params=<urlencoded>&encSecKey=<hex小写>
 * 密钥常量社区公开(网易前端自带), 见 api-enhanced 仓库 util/crypto.js。
 *
 * IDF 6.1 = mbedtls 4.x: AES 走 PSA Crypto(固 IV + CBC_NO_PADDING,
 * PKCS7 手工填充), RSA 裸模幂用 bignum MPI。
 */
#include "ncm_weapi.h"

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
/* 顺序敏感: bignum.h(ESP port 封装)必须先于 psa/crypto.h —— 后者会
 * 无宏私含 private/bignum.h, 若先被包含会占掉 include guard 使 mpi 声明丢失 */
#include "mbedtls/bignum.h"
#include "psa/crypto.h"
#include "mbedtls/base64.h"

static const char *TAG = "ncm_weapi";

/* 网易 weapi 公开常量(前端自带, 社区逆向公开); 恰 16 字节(不含 NUL) */
static const char k_preset_key[] = "0CoJUm6Qyw8W8jud";
static const char k_iv[] = "0102030405060708";
static const char k_base62[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/* RSA-1024 公钥模数(大端 128B, 自 api-enhanced crypto.js 的 PEM 提取;
 * 指数 e = 0x10001) */
static const char *k_rsa_n_hex =
    "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152"
    "b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbd"
    "a92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813c"
    "fe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

static int hex2bin_n(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* AES-128-CBC 加密(手工 PKCS7 + 固定 IV; 与 CryptoJS.AES.encrypt 对齐),
 * 输出 base64 字符串(NUL 结尾)。PSA CBC_NO_PADDING: 输出长度恒=填充后长度 */
static esp_err_t aes_cbc_b64(const char *text, const char key[16],
                             char **out_b64)
{
    size_t in_len = strlen(text);
    size_t pad = 16 - (in_len % 16);            /* PKCS7: 恒填充 1..16 */
    size_t padded = in_len + pad;
    uint8_t *buf = malloc(padded);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, text, in_len);
    memset(buf + in_len, (int)pad, pad);

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CBC_NO_PADDING);

    psa_key_id_t key_id = 0;
    psa_status_t st = psa_import_key(&attrs, (const uint8_t *)key, 16, &key_id);
    psa_reset_key_attributes(&attrs);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa import: %d", (int)st);
        free(buf);
        return ESP_FAIL;
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t olen = 0, total = 0;
    uint8_t *enc = NULL;
    do {
        st = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CBC_NO_PADDING);
        if (st != PSA_SUCCESS) break;
        st = psa_cipher_set_iv(&op, (const uint8_t *)k_iv, 16);
        if (st != PSA_SUCCESS) break;
        enc = malloc(padded);                   /* NO_PADDING: 输出=输入长 */
        if (enc == NULL) { st = PSA_ERROR_INSUFFICIENT_MEMORY; break; }
        st = psa_cipher_update(&op, buf, padded, enc, padded, &olen);
        if (st != PSA_SUCCESS) break;
        total = olen;
        st = psa_cipher_finish(&op, enc + total, padded - total, &olen);
        if (st != PSA_SUCCESS) break;
        total += olen;
    } while (0);
    (void)psa_cipher_abort(&op);
    (void)psa_destroy_key(key_id);
    free(buf);
    if (st != PSA_SUCCESS || total != padded) {
        ESP_LOGE(TAG, "psa cbc: %d (len %zu/%zu)", (int)st, total, padded);
        free(enc);
        return ESP_FAIL;
    }

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, enc, total);   /* 查询所需长度 */
    char *b64 = malloc(b64_len + 1);
    if (b64 == NULL) {
        free(enc);
        return ESP_ERR_NO_MEM;
    }
    int r = mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, &b64_len,
                                  enc, total);
    free(enc);
    if (r != 0) {
        free(b64);
        return ESP_FAIL;
    }
    b64[b64_len] = 0;
    *out_b64 = b64;
    return ESP_OK;
}

/* 教科书式裸 RSA: m^e mod n(forge encrypt('NONE') 等价), 输出 256 字符小写 hex */
static esp_err_t rsa_raw_hex(const uint8_t *m, size_t m_len, char *out_hex /*257*/)
{
    uint8_t n_bin[128];
    if (hex2bin_n(k_rsa_n_hex, n_bin, sizeof(n_bin)) != 0) {
        return ESP_FAIL;
    }

    mbedtls_mpi N, E, M, X;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&M);
    mbedtls_mpi_init(&X);
    esp_err_t err = ESP_FAIL;
    uint8_t e_bin[3] = { 0x01, 0x00, 0x01 };    /* 65537 */

    do {
        if (mbedtls_mpi_read_binary(&N, n_bin, sizeof(n_bin)) != 0) break;
        if (mbedtls_mpi_read_binary(&E, e_bin, sizeof(e_bin)) != 0) break;
        if (mbedtls_mpi_read_binary(&M, m, m_len) != 0) break;
        if (mbedtls_mpi_exp_mod(&X, &M, &E, &N, NULL) != 0) break;
        uint8_t out[128];
        if (mbedtls_mpi_write_binary(&X, out, sizeof(out)) != 0) break;
        static const char hexdig[] = "0123456789abcdef";
        for (int i = 0; i < 128; i++) {
            out_hex[2 * i] = hexdig[out[i] >> 4];
            out_hex[2 * i + 1] = hexdig[out[i] & 0xf];
        }
        out_hex[256] = 0;
        err = ESP_OK;
    } while (0);

    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&M);
    mbedtls_mpi_free(&X);
    return err;
}

esp_err_t ncm_weapi_build(const char *json_body, char **out_body, size_t *out_len)
{
    /* 0. PSA 一次性初始化(TLS 已初始化时幂等) */
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS && st != PSA_ERROR_ALREADY_EXISTS) {
        ESP_LOGE(TAG, "psa init: %d", (int)st);
        return ESP_FAIL;
    }

    /* 1. 随机 16 字符 secretKey(base62) */
    char sec_key[17];
    for (int i = 0; i < 16; i++) {
        sec_key[i] = k_base62[esp_random() % 62];
    }
    sec_key[16] = 0;

    /* 2. 双层 AES-CBC → base64 */
    char *inner = NULL, *params = NULL;
    esp_err_t err = aes_cbc_b64(json_body, k_preset_key, &inner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "inner aes fail");
        return err;
    }
    err = aes_cbc_b64(inner, sec_key, &params);
    free(inner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "outer aes fail");
        return err;
    }

    /* 3. 反转 secretKey → 裸 RSA → hex */
    char rev[17];
    for (int i = 0; i < 16; i++) {
        rev[i] = sec_key[15 - i];
    }
    rev[16] = 0;
    char *enc_sec = malloc(257);
    if (enc_sec == NULL) {
        free(params);
        return ESP_ERR_NO_MEM;
    }
    err = rsa_raw_hex((const uint8_t *)rev, 16, enc_sec);
    if (err != ESP_OK) {
        free(params);
        free(enc_sec);
        ESP_LOGE(TAG, "rsa fail");
        return err;
    }

    /* 4. 表单编码(params 的 base64 需转义 +,/,=) */
    size_t cap = strlen(params) * 3 + 256 + 64;
    char *body = malloc(cap);
    if (body == NULL) {
        free(params);
        free(enc_sec);
        return ESP_ERR_NO_MEM;
    }
    char *p = body;
    p += sprintf(p, "params=");
    for (const char *c = params; *c; c++) {
        if (*c == '+') p += sprintf(p, "%%2B");
        else if (*c == '/') p += sprintf(p, "%%2F");
        else if (*c == '=') p += sprintf(p, "%%3D");
        else *p++ = *c;
    }
    p += sprintf(p, "&encSecKey=%s", enc_sec);
    free(params);
    free(enc_sec);

    *out_body = body;
    *out_len = (size_t)(p - body);
    return ESP_OK;
}
