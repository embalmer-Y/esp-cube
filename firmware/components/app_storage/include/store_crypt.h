/* store_crypt.h —— EPSCV2 认证加密容器(T14; docs/20 §5)
 *
 * 格式: [8B magic "EPSCV2\0"][24B nonce(随机, 16B 作 HKDF salt)][密文][16B GCM tag]
 * 密钥: HKDF-SHA256(sd_key, salt=nonce, info="epsc/<rel>") —— per-file 路径绑定。
 * aad = rel 路径(防文件换位)。解密失败 = AUTH 错误 → 调用方走双槽回退。
 *
 * 密码学原语经 port 注入: 目标=mbedtls 实现(crypt_port_mbedtls), 宿主
 * 测试=测试桩(非密码学, 仅验流程/失败语义——真互操作归目标构建)。
 * 兼容读: 无 EPSCV2 magic 的旧明文文件按明文读(T20 旧数据迁移期), 写一律加密。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_MAGIC_LEN   8
#define SC_NONCE_LEN   24
#define SC_TAG_LEN     16
#define SC_KEY_LEN     32

typedef struct {
    /* GCM 加密: out=密文(len), tag[16]; 返回 0=OK */
    int (*gcm_enc)(void *ud, const uint8_t key[32], const uint8_t gcm_nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *plain, size_t len,
                   uint8_t *out, uint8_t tag[16]);
    /* GCM 解密: tag 不符返回非 0 */
    int (*gcm_dec)(void *ud, const uint8_t key[32], const uint8_t gcm_nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *cipher, size_t len,
                   const uint8_t tag[16], uint8_t *out);
    /* HKDF-SHA256(ikm[32], salt, salt_len, info, info_len) → out[32] */
    int (*hkdf)(void *ud, const uint8_t ikm[32],
                const uint8_t *salt, size_t salt_len,
                const char *info, size_t info_len, uint8_t out[32]);
    /* 随机填充( nonce 用) */
    int (*rng)(void *ud, uint8_t *out, size_t len);
    void *ud;
} sc_port_t;

typedef struct {
    const sc_port_t *port;
    uint8_t sd_key[32];        /* 主密钥(trust 注入) */
    /* 文件层透传(与 app_storage read/write 同语义) */
    char *(*read_fn)(const char *rel, size_t *len);          /* NULL=不存在 */
    int (*write_fn)(const char *rel, const void *data, size_t len);
    /* read(NULL) 模式的解密缓存(内部持有, store_crypt_payload 取访问) */
    uint8_t *cache;
    size_t cache_len;
} store_crypt_t;

int store_crypt_init(store_crypt_t *c, const sc_port_t *port,
                     const uint8_t sd_key[32],
                     char *(*read_fn)(const char *, size_t *),
                     int (*write_fn)(const char *, const void *, size_t));

/* 加密写(整文件); 返回 0=OK */
int store_crypt_write(store_crypt_t *c, const char *rel,
                      const void *plain, size_t len);
/* 读: 自动识别 EPSCV2(解密) 或旧明文(透传); AUTH 失败返回 -2 */
int store_crypt_read(store_crypt_t *c, const char *rel,
                     void *buf, size_t cap, size_t *len);
/* read(NULL) 后有效: 返回内部解密缓存 payload(至下次 read) */
const void *store_crypt_payload(const store_crypt_t *c);

#ifdef __cplusplus
}
#endif
