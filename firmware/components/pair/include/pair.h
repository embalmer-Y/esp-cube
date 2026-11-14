/* pair.h —— 配对服务(T15; docs/20 §3 M1)
 *
 * 首配语义(无硬件信任根): 绑定第一台物理接触的 App(近场假设)。
 *   1. 触摸/物理入口开窗(120s) → 屏显 6 位 PIN;
 *   2. App 于窗口内发起 SRP-6a 交换(step1 salt/B → step2 A → step3
 *      M1/M2 双向证明; PIN 错 5 次/窗 → 窗口作废, 重开须物理在场);
 *   3. 交换成功后 App 出示其公钥(P-256 SEC1 65B) → SHA-256 指纹入表
 *      (NVS "pair" 区, ≤4 台; client_id 由设备签发)。
 * 撤销 = 删表项; 换机 = 新开窗(旧 App 收到变更通知归 T17 通道)。
 * SRP 会话密钥(srp_srv_key)供后续通道密钥派生(T15 续 mTLS)。
 *
 * 调用序列: window_open → step1 → step2 → step3 → bind(可选换机多客户端)
 * 层位: L3(依赖 app_storage L2 公开头的信任/NVS 同族能力——本组件直用
 * NVS "pair" 区, 与 trust 区互不触碰)。
 */
#ifndef PAIR_H
#define PAIR_H

#include <stddef.h>
#include <stdint.h>

#include "srp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_PIN_LEN        6
#define PAIR_WINDOW_MS      120000u
#define PAIR_MAX_FAILS      5
#define PAIR_FP_LEN         32
#define PAIR_CLIENTS_MAX    4
#define PAIR_PUBKEY_LEN     65      /* SEC1 未压缩 P-256 */

typedef struct {
    srp_port_t srp;                    /* hash + rng(共享 ud) */
    /* 可注入时钟(TTL 测试); NULL = 0(窗口永续, 仅测试用) */
    uint64_t (*now_ms)(void *ud);
} pair_port_t;

typedef struct pair_svc {
    /* 字段属实现细节(勿直接触碰); 公开仅为调用方嵌入存储 */
    int inited;
    pair_port_t port;
    int window_open;
    uint64_t window_deadline;
    uint8_t pin[PAIR_PIN_LEN + 1];
    int fails;
    _Alignas(8) uint8_t srv_storage[2200];   /* srp_srv_t(实现在此, 含
         指针字段 → 8 字节对齐必须; 容量由 pair_svc.c 静态断言守护) */
    int exchange_ok;
} pair_svc_t;

/* 结果码(>0 保留给上通道) */
#define PAIR_RES_OK        0
#define PAIR_RES_ERR      -1       /* 参数/状态/协议错 */
#define PAIR_RES_WINDOW   -2       /* 无有效窗口 */
#define PAIR_RES_LOCKED   -3       /* 失败超限, 窗口作废 */
#define PAIR_RES_FULL     -4       /* 指纹表满(先撤销) */
#define PAIR_RES_NOTFND   -5       /* 客户端不在表 */

/* 配对会话建议容量: 调用方保证 pair_svc_t 所在存储 ≥ sizeof +
 * step 调用任务栈 ≥ 12KB(SRP 大数运算峰值, 见 srp6a.h) */
/* 目标装配 port(PSA SHA-256 + esp_random; pair_port_psa.c 提供) */
const srp_port_t *pair_port_psa(void);

int pair_size(void);

/* 初始化(port 必须含 hash/rng; NVS "pair" 区就绪后可即调用) */
int pair_init(pair_svc_t *p, const pair_port_t *port);

/* 开窗: 生成 PIN(数字 6 位, 终端 NUL)返回调用方展示 */
int pair_window_open(pair_svc_t *p, char pin_out[PAIR_PIN_LEN + 1]);
/* 窗口是否有效(TTL 内) */
int pair_window_active(pair_svc_t *p);

/* SRP 交换三步(须窗口有效; 顺序调用) */
int pair_step1(pair_svc_t *p, uint8_t salt[16], uint8_t B[384]);
int pair_step2(pair_svc_t *p, const uint8_t A[384]);
/* step3: 校验 M1; 通过则回 M2。PIN 错 = M1 不过 → 计失败,
 * 达 PAIR_MAX_FAILS 窗口作废(返回 PAIR_RES_LOCKED)。 */
int pair_step3(pair_svc_t *p, const uint8_t m1[32], uint8_t m2_out[32]);
/* 最近一次 step3 的会话密钥(SRP K; 供通道派生; 未交换返回 NULL) */
const uint8_t *pair_session_key(pair_svc_t *p);

/* 绑定: step3 通过后出示客户端公钥 → 指纹入表, 签发 client_id */
int pair_bind(pair_svc_t *p, const uint8_t pubkey[PAIR_PUBKEY_LEN],
              uint32_t *client_id_out);
/* 已配对客户端查找(按公钥指纹; mTLS 客户端证书校验用) */
int pair_client_known(pair_svc_t *p, const uint8_t pubkey[PAIR_PUBKEY_LEN],
                      uint32_t *client_id_out);
int pair_client_revoke(pair_svc_t *p, uint32_t client_id);
int pair_client_count(pair_svc_t *p);

#ifdef __cplusplus
}
#endif
#endif /* PAIR_H */
