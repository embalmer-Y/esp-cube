/* store_crypt.c —— EPSCV2 认证加密容器实现(见 store_crypt.h)
 *
 * 组装用缓冲按需 malloc(写频率低: 记忆全量回写/registry 台调);
 * nonce 每写随机(24B, 其中 16B 兼作 HKDF salt, 前 12B 兼作 GCM nonce)。
 */
#include <stdlib.h>
#include <string.h>

#include "store_crypt.h"

#include "esp_heap_caps.h"

static const char SC_MAGIC[SC_MAGIC_LEN] = "EPSCV2\0";

int store_crypt_init(store_crypt_t *c, const sc_port_t *port,
                     const uint8_t sd_key[32],
                     char *(*read_fn)(const char *, size_t *),
                     int (*write_fn)(const char *, const void *, size_t))
{
    if (c == NULL || port == NULL || read_fn == NULL || write_fn == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->port = port;
    memcpy(c->sd_key, sd_key, SC_KEY_LEN);
    c->read_fn = read_fn;
    c->write_fn = write_fn;
    return 0;
}

/* per-file 密钥派生: HKDF(sd_key, salt=nonce, info="epsc/<rel>") */
static int derive_key(store_crypt_t *c, const uint8_t nonce[SC_NONCE_LEN],
                      const char *rel, uint8_t out[32])
{
    char info[96];
    size_t rl = strlen(rel);
    if (rl > sizeof(info) - 6) return -1;
    memcpy(info, "epsc/", 5);
    memcpy(info + 5, rel, rl + 1);
    return c->port->hkdf(c->port->ud, c->sd_key, nonce, SC_NONCE_LEN,
                         info, 5 + rl, out);
}

int store_crypt_write(store_crypt_t *c, const char *rel,
                      const void *plain, size_t len)
{
    if (c == NULL || c->port == NULL || rel == NULL ||
        (plain == NULL && len != 0)) {
        return -1;
    }
    uint8_t *blob = malloc(SC_MAGIC_LEN + SC_NONCE_LEN + len + SC_TAG_LEN);
    if (blob == NULL) return -1;

    /* magic + 随机 nonce(16B salt + 前 12B 亦作 GCM nonce) */
    memcpy(blob, SC_MAGIC, SC_MAGIC_LEN);
    if (c->port->rng != NULL &&
        c->port->rng(c->port->ud, blob + SC_MAGIC_LEN, SC_NONCE_LEN) != 0) {
        free(blob);
        return -1;
    }
    uint8_t key[32];
    if (derive_key(c, blob + SC_MAGIC_LEN, rel, key) != 0) {
        free(blob);
        return -1;
    }
    /* aad = rel 路径(文件换位可检出) */
    if (c->port->gcm_enc(c->port->ud, key, blob + SC_MAGIC_LEN,
                         (const uint8_t *)rel, strlen(rel),
                         plain, len,
                         blob + SC_MAGIC_LEN + SC_NONCE_LEN,
                         blob + SC_MAGIC_LEN + SC_NONCE_LEN + len) != 0) {
        free(blob);
        return -1;
    }
    int rc = c->write_fn(rel, blob,
                         SC_MAGIC_LEN + SC_NONCE_LEN + len + SC_TAG_LEN);
    free(blob);
    return rc;
}

int store_crypt_read(store_crypt_t *c, const char *rel,
                     void *buf, size_t cap, size_t *len)
{
    if (c == NULL || c->port == NULL || rel == NULL || len == NULL) {
        return -1;
    }
    size_t flen = 0;
    char *blob = c->read_fn(rel, &flen);
    if (blob == NULL) return 1;                 /* 不存在 */
    /* 旧明文兼容(T20 迁移期): 无 EPSCV2 magic → 按明文透传
     * (检查顺序: magic 先于最短长度——短明文文件合法) */
    if (flen >= SC_MAGIC_LEN && memcmp(blob, SC_MAGIC, SC_MAGIC_LEN) == 0) {
        if (flen < SC_MAGIC_LEN + SC_NONCE_LEN + SC_TAG_LEN) {
            free(blob);
            return -1;
        }
    } else {
        /* 旧明文兼容(T20 迁移期): 无 EPSCV2 magic → 按明文透传。
         * 修复(2026-11-19, §55.16 事件 A 根因): 原实现在 buf=NULL 长度
         * 查询模式(app_storage_read 唯一用法)下 memcpy(NULL,..,0) 且
         * 不设 cache → 调用方拿到 len=0/空 payload → 上层判 BAD_FILE
         * ——crypt 激活后卡上全部明文资产(表情素材等)静默读失败。
         * 统一走 cache 承载(与 GCM 路径同一出口语义)。 */
        free(c->cache);
        /* PSRAM: 明文资产可到几十 KB(§55.16 续2: 内部堆分配失败=BAD_FILE
         * 且与 boot 期任务争内存在 v6 时代已证不可行) */
        c->cache = heap_caps_malloc(flen ? flen : 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (c->cache == NULL) {
            free(blob);
            return -1;
        }
        memcpy(c->cache, blob, flen);
        c->cache_len = flen;
        if (buf != NULL) {
            size_t k = flen < cap ? flen : cap;
            memcpy(buf, blob, k);
        }
        *len = flen;
        free(blob);
        return 0;
    }
    const uint8_t *nonce = (const uint8_t *)blob + SC_MAGIC_LEN;
    size_t clen = flen - SC_MAGIC_LEN - SC_NONCE_LEN - SC_TAG_LEN;
    const uint8_t *cipher = (const uint8_t *)blob + SC_MAGIC_LEN + SC_NONCE_LEN;
    const uint8_t *tag = (const uint8_t *)blob + SC_MAGIC_LEN + SC_NONCE_LEN + clen;
    uint8_t key[32];
    if (derive_key(c, nonce, rel, key) != 0) { free(blob); return -1; }
    /* 解入内部 cache(供 buf=NULL 模式原地解析); 旧 cache 释放。
     * PSRAM(同明文路径); 并发安全由调用方 app_storage_read 持锁 */
    free(c->cache);
    c->cache = heap_caps_malloc(clen ? clen : 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c->cache == NULL) { free(blob); return -1; }
    int rc = c->port->gcm_dec(c->port->ud, key, nonce,
                              (const uint8_t *)rel, strlen(rel),
                              cipher, clen, tag, c->cache);
    free(blob);
    if (rc != 0) {                              /* AUTH 失败: 篡改/换位/他卡 */
        free(c->cache); c->cache = NULL; c->cache_len = 0;
        return -2;
    }
    c->cache_len = clen;
    if (buf != NULL) {
        if (cap < clen) return -1;
        memcpy(buf, c->cache, clen);
    }
    *len = clen;
    return 0;
}

const void *store_crypt_payload(const store_crypt_t *c)
{
    return (c != NULL && c->cache != NULL) ? (const void *)c->cache : NULL;
}
