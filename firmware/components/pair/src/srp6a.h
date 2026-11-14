/* srp6a.h —— SRP-6a 协议角色实现(T15; docs/20 §3; mbedtls 4 已移除 SRP)
 *
 * 协议档(本实现定义, 客户端须一致):
 *   群     = 3072-bit 安全素数(gen_srp_group.py 本地生成, N/g 同源嵌入)
 *   k      = 3(RFC 2945 经典; 非 RFC 5054 的 H(N)|g)
 *   H      = SHA-256; K = H(S)(简化档: 非 RFC 2945 交错扩展)
 *   x      = H(s | H(user | ":" | password))   (RFC 2945)
 *   M1     = H(H(N)^H(g) | H(user) | s | A(384B) | B(384B) | K)
 *            (g 按最小表示哈希, 无填充)
 *   M2     = H(A | M1 | K)
 *   A/B/salt 传输格式: A/B 定长 384B 大端(左零填充); salt 16B
 *
 * 用法(设备=服务器):
 *   srv_begin(port, srv, pin)          → salt[16], B[384] 发客户端
 *   srv_A(srv, A[384])                 → S/K 派生(栈峰值约 8KB, 任务栈留量)
 *   srv_m1_check(srv, m1[32])          → 客户端证明校验(0=过)
 *   srv_m2(srv, m2[32])                → 服务器证明(m1 过后有效)
 * 客户端(测试/参考):
 *   cli_begin(port, cli, pin, salt, B) → A[384]
 *   cli_m1(cli, m1[32])                → 客户端证明(内部 K 派生)
 *   cli_m2_check(cli, m2[32])          → 服务器证明校验
 *
 * 时序非恒定(M1 骨架边界, 见 ubn.h 注); 密码学原语经 port 注入:
 * 目标=PSA(sha256)+esp_random; 宿主测试=独立实现+系统熵。
 */
#ifndef SRP6A_H
#define SRP6A_H

#include <stddef.h>
#include <stdint.h>

#include "srp_port.h"
#include "ubn.h"

#define SRP_SALT_LEN   16
#define SRP_ABN_LEN    384          /* A/B 定长大端 */
#define SRP_M_LEN      32           /* M1/M2 */

/* 会话堆工作区: SRP 大数中间态(单次 SRP 调用峰值 ~12KB)。
 * 栈放不下被挤占的 httpd 任务(批次 19: 16KB 内部栈分配失败实证);
 * 挂在会话上 begin 时 malloc(SPIRAM_USE_MALLOC 下大块自动落 PSRAM),
 * 角色结构体生命周期内持有。 */
typedef struct {
    ubn_t u1, u2, u3, u4, u5, u6, u7, u8, u9, u10, u11, u12, u13, u14;
    uint8_t abuf[SRP_ABN_LEN * 2];
    uint8_t e_be[400];
    uint8_t s_be[SRP_ABN_LEN];
} srp_ws_t;

#define SRP_PRIV_LEN  32             /* 私随机指数 256-bit(RFC 2945 惯例:
    私值只需 ≥2×安全强度; 3072-bit 会令 modexp 慢 12×, -Os 下超时) */
typedef struct {
    srp_port_t port;
    char user[32];
    uint8_t salt[SRP_SALT_LEN];
    uint8_t b[SRP_PRIV_LEN];           /* 私随机(srv_A 指数) */
    uint8_t v_be[SRP_ABN_LEN];         /* verifier 出域值(srv_A 重进域) */
    uint8_t A[SRP_ABN_LEN];
    uint8_t B[SRP_ABN_LEN];
    uint8_t K[SRP_M_LEN];              /* srv_A 后有效 */
    uint8_t m1[SRP_M_LEN];             /* m1_check 通过后有效(M2 用) */
    int have_A;
    srp_ws_t *ws;                      /* begin 时分配 */
} srp_srv_t;

typedef struct {
    srp_port_t port;
    char user[32];
    uint8_t salt[SRP_SALT_LEN];
    uint8_t a[SRP_PRIV_LEN];           /* 私随机 */
    uint8_t x[SRP_M_LEN];              /* 私钥指数哈希 */
    uint8_t A[SRP_ABN_LEN];
    uint8_t B[SRP_ABN_LEN];
    uint8_t K[SRP_M_LEN];              /* cli_m1 后有效 */
    uint8_t m1[SRP_M_LEN];
    srp_ws_t *ws;                      /* begin 时分配 */
} srp_cli_t;

/* 服务器: 由 PIN 生成 salt/verifier 并计算 B; 返回 0=OK */
int srp_srv_begin(srp_srv_t *s, const srp_port_t *port,
                  const char *user, const char *pin,
                  uint8_t salt_out[SRP_SALT_LEN],
                  uint8_t B_out[SRP_ABN_LEN]);
/* 服务器: 收 A 并完成 S/K 派生; A≡0/未 begin → -1 */
int srp_srv_A(srp_srv_t *s, const uint8_t A[SRP_ABN_LEN]);
/* 服务器: 校验客户端证明(恒时比较); 0=过, 非 0=认证失败 */
int srp_srv_m1_check(srp_srv_t *s, const uint8_t m1[SRP_M_LEN]);
/* 服务器证明(m1 通过后调用) */
void srp_srv_m2(const srp_srv_t *s, uint8_t m2_out[SRP_M_LEN]);

/* 客户端: 由 salt/B+PIN 计算 A; 返回 0=OK(B≡0 → -1) */
int srp_cli_begin(srp_cli_t *c, const srp_port_t *port,
                  const char *user, const char *pin,
                  const uint8_t salt[SRP_SALT_LEN],
                  const uint8_t B[SRP_ABN_LEN],
                  uint8_t A_out[SRP_ABN_LEN]);
/* 客户端证明(内部 S/K 派生; 栈峰值约 8KB) */
int srp_cli_m1(srp_cli_t *c, uint8_t m1_out[SRP_M_LEN]);
/* 校验服务器证明; 0=过 */
int srp_cli_m2_check(const srp_cli_t *c, const uint8_t m2[SRP_M_LEN]);

/* 两侧一致会话密钥(供通道密钥派生) */
static inline const uint8_t *srp_srv_key(const srp_srv_t *s)
{ return s->K; }
static inline const uint8_t *srp_cli_key(const srp_cli_t *c)
{ return c->K; }

#endif /* SRP6A_H */
