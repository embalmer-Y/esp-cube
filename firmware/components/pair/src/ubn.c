/* ubn.c —— 无符号大数实现(见 ubn.h; T15 SRP-6a 自实现)
 *
 * 除法回避设计: mont_init 的 R/R² 派生用倍增(移位+条件减), REDC 用
 * n0inv 逐 limb 归零, 幂用平方乘 —— 全模块无通用除法(Knuth D 是大数
 * 实现的错误高发区, M1 骨架不需要)。
 */
#include <string.h>

#include "ubn.h"

#define LIM(i, n) ((i) < (n) ? (i) : 0)

void ubn_zero(ubn_t *a)
{
    a->n = 0;
    memset(a->l, 0, sizeof(a->l));
}

void ubn_from_be(ubn_t *a, const uint8_t *be, size_t len)
{
    ubn_zero(a);
    size_t lead = 0;
    while (lead < len && be[lead] == 0) lead++;   /* 剥离前导零 */
    for (size_t i = len; i-- > lead;) {
        int limb = (int)((len - 1 - i) / 4);      /* 距最低字节 4 步 = 1 limb */
        int byte = (int)((len - 1 - i) % 4);
        if (limb < UBN_MAX) {
            a->l[limb] |= (uint32_t)be[i] << (8 * byte);
            if (limb + 1 > a->n) a->n = limb + 1;
        }
    }
}

int ubn_to_be(const ubn_t *a, uint8_t *out, size_t cap)
{
    if (a->n * 4 > (int)cap) return -1;
    memset(out, 0, cap);
    for (int i = 0; i < a->n; i++) {
        for (int b = 0; b < 4; b++) {
            out[cap - 1 - i * 4 - b] =
                (uint8_t)(a->l[i] >> (8 * b));
        }
    }
    return 0;
}

int ubn_cmp(const ubn_t *a, const ubn_t *b)
{
    if (a->n != b->n) return a->n > b->n ? 1 : -1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i]) return a->l[i] > b->l[i] ? 1 : -1;
    }
    return 0;
}

void ubn_add(ubn_t *r, const ubn_t *a, const ubn_t *b)
{
    int n = a->n > b->n ? a->n : b->n;
    uint64_t k = 0;
    for (int i = 0; i < n || k; i++) {
        uint64_t t = (uint64_t)(i < n ? a->l[i] : 0)
                   + (uint64_t)(i < n ? b->l[i] : 0) + k;
        if (i < UBN_MAX) {
            r->l[i] = (uint32_t)t;
            if (i + 1 > r->n) r->n = i + 1;
        }
        k = t >> 32;
    }
}

void ubn_sub(ubn_t *r, const ubn_t *a, const ubn_t *b)
{
    /* 支持别名(r==a): 循环界先取局部, 再动 r->n */
    int n = a->n;
    int64_t borrow = 0;
    r->n = 0;
    for (int i = 0; i < n; i++) {
        int64_t t = (int64_t)a->l[i]
                  - (int64_t)(i < b->n ? b->l[i] : 0) - borrow;
        if (t < 0) {
            t += ((int64_t)1 << 32);
            borrow = 1;
        } else {
            borrow = 0;
        }
        r->l[i] = (uint32_t)t;
        if ((uint32_t)t != 0 && i + 1 > r->n) r->n = i + 1;
    }
    /* borrow!=0 表示 a<b(调用契约违反); r 值已回卷 —— 仅调试语义 */
}

void ubn_mul(ubn_t *r, const ubn_t *a, const ubn_t *b)
{
    int rn = a->n + b->n;
    if (rn > UBN_MAX) rn = UBN_MAX;
    ubn_zero(r);
    r->n = rn;
    for (int i = 0; i < a->n; i++) {
        uint64_t k = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->l[i] * b->l[j] + r->l[i + j] + k;
            r->l[i + j] = (uint32_t)t;
            k = t >> 32;
        }
        int idx = i + b->n;
        while (k && idx < UBN_MAX) {
            uint64_t t = r->l[idx] + (uint32_t)k;
            r->l[idx] = (uint32_t)t;
            k = t >> 32;
            idx++;
        }
    }
    while (r->n > 0 && r->l[r->n - 1] == 0) r->n--;   /* 剥离高零 */
}

int ubn_mod_small(ubn_t *r, const ubn_t *a, const ubn_t *m)
{
    ubn_t t;
    t = *a;
    int guard = 0;
    while (ubn_cmp(&t, m) >= 0) {
        ubn_sub(&t, &t, m);
        if (++guard > 16) return -1;      /* 超出假设域: 拒绝而非静默 */
    }
    *r = t;
    return 0;
}

int ubn_is_zero(const ubn_t *a) { return a->n == 0; }

/* ---- Montgomery ---- */

/* n0 = -(m⁻¹) mod 2³²; Newton 迭代(8→16→32 bit), m 最低位必须奇 */
static uint32_t n0inv_of(uint32_t m0)
{
    /* 8-bit 逆: 暴力找 x: m0*x ≡ 1 (mod 256) */
    uint32_t x = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (((m0 * i) & 0xFFu) == 1u) { x = i; break; }
    }
    for (int i = 0; i < 3; i++) {          /* 8 → 16 → 32 */
        x = x * (2u - m0 * x);
    }
    return (uint32_t)(0u - x);
}

