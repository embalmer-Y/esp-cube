/* trust.h —— 设备信任区(T14; docs/20 §2): dev_key/sd_key 生成与持久化
 *
 * M1 软件方案: 密钥存 NVS "trust" 命名空间——诚实边界: 物理 dump flash
 * 可提取(M2 flash encryption 上线后才防, 独立授权项)。
 * RNG 注入: 目标=esp_random 包装(联网后生成避弱熵), 宿主=确定性序列。
 * NVS 直调(宿主经 stub), key 32B AES-256。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUST_KEY_LEN 32
#define TRUST_NVS_NS  "trust"

typedef struct {
    int (*rng)(void *ud, uint8_t *out, size_t len);   /* 填充 out */
    void *ud;
} trust_rng_t;

typedef struct {
    trust_rng_t rng;
    bool dev_ready, sd_ready;
    uint8_t dev_cache[32], sd_cache[32];
} trust_ctx_t;

/* 初始化; rng 必填。不立即生成密钥——首次 get 时惰性生成并持久化。 */
int trust_init(trust_ctx_t *c, const trust_rng_t *rng);

/* 取设备/SD 密钥(32B); 首次调用生成并写 NVS, 之后读回。 */
int trust_dev_key(trust_ctx_t *c, uint8_t out[TRUST_KEY_LEN]);
int trust_sd_key(trust_ctx_t *c, uint8_t out[TRUST_KEY_LEN]);

/* 清除信任区(恢复出厂语义; 不可逆) */
int trust_wipe(trust_ctx_t *c);

#ifdef __cplusplus
}
#endif
