/* srp6a.c —— SRP-6a 服务器/客户端角色实现(见 srp6a.h; T15)
 *
 * 群常量占位 @N_HEX@/@G@ 由 tools/gen_srp_group.py 生成后替换,
 * 与 tools/srp_ref.py 同源(两处必须一致——差分测试即校验此一致性)。
 * 正规性档: A/B 必须为 < N 的定长 384B 大端表示, ≥N 拒绝(免除法,
 * 客户端实现同约束)。
 */
#include <stdlib.h>
#include <string.h>

#include "srp6a.h"

static const uint8_t SRP_N_BE[SRP_ABN_LEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
    0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
    0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
    0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
    0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x37, 0xed, 0x6b, 0x0b, 0xff, 0x5c, 0xb6, 0xf4, 0x06, 0xb7, 0xed,
    0xee, 0x38, 0x6b, 0xfb, 0x5a, 0x89, 0x9f, 0xa5, 0xae, 0x9f, 0x24, 0x11, 0x7c, 0x4b, 0x1f, 0xe6,
    0x49, 0x28, 0x66, 0x51, 0xec, 0xe4, 0x5b, 0x3d, 0xc2, 0x00, 0x7c, 0xb8, 0xa1, 0x63, 0xbf, 0x05,
    0x98, 0xda, 0x48, 0x36, 0x1c, 0x55, 0xd3, 0x9a, 0x69, 0x16, 0x3f, 0xa8, 0xfd, 0x24, 0xcf, 0x5f,
    0x83, 0x65, 0x5d, 0x23, 0xdc, 0xa3, 0xad, 0x96, 0x1c, 0x62, 0xf3, 0x56, 0x20, 0x85, 0x52, 0xbb,
    0x9e, 0xd5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6d, 0x67, 0x0c, 0x35, 0x4e, 0x4a, 0xbc, 0x98, 0x04,
    0xf1, 0x74, 0x6c, 0x08, 0xca, 0x18, 0x21, 0x7c, 0x32, 0x90, 0x5e, 0x46, 0x2e, 0x36, 0xce, 0x3b,
    0xe3, 0x9e, 0x77, 0x2c, 0x18, 0x0e, 0x86, 0x03, 0x9b, 0x27, 0x83, 0xa2, 0xec, 0x07, 0xa2, 0x8f,
    0xb5, 0xc5, 0x5d, 0xf0, 0x6f, 0x4c, 0x52, 0xc9, 0xde, 0x2b, 0xcb, 0xf6, 0x95, 0x58, 0x17, 0x18,
    0x39, 0x95, 0x49, 0x7c, 0xea, 0x95, 0x6a, 0xe5, 0x15, 0xd2, 0x26, 0x18, 0x98, 0xfa, 0x05, 0x10,
    0x15, 0x72, 0x8e, 0x5a, 0x8a, 0xaa, 0xc4, 0x2d, 0xad, 0x33, 0x17, 0x0d, 0x04, 0x50, 0x7a, 0x33,
    0xa8, 0x55, 0x21, 0xab, 0xdf, 0x1c, 0xba, 0x64, 0xec, 0xfb, 0x85, 0x04, 0x58, 0xdb, 0xef, 0x0a,
    0x8a, 0xea, 0x71, 0x57, 0x5d, 0x06, 0x0c, 0x7d, 0xb3, 0x97, 0x0f, 0x85, 0xa6, 0xe1, 0xe4, 0xc7,
    0xab, 0xf5, 0xae, 0x8c, 0xdb, 0x09, 0x33, 0xd7, 0x1e, 0x8c, 0x94, 0xe0, 0x4a, 0x25, 0x61, 0x9d,
    0xce, 0xe3, 0xd2, 0x26, 0x1a, 0xd2, 0xee, 0x6b, 0xf1, 0x2f, 0xfa, 0x06, 0xd9, 0x8a, 0x08, 0x64,
    0xd8, 0x76, 0x02, 0x73, 0x3e, 0xc8, 0x6a, 0x64, 0x52, 0x1f, 0x2b, 0x18, 0x17, 0x7b, 0x20, 0x0c,
    0xbb, 0xe1, 0x17, 0x57, 0x7a, 0x61, 0x5d, 0x6c, 0x77, 0x09, 0x88, 0xc0, 0xba, 0xd9, 0x46, 0xe2,
    0x08, 0xe2, 0x4f, 0xa0, 0x74, 0xe5, 0xab, 0x31, 0x43, 0xdb, 0x5b, 0xfc, 0xe0, 0xfd, 0x10, 0x8e,
    0x4b, 0x82, 0xd1, 0x20, 0xa9, 0x3a, 0xd2, 0xca, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
#define SRP_G_U32 5u

/* ---- 内部 ---- */

#define H_BUF_MAX 900              /* M1 拼装上界: 32+32+16+384+384+32=880 */

static void hash_of(const srp_port_t *p, const uint8_t *d, size_t n,
                    uint8_t out[32])
{
    p->hash(p->ud, d, n, out);
}

/* x = H(salt | H(user ":" password)); user/pin 定长界(配对入口 6 位数字) */
static void srp_x(const srp_port_t *p, const char *user,
                  const char *pin, const uint8_t salt[SRP_SALT_LEN],
                  uint8_t x_out[32])
{
    char line[32 + 1 + 64];
    size_t ul = strlen(user), pl = strlen(pin);
    if (ul > 32) ul = 32;
    if (pl > 64) pl = 64;
    memcpy(line, user, ul);
    line[ul] = ':';
    memcpy(line + ul + 1, pin, pl);
    uint8_t inner[32];
    hash_of(p, (const uint8_t *)line, ul + 1 + pl, inner);
    uint8_t buf[SRP_SALT_LEN + 32];
    memcpy(buf, salt, SRP_SALT_LEN);
    memcpy(buf + SRP_SALT_LEN, inner, 32);
    hash_of(p, buf, sizeof(buf), x_out);
}

/* M1 = H(H(N)^H(g) | H(user) | salt | A | B | K); g 按最小表示哈希 */
static void srp_m1(const srp_port_t *p, const char *user,
                   const uint8_t salt[SRP_SALT_LEN],
                   const uint8_t A[SRP_ABN_LEN],
                   const uint8_t B[SRP_ABN_LEN],
                   const uint8_t K[SRP_M_LEN],
                   uint8_t m1_out[SRP_M_LEN])
{
    uint8_t hn[32], hg[32], hu[32];
    hash_of(p, SRP_N_BE, SRP_ABN_LEN, hn);
    uint32_t g = SRP_G_U32;
    uint8_t gbe[4] = {
        (uint8_t)(g >> 24), (uint8_t)(g >> 16),
        (uint8_t)(g >> 8), (uint8_t)g,
    };
    int g0 = 0;
    while (g0 < 3 && gbe[g0] == 0) g0++;   /* 去前导零 → 最小表示 */
    hash_of(p, gbe + g0, (size_t)(4 - g0), hg);
    for (int i = 0; i < 32; i++) hn[i] ^= hg[i];
    hash_of(p, (const uint8_t *)user, strlen(user), hu);
    uint8_t buf[H_BUF_MAX];
    size_t off = 0;
    memcpy(buf + off, hn, 32); off += 32;
    memcpy(buf + off, hu, 32); off += 32;
    memcpy(buf + off, salt, SRP_SALT_LEN); off += SRP_SALT_LEN;
    memcpy(buf + off, A, SRP_ABN_LEN); off += SRP_ABN_LEN;
    memcpy(buf + off, B, SRP_ABN_LEN); off += SRP_ABN_LEN;
    memcpy(buf + off, K, SRP_M_LEN); off += SRP_M_LEN;
    hash_of(p, buf, off, m1_out);
}

/* M2 = H(A | M1 | K) */
static void srp_m2(const srp_port_t *p, const uint8_t A[SRP_ABN_LEN],
                   const uint8_t m1[SRP_M_LEN],
                   const uint8_t K[SRP_M_LEN], uint8_t m2_out[SRP_M_LEN])
{
    uint8_t buf[SRP_ABN_LEN + SRP_M_LEN + SRP_M_LEN];
    memcpy(buf, A, SRP_ABN_LEN);
    memcpy(buf + SRP_ABN_LEN, m1, SRP_M_LEN);
    memcpy(buf + SRP_ABN_LEN + SRP_M_LEN, K, SRP_M_LEN);
    hash_of(p, buf, sizeof(buf), m2_out);
}

static int ct_cmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff != 0 ? -1 : 0;
}

