/* ubn.h —— 无符号大数(固定容量, 无堆分配; SRP-6a 专用, T15)
 *
 * 容量 UBN_MAX=200 limbs(u32) = 6400 bits: 容纳 3072-bit 群的完整
 * 乘积(192 limbs)+进位冗余。域算术走 Montgomery(模数必须奇——安全
 * 素数恒奇), 全流程无 Knuth 除法:
 *   mont_init   模数/n0inv/R mod m/R² mod m(倍增法派生, 无除法)
 *   mont_mul    (a*b*R⁻¹) mod m —— 全积 + 粗粒度 REDC
 *   mont_exp    平方乘(指数任意长度字节串, 不需约减)
 *   mont_in/out mont 域进出
 * 运算均非常数时间(M1 骨架边界: 配对一次性+近场假设, 时序侧信道
 * 不在本批威胁模型内——docs/20 §8 M2 再议)。
 */
#ifndef UBN_H
#define UBN_H

#include <stddef.h>
#include <stdint.h>

#define UBN_MAX 200

typedef struct {
    int n;                          /* 有效 limb 数(0 = 值 0) */
    uint32_t l[UBN_MAX];
} ubn_t;

/* Montgomery 上下文(m 必须奇数) */
typedef struct {
    const ubn_t *m;                 /* 模数 */
    uint32_t n0;                    /* -m⁻¹ mod 2³² */
    ubn_t r;                        /* R mod m(mont 域的 1) */
    ubn_t r2;                       /* R² mod m(进域乘子) */
} ubn_mctx_t;

void ubn_zero(ubn_t *a);
/* 大端字节串 → ubn(高位零自动剥离) */
void ubn_from_be(ubn_t *a, const uint8_t *be, size_t len);
/* ubn → 大端, 左填充至 cap(值超出 cap 返回 -1); 成功 0 */
int ubn_to_be(const ubn_t *a, uint8_t *out, size_t cap);
int ubn_cmp(const ubn_t *a, const ubn_t *b);
void ubn_add(ubn_t *r, const ubn_t *a, const ubn_t *b);
void ubn_sub(ubn_t *r, const ubn_t *a, const ubn_t *b);   /* 要求 a>=b */
void ubn_mul(ubn_t *r, const ubn_t *a, const ubn_t *b);   /* r≠a,b */
/* 模约减: r = a mod m(仅用条件减法, 假设 a < m³ —— 配对流程内
 * a 至多 3m, 循环 ≤3 次; 超出返回 -1 不支持) */
int ubn_mod_small(ubn_t *r, const ubn_t *a, const ubn_t *m);
int ubn_is_zero(const ubn_t *a);

int ubn_mont_init(ubn_mctx_t *c, const ubn_t *m);
/* r = a*b*R⁻¹ mod m(a,b 已在 mont 域; r 可与 a/b 同域独立) */
void ubn_mont_mul(ubn_t *r, const ubn_t *a, const ubn_t *b,
                  const ubn_mctx_t *c);
/* mont 域幂: r = (base^exp)*R mod m; exp 为大端字节串(任意长) */
void ubn_mont_exp(ubn_t *r, const ubn_t *base_mont,
                  const uint8_t *exp_be, size_t exp_len,
                  const ubn_mctx_t *c);
/* mont 域进出 */
void ubn_mont_in(ubn_t *r, const ubn_t *plain, const ubn_mctx_t *c);
void ubn_mont_out(ubn_t *r, const ubn_t *mont, const ubn_mctx_t *c);

#endif /* UBN_H */