/* x = x·2^times mod m(倍增, 无除法; times ≤ 32n) */
static void dbl_mod(ubn_t *x, const ubn_t *m, int times)
{
    for (int b = 0; b < times; b++) {
        uint32_t carry = 0;
        for (int i = 0; i < x->n; i++) {
            uint32_t nc = x->l[i] >> 31;
            x->l[i] = (x->l[i] << 1) | carry;
            carry = nc;
        }
        if (carry) {
            if (x->n < UBN_MAX) {
                x->l[x->n] = carry;
                x->n++;
            }
        }
        if (ubn_cmp(x, m) >= 0) ubn_sub(x, x, m);
    }
}

int ubn_mont_init(ubn_mctx_t *c, const ubn_t *m)
{
    if (ubn_is_zero(m) || (m->l[0] & 1u) == 0) return -1;   /* 需奇数模 */
    c->m = m;
    c->n0 = n0inv_of(m->l[0]);

    /* R mod m: R = 2^(32n); 从 2 倍增 32n-1 次 */
    ubn_t x;
    ubn_zero(&x);
    x.l[0] = 2;
    x.n = 1;
    dbl_mod(&x, m, 32 * m->n - 1);
    c->r = x;                              /* = R mod m */
    /* R² mod m: 对 R 再倍增 32n 次(×R) —— mont_mul(R,R) 只给
     * R²·R⁻¹ = R, 不能用(此即初版缺陷) */
    x = c->r;
    dbl_mod(&x, m, 32 * m->n);
    c->r2 = x;
    return 0;
}

void ubn_mont_mul(ubn_t *r, const ubn_t *a, const ubn_t *b,
                  const ubn_mctx_t *c)
{
    const ubn_t *m = c->m;
    uint32_t p[2 * UBN_MAX + 1];
    int n = m->n;
    int pn = a->n + b->n;
    if (pn < n) pn = n;                    /* REDC 需至少 n limb */
    if (pn > 2 * UBN_MAX) pn = 2 * UBN_MAX;
    /* 清零必须覆盖 REDC 全触达域 p[0..2n](结果窗含溢出字) —— 只清 pn
     * 会让 p[pn+1..2n] 残留上次调用的栈垃圾(初版缺陷, 差分即现形) */
    int clear = pn + 1 > 2 * n + 1 ? pn + 1 : 2 * n + 1;
    memset(p, 0, sizeof(uint32_t) * (size_t)clear);
    /* p = a*b */
    for (int i = 0; i < a->n; i++) {
        uint64_t k = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->l[i] * b->l[j] + p[i + j] + k;
            p[i + j] = (uint32_t)t;
            k = t >> 32;
        }
        int idx = i + b->n;
        while (k && idx < pn + 1) {
            uint64_t t = (uint64_t)p[idx] + k;   /* 先提升: u32+u32 会先回卷 */
            p[idx] = (uint32_t)t;
            k = t >> 32;
            idx++;
        }
    }
    /* REDC: 逐 limb 归零低位 */
    for (int i = 0; i < n; i++) {
        uint32_t u = (uint32_t)(p[i] * c->n0);
        uint64_t k = 0;
        for (int j = 0; j < n; j++) {
            uint64_t t = (uint64_t)p[i + j] + (uint64_t)u * m->l[j] + k;
            p[i + j] = (uint32_t)t;
            k = t >> 32;
        }
        int idx = i + n;
        while (k && idx < 2 * n + 1) {
            uint64_t t = (uint64_t)p[idx] + k;   /* 先提升: u32+u32 会先回卷 */
            p[idx] = (uint32_t)t;
            k = t >> 32;
            idx++;
        }
    }
    /* 结果 = p[n .. n+n-1](可能多一个溢出 limb p[2n] ∈ {0,1}) */
    ubn_t t;
    ubn_zero(&t);
    t.n = n + 1;                           /* 含溢出位参与比较 */
    for (int i = 0; i <= n; i++) t.l[i] = p[n + i];
    while (t.n > 0 && t.l[t.n - 1] == 0) t.n--;
    if (ubn_cmp(&t, m) >= 0) ubn_sub(&t, &t, m);
    *r = t;
}

void ubn_mont_exp(ubn_t *r, const ubn_t *base_mont,
                  const uint8_t *exp_be, size_t exp_len,
                  const ubn_mctx_t *c)
{
    ubn_t acc, base;
    acc = c->r;                            /* mont(1) */
    base = *base_mont;
    size_t lead = 0;
    while (lead < exp_len && exp_be[lead] == 0) lead++;
    for (size_t i = lead; i < exp_len; i++) {
        for (int b = 7; b >= 0; b--) {
            ubn_mont_mul(&acc, &acc, &acc, c);
            if ((exp_be[i] >> b) & 1u) ubn_mont_mul(&acc, &acc, &base, c);
        }
    }
    *r = acc;
}

void ubn_mont_in(ubn_t *r, const ubn_t *plain, const ubn_mctx_t *c)
{
    ubn_mont_mul(r, plain, &c->r2, c);
}

void ubn_mont_out(ubn_t *r, const ubn_t *mont, const ubn_mctx_t *c)
{
    static const uint32_t one = 1;         /* mont(1) 与 1 相乘出域 */
    ubn_t one_u;
    one_u.n = 1;
    one_u.l[0] = one;
    memset(&one_u.l[1], 0, sizeof(one_u.l) - sizeof(uint32_t));
    ubn_mont_mul(r, mont, &one_u, c);
}