/* 惰性 mont 上下文(配对单流, 无并发) */
static ubn_t s_N;
static ubn_mctx_t s_mc;
static int s_mc_ready;

static int mont_ready(void)
{
    if (!s_mc_ready) {
        ubn_from_be(&s_N, SRP_N_BE, SRP_ABN_LEN);
        if (ubn_mont_init(&s_mc, &s_N) != 0) return -1;
        s_mc_ready = 1;
    }
    return 0;
}

static void g_ubn(ubn_t *g)
{
    ubn_zero(g);
    g->l[0] = SRP_G_U32;
    g->n = 1;
}

/* t = 3*plain mod N(plain < N; 结果 ≤ 2 次条件减) */
static void triple_mod(ubn_t *t, const ubn_t *plain)
{
    ubn_add(t, plain, plain);
    ubn_add(t, t, plain);
    (void)ubn_mod_small(t, t, &s_N);       /* ≤3m, guard=16 充裕 */
}

/* base = (B - 3·g^x) mod N(调用方保证两入参 < N) */
static void mod_sub(ubn_t *r, const ubn_t *a, const ubn_t *b)
{
    if (ubn_cmp(a, b) >= 0) {
        ubn_sub(r, a, b);
    } else {
        ubn_t an;
        ubn_add(&an, a, &s_N);             /* a+N < 2N */
        ubn_sub(r, &an, b);
    }
}

