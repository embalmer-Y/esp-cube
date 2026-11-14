/* credstore.h —— 服务凭据安全存储(T17 设备侧; docs/18 T17)
 *
 * 职责: 用户自备凭据(LLM API key 等)的落盘/轮换/删除/版本冲突语义。
 *
 * 安全口径(docs/18 T17 验收"存储检查不见密钥明文"):
 *   - 每条凭据 EPSCV2 容器落盘(NVS "creds" 区单 blob): 密钥 =
 *     HKDF(dev_key, salt=nonce, info="epsc/cred/<name>") —— 与 SD 记忆
 *     容器同族(store_crypt 复用), AAD=name 绑定防换位;
 *   - 主密钥=trust dev_key(与 sd_key 分域: 换 SD 卡不影响凭据);
 *   - 明文仅瞬时存在于调用缓冲, 不入日志; list 只给名字+版本。
 *
 * 版本冲突(双手机并发修改): 每条凭据单调版本号; set/delete 携带
 * expected_version(0=新建); 不匹配返回 CRED_RES_CONFLICT 并回当前值。
 *
 * 层位: L3(依赖 app_storage L2 公开头: trust/store_crypt/port getter)。
 */
#ifndef CREDSTORE_H
#define CREDSTORE_H

#include <stddef.h>
#include <stdint.h>

#include "store_crypt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRED_NAME_MAX   32
#define CRED_VALUE_MAX  512
#define CRED_COUNT_MAX  8

/* 结果码 */
#define CRED_RES_OK       0
#define CRED_RES_ERR     -1       /* 参数/NVS 错 */
#define CRED_RES_CONFLICT -2      /* 版本冲突(当前版本经 out 回传) */
#define CRED_RES_NOTFND  -3
#define CRED_RES_FULL    -4       /* 条目满 */
#define CRED_RES_TOOBIG  -5

/* 调用方嵌入(同 pair 惯例); 字段属实现细节勿直接触碰 */
typedef struct credstore {
    uint8_t dev_key[32];
    store_crypt_t crypt;               /* EPSCV2 容器上下文 */
} credstore_t;

/* 初始化(dev_key 32B 由调用方注入——trust 注入); 返回 0=OK */
int credstore_init(credstore_t *c, const uint8_t dev_key[32]);

/* 写入/轮换: expected=0 表示新建(已存在则冲突); 成功回新版本号;
 * 冲突时 *ver_out=当前版本 */
int credstore_set(credstore_t *c, const char *name,
                  const char *value, uint32_t expected,
                  uint32_t *ver_out);
/* 读明文(仅瞬态; 不存在返回 NOTFND; 值长经 *len_out) */
int credstore_get(credstore_t *c, const char *name,
                  char *value_out, size_t cap, size_t *len_out);
/* 删除: expected 不匹配 → CONFLICT(回当前版本) */
int credstore_delete(credstore_t *c, const char *name,
                     uint32_t expected, uint32_t *ver_out);
/* 枚举(名字+版本; 不含值): 返回条目数 */
int credstore_list(credstore_t *c,
                   char names[][CRED_NAME_MAX], uint32_t vers[],
                   int max);
/* 单条当前版本(不存在 → -1) */
int credstore_version(credstore_t *c, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* CREDSTORE_H */