static void user_copy(char *dst, size_t cap, const char *user)
{
    const char *u = (user != NULL && user[0] != '\0') ? user : "espcube";
    size_t ul = strlen(u);
    if (ul > cap - 1) ul = cap - 1;
    memcpy(dst, u, ul);
    dst[ul] = '\0';
}

/* ---- 服务器 ---- */

int srp_srv_begin(srp_srv_t *s, const srp_port_t *port,
                  const char *user, const char *pin,
                  uint8_t salt_out[SRP_SALT_LEN],
                  uint8_t B_out[SRP_ABN_LEN])
{
    if (s == NULL || port == NULL || port->hash == NULL ||
        port->rng == NULL || pin == NULL || pin[0] == '\0') {
        return -1;
    }
    /* ws 生命周期契约: 会话所有方(包含栈上)须先零初始化; pair_svc
     * 重开窗路径显式保留指针(见 pair_window_open), 此处按 NULL 判定
     * 分配 —— 大块: SPIRAM_USE_MALLOC 下自动落 PSRAM */
    memset(s, 0, sizeof(*s));
    if (s->ws == NULL) {
        s->ws = malloc(sizeof(srp_ws_t));
        if (s->ws == NULL) return -1;
    }
    s->port = *port;
    user_copy(s->user, sizeof(s->user), user);
    if (mont_ready() != 0) return -1;
    if (port->rng(port->ud, s->salt, SRP_SALT_LEN) != 0) return -1;
    memcpy(salt_out, s->salt, SRP_SALT_LEN);

    /* v = g^x mod N;  B = 3v + g^b mod N (中间态在工作区, 不占任务栈) */
    srp_ws_t *w = s->ws;
    ubn_t *g = &w->u1, *g_m = &w->u2, *v_m = &w->u3, *gb_m = &w->u4,
          *v_out = &w->u5, *gb_out = &w->u6, *t3 = &w->u7;
    uint8_t x[32];
    srp_x(&s->port, s->user, pin, s->salt, x);
    g_ubn(g);
    ubn_mont_in(g_m, g, &s_mc);
    ubn_mont_exp(v_m, g_m, x, 32, &s_mc);
    ubn_mont_out(v_out, v_m, &s_mc);
    ubn_to_be(v_out, s->v_be, SRP_ABN_LEN);
    if (port->rng(port->ud, s->b, SRP_PRIV_LEN) != 0) return -1;
    ubn_mont_exp(gb_m, g_m, s->b, SRP_PRIV_LEN, &s_mc);
    ubn_mont_out(gb_out, gb_m, &s_mc);
    triple_mod(t3, v_out);
    ubn_add(v_out, t3, gb_out);            /* < 3N + N, 回模 */
    (void)ubn_mod_small(v_out, v_out, &s_N);
    if (ubn_is_zero(v_out)) return -1;     /* B≢0(防御, 概率忽略) */
    ubn_to_be(v_out, s->B, SRP_ABN_LEN);
    memcpy(B_out, s->B, SRP_ABN_LEN);
    return 0;
}

int srp_srv_A(srp_srv_t *s, const uint8_t A[SRP_ABN_LEN])
{
    if (s == NULL || A == NULL || !s_mc_ready ||
        (!s->b[SRP_PRIV_LEN - 1] && !s->v_be[SRP_ABN_LEN - 1])) {
        return -1;                          /* begin 未调(b/v 全零粗查) */
    }
    srp_ws_t *w = s->ws;
    ubn_t *Au = &w->u1, *A_m = &w->u2, *v_p = &w->u3, *v_m = &w->u4,
          *vu_m = &w->u5, *t = &w->u6, *S_m = &w->u7, *S = &w->u8;
    ubn_from_be(Au, A, SRP_ABN_LEN);
    if (ubn_is_zero(Au) || ubn_cmp(Au, &s_N) >= 0) return -1;
    memcpy(s->A, A, SRP_ABN_LEN);

    uint8_t u[32];
    memcpy(w->abuf, s->A, SRP_ABN_LEN);
    memcpy(w->abuf + SRP_ABN_LEN, s->B, SRP_ABN_LEN);
    hash_of(&s->port, w->abuf, sizeof(w->abuf), u);

    ubn_mont_in(A_m, Au, &s_mc);
    ubn_from_be(v_p, s->v_be, SRP_ABN_LEN);
    ubn_mont_in(v_m, v_p, &s_mc);
    ubn_mont_exp(vu_m, v_m, u, 32, &s_mc);
    ubn_mont_mul(t, A_m, vu_m, &s_mc);
    ubn_mont_exp(S_m, t, s->b, SRP_PRIV_LEN, &s_mc);
    ubn_mont_out(S, S_m, &s_mc);
    ubn_to_be(S, w->s_be, SRP_ABN_LEN);
    hash_of(&s->port, w->s_be, SRP_ABN_LEN, s->K);
    s->have_A = 1;
    return 0;
}

int srp_srv_m1_check(srp_srv_t *s, const uint8_t m1[SRP_M_LEN])
{
    if (s == NULL || m1 == NULL || !s->have_A) return -1;
    uint8_t expect[SRP_M_LEN];
    srp_m1(&s->port, s->user, s->salt, s->A, s->B, s->K, expect);
    if (ct_cmp(expect, m1, SRP_M_LEN) != 0) return -1;
    memcpy(s->m1, m1, SRP_M_LEN);
    return 0;
}

void srp_srv_m2(const srp_srv_t *s, uint8_t m2_out[SRP_M_LEN])
{
    if (s == NULL || !s->have_A) return;
    srp_m2(&s->port, s->A, s->m1, s->K, m2_out);
}

/* ---- 客户端 ---- */

int srp_cli_begin(srp_cli_t *c, const srp_port_t *port,
                  const char *user, const char *pin,
                  const uint8_t salt[SRP_SALT_LEN],
                  const uint8_t B[SRP_ABN_LEN],
                  uint8_t A_out[SRP_ABN_LEN])
{
    if (c == NULL || port == NULL || port->hash == NULL ||
        port->rng == NULL || pin == NULL || pin[0] == '\0' ||
        salt == NULL || B == NULL) {
        return -1;
    }
    if (mont_ready() != 0) return -1;
    ubn_t Bu;
    ubn_from_be(&Bu, B, SRP_ABN_LEN);
    if (ubn_is_zero(&Bu) || ubn_cmp(&Bu, &s_N) >= 0) return -1;
    memset(c, 0, sizeof(*c));
    if (c->ws == NULL) {
        c->ws = malloc(sizeof(srp_ws_t));
        if (c->ws == NULL) return -1;
    }
    c->port = *port;
    user_copy(c->user, sizeof(c->user), user);
    memcpy(c->salt, salt, SRP_SALT_LEN);
    memcpy(c->B, B, SRP_ABN_LEN);
    srp_x(&c->port, c->user, pin, c->salt, c->x);
    if (port->rng(port->ud, c->a, SRP_PRIV_LEN) != 0) return -1;

    ubn_t g, g_m, A_m, A;
    g_ubn(&g);
    ubn_mont_in(&g_m, &g, &s_mc);
    ubn_mont_exp(&A_m, &g_m, c->a, SRP_PRIV_LEN, &s_mc);
    ubn_mont_out(&A, &A_m, &s_mc);
    ubn_to_be(&A, c->A, SRP_ABN_LEN);
    memcpy(A_out, c->A, SRP_ABN_LEN);
    return 0;
}

int srp_cli_m1(srp_cli_t *c, uint8_t m1_out[SRP_M_LEN])
{
    if (c == NULL || m1_out == NULL ||
        (c->A[SRP_ABN_LEN - 1] == 0 && c->x[0] == 0 && c->x[31] == 0)) {
        return -1;                          /* begin 未调(粗查) */
    }
    srp_ws_t *w = c->ws;
    ubn_t *Bu = &w->u1, *g = &w->u2, *g_m = &w->u3, *gx_m = &w->u4,
          *gx = &w->u5, *k3 = &w->u6, *base = &w->u7, *base_m = &w->u8,
          *u_u = &w->u9, *x_u = &w->u10, *ux = &w->u11, *e = &w->u12,
          *S_m = &w->u13, *S = &w->u14;
    uint8_t u[32];

    /* u = H(A|B); base = B - 3·g^x mod N */
    memcpy(w->abuf, c->A, SRP_ABN_LEN);
    memcpy(w->abuf + SRP_ABN_LEN, c->B, SRP_ABN_LEN);
    hash_of(&c->port, w->abuf, sizeof(w->abuf), u);
    ubn_from_be(Bu, c->B, SRP_ABN_LEN);
    g_ubn(g);
    ubn_mont_in(g_m, g, &s_mc);
    ubn_mont_exp(gx_m, g_m, c->x, 32, &s_mc);
    ubn_mont_out(gx, gx_m, &s_mc);
    triple_mod(k3, gx);
    mod_sub(base, Bu, k3);

    /* e = a + u·x(不作约减, RFC 2945) */
    ubn_from_be(u_u, u, 32);
    ubn_from_be(x_u, c->x, 32);
    ubn_mul(ux, u_u, x_u);
    ubn_from_be(e, c->a, SRP_PRIV_LEN);
    ubn_add(e, e, ux);
    if (ubn_to_be(e, w->e_be, sizeof(w->e_be)) != 0) return -1;

    ubn_mont_in(base_m, base, &s_mc);
    ubn_mont_exp(S_m, base_m, w->e_be, sizeof(w->e_be), &s_mc);
    ubn_mont_out(S, S_m, &s_mc);
    ubn_to_be(S, w->s_be, SRP_ABN_LEN);
    hash_of(&c->port, w->s_be, SRP_ABN_LEN, c->K);
    srp_m1(&c->port, c->user, c->salt, c->A, c->B, c->K, c->m1);
    memcpy(m1_out, c->m1, SRP_M_LEN);
    return 0;
}

int srp_cli_m2_check(const srp_cli_t *c, const uint8_t m2[SRP_M_LEN])
{
    if (c == NULL || m2 == NULL ||
        (c->m1[0] == 0 && c->m1[1] == 0 && c->m1[2] == 0
         && c->m1[3] == 0)) {
        return -1;                          /* m1 未调(粗查) */
    }
    uint8_t expect[SRP_M_LEN];
    srp_m2(&c->port, c->A, c->m1, c->K, expect);
    return ct_cmp(expect, m2, SRP_M_LEN);
}
