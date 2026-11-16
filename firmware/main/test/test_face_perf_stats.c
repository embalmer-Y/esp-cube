/*
 * test_face_perf_stats.c —— 表情帧耗时统计宿主验收(任务 4.1)
 *
 * 编译运行(tools/host_tests.sh [18] 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
 *       -I firmware/main -o /tmp/test_face_perf_stats \
 *       firmware/main/test/test_face_perf_stats.c firmware/main/face_perf_stats.c \
 *   && /tmp/test_face_perf_stats
 *
 * 契约见 face_perf_stats.h。覆盖:
 * - n=0、n=1、n=2、重复值、阈值边界(严格大于)、极值 0/UINT32_MAX、参数非法
 * - 取整规则: n=1..4096 的 P50/P95 下标与 CLI `baseline` 表达式 n/2、(n*95)/100
 *   (int 运算, 与 cli.c 同写法)逐一相等且 < n; 已排序 0..n-1 样本取值即下标
 * - 样本环: 回绕覆盖、pushed/len、按列从旧到新、out_cap 不足时取最新、reset
 * - PBT(固定种子 xorshift32): 汇总 2000 例与"qsort + 同一下标规则 + 线性计数"
 *   参考实现对拍(含 vals 被排成输入的有序排列); 样本环 1000 例与"保存全部
 *   push 记录的朴素模型"对拍。缓冲都用恰好大小的堆分配, 越界由 ASan 捕获
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "face_perf_stats.h"

static int s_fail;
static int s_checks;
#define CHECK(cond, ...) do { \
    s_checks++; \
    if (!(cond)) { \
        printf("  FAIL %s:%d ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); s_fail++; \
    } \
} while (0)

static uint32_t s_rng = 0x2611150Fu;                   /* 固定种子 */
static uint32_t xs32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static const uint32_t THR_US[] = FACE_PERF_STATS_THR_US_INIT;

/* 汇总一份拷贝, 原数组不动 */
static face_perf_summary_t summ(const uint32_t *v, uint32_t n,
                                const uint32_t *thr, uint32_t nthr)
{
    face_perf_summary_t s;
    uint32_t *c = malloc((n ? n : 1) * sizeof(uint32_t));
    if (n) memcpy(c, v, n * sizeof(uint32_t));
    int rc = face_perf_summarize(n ? c : NULL, n, thr, nthr, &s);
    CHECK(rc == 0, "summarize rc=%d (n=%u)", rc, n);
    free(c);
    return s;
}

static void test_examples(void)
{
    face_perf_summary_t s;

    /* n=0 */
    s = summ(NULL, 0, THR_US, FACE_PERF_STATS_THR_US_COUNT);
    CHECK(s.n == 0 && s.p50 == 0 && s.p95 == 0 && s.max == 0 &&
          s.over[0] == 0 && s.over[1] == 0 && s.over[2] == 0 && s.nthr == 3 &&
          s.thr[0] == 33000 && s.thr[1] == 66000 && s.thr[2] == 100000,
          "n=0 → all zero, thr copied");

    /* n=1 */
    const uint32_t one[] = {40000};
    s = summ(one, 1, THR_US, 3);
    CHECK(s.n == 1 && s.p50 == 40000 && s.p95 == 40000 && s.max == 40000,
          "n=1 → p50=p95=max=v (got %u/%u/%u)", s.p50, s.p95, s.max);
    CHECK(s.over[0] == 1 && s.over[1] == 0 && s.over[2] == 0,
          "n=1 over = 1/0/0 (got %u/%u/%u)", s.over[0], s.over[1], s.over[2]);

    /* n=2: P50 = s[1](较大者), P95 = s[(2*95)/100] = s[1] */
    const uint32_t two[] = {5000, 3000};
    s = summ(two, 2, NULL, 0);
    CHECK(s.n == 2 && s.p50 == 5000 && s.p95 == 5000 && s.max == 5000,
          "n=2 [5000,3000] → p50=p95=max=5000 (got %u/%u/%u)", s.p50, s.p95,
          s.max);

    /* 重复值 */
    const uint32_t rep[] = {4, 4, 4, 4, 4};
    const uint32_t thr_rep[] = {3, 4, 5};
    s = summ(rep, 5, thr_rep, 3);
    CHECK(s.p50 == 4 && s.p95 == 4 && s.max == 4, "repeated → 4/4/4");
    CHECK(s.over[0] == 5 && s.over[1] == 0 && s.over[2] == 0,
          "repeated over thr 3/4/5 = 5/0/0 (strict >) (got %u/%u/%u)",
          s.over[0], s.over[1], s.over[2]);

    const uint32_t rep2[] = {9, 1, 9, 1, 9, 1, 9, 1, 9, 9};  /* 4×1, 6×9 */
    s = summ(rep2, 10, NULL, 0);
    CHECK(s.p50 == 9 && s.p95 == 9 && s.max == 9,
          "4x1+6x9 → p50 s[5]=9 (got %u/%u/%u)", s.p50, s.p95, s.max);

    /* 阈值边界: 恰好等于阈值不计 */
    const uint32_t edge[] = {33000, 33001, 66000, 66001, 100000, 100001, 0};
    s = summ(edge, 7, THR_US, 3);
    CHECK(s.over[0] == 5 && s.over[1] == 3 && s.over[2] == 1,
          "edge over = 5/3/1 (got %u/%u/%u)", s.over[0], s.over[1], s.over[2]);
    CHECK(s.max == 100001 && s.p50 == 66000,
          "edge max=100001 p50=s[3]=66000 (got %u/%u)", s.max, s.p50);

    /* 极值 */
    const uint32_t ext[] = {UINT32_MAX, 0, UINT32_MAX, 0};
    const uint32_t thr_ext[] = {0, UINT32_MAX - 1, UINT32_MAX};
    s = summ(ext, 4, thr_ext, 3);
    CHECK(s.p50 == UINT32_MAX && s.max == UINT32_MAX,
          "extremes p50=s[2]=MAX max=MAX");
    CHECK(s.over[0] == 2 && s.over[1] == 2 && s.over[2] == 0,
          "extremes over = 2/2/0 (got %u/%u/%u)", s.over[0], s.over[1],
          s.over[2]);

    /* 排序副作用: vals 被原地升序 */
    uint32_t v[] = {7, 3, 9, 1, 5};
    face_perf_summarize(v, 5, NULL, 0, &s);
    CHECK(v[0] == 1 && v[1] == 3 && v[2] == 5 && v[3] == 7 && v[4] == 9,
          "vals sorted in place");

    /* 参数非法 */
    uint32_t keep[] = {3, 2, 1};
    const uint32_t thr5[] = {1, 2, 3, 4, 5};
    CHECK(face_perf_summarize(keep, 3, NULL, 0, NULL) == -1, "out NULL → -1");
    CHECK(face_perf_summarize(NULL, 3, NULL, 0, &s) == -1,
          "vals NULL n>0 → -1");
    CHECK(face_perf_summarize(keep, 3, NULL, 1, &s) == -1,
          "thr NULL nthr>0 → -1");
    CHECK(face_perf_summarize(keep, 3, thr5, 5, &s) == -1, "nthr 5 → -1");
    CHECK(keep[0] == 3 && keep[1] == 2 && keep[2] == 1,
          "vals untouched on error");
    CHECK(face_perf_summarize(NULL, 0, thr5, 4, &s) == 0 && s.nthr == 4,
          "vals NULL n=0 → ok");
}

/* 取整规则: 与 cli.c baseline 的 int 表达式逐一相等 */
static void test_index_rule(void)
{
    int bad = 0;
    for (int n = 1; n <= 4096; n++) {
        uint32_t i50 = face_perf_idx_p50((uint32_t)n);
        uint32_t i95 = face_perf_idx_p95((uint32_t)n);
        if (i50 != (uint32_t)(n / 2) || i95 != (uint32_t)((n * 95) / 100) ||
            i50 >= (uint32_t)n || i95 >= (uint32_t)n) {
            if (bad++ < 3) printf("  n=%d i50=%u i95=%u\n", n, i50, i95);
        }
    }
    CHECK(bad == 0, "idx rule n=1..4096 == baseline n/2, (n*95)/100, < n "
          "(%d bad)", bad);
    CHECK(face_perf_idx_p95(UINT32_MAX) == 4080218930u &&
          face_perf_idx_p95(UINT32_MAX) < UINT32_MAX,
          "idx p95 64-bit at n=UINT32_MAX (got %u)",
          face_perf_idx_p95(UINT32_MAX));

    /* 已知点: 样本为打乱的 0..n-1, 统计值即下标 */
    const struct { uint32_t n, p50, p95; } pts[] = {
        {1, 0, 0}, {2, 1, 1}, {3, 1, 2}, {10, 5, 9}, {19, 9, 18},
        {20, 10, 19}, {21, 10, 19}, {100, 50, 95}, {101, 50, 95},
        {300, 150, 285}, {1024, 512, 972},
    };
    for (size_t t = 0; t < sizeof(pts) / sizeof(pts[0]); t++) {
        uint32_t n = pts[t].n;
        uint32_t *v = malloc(n * sizeof(uint32_t));
        for (uint32_t i = 0; i < n; i++) v[i] = i;
        for (uint32_t i = n; i > 1; i--) {           /* Fisher-Yates */
            uint32_t j = xs32() % i, tmp = v[i - 1];
            v[i - 1] = v[j]; v[j] = tmp;
        }
        face_perf_summary_t s = summ(v, n, NULL, 0);
        CHECK(s.n == n && s.p50 == pts[t].p50 && s.p95 == pts[t].p95 &&
              s.max == n - 1, "n=%u → p50=%u p95=%u max=%u (got %u/%u/%u)",
              n, pts[t].p50, pts[t].p95, n - 1, s.p50, s.p95, s.max);
        free(v);
    }
}

static void test_ring_examples(void)
{
    face_perf_ring_t r;
    uint32_t *buf = malloc(4 * 2 * sizeof(uint32_t));
    CHECK(face_perf_ring_init(&r, buf, 4, 2) == 0, "init 4x2");
    uint32_t out[8];
    CHECK(face_perf_ring_column(&r, 0, out, 8) == 0, "empty column → 0");

    for (uint32_t i = 1; i <= 6; i++) {
        uint32_t rec[2] = {i, 100 + i};
        face_perf_ring_push(&r, rec);
    }
    CHECK(r.len == 4 && r.pushed == 6, "after 6 pushes len=4 pushed=6");
    uint32_t m = face_perf_ring_column(&r, 0, out, 8);
    CHECK(m == 4 && out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6,
          "col0 oldest→newest 3..6");
    m = face_perf_ring_column(&r, 1, out, 8);
    CHECK(m == 4 && out[0] == 103 && out[3] == 106, "col1 103..106");
    m = face_perf_ring_column(&r, 0, out, 2);
    CHECK(m == 2 && out[0] == 5 && out[1] == 6, "out_cap 2 → newest 5,6");
    CHECK(face_perf_ring_column(&r, 2, out, 8) == 0, "col out of range → 0");

    face_perf_summary_t s;
    const uint32_t thr[] = {4};
    CHECK(face_perf_ring_summarize(&r, 1, out, 8, thr, 1, &s) == 0 &&
          s.n == 4 && s.p50 == 105 && s.p95 == 106 && s.max == 106 &&
          s.over[0] == 4, "ring_summarize col1");
    CHECK(face_perf_ring_summarize(&r, 2, out, 8, NULL, 0, &s) == -1,
          "ring_summarize col out of range → -1");
    CHECK(face_perf_ring_summarize(&r, 0, NULL, 8, NULL, 0, &s) == -1,
          "ring_summarize scratch NULL → -1");

    face_perf_ring_reset(&r);
    CHECK(r.len == 0 && r.pushed == 0 && r.cap == 4 && r.ncol == 2 &&
          face_perf_ring_column(&r, 0, out, 8) == 0, "reset");
    CHECK(face_perf_ring_summarize(&r, 0, out, 8, NULL, 0, &s) == 0 &&
          s.n == 0, "summarize empty ring → n=0");

    /* 非法 init → 惰性状态, push 为空操作 */
    face_perf_ring_t z;
    uint32_t one[1] = {9};
    CHECK(face_perf_ring_init(&z, NULL, 4, 1) == -1 && z.cap == 0,
          "init buf NULL → -1");
    CHECK(face_perf_ring_init(&z, buf, 0, 1) == -1, "init cap 0 → -1");
    CHECK(face_perf_ring_init(&z, buf, 4, 0) == -1, "init ncol 0 → -1");
    CHECK(face_perf_ring_init(&z, buf, 4, FACE_PERF_STATS_MAX_COL + 1) == -1,
          "init ncol > max → -1");
    CHECK(face_perf_ring_init(&z, buf, 0x80000000u, 2) == -1,
          "init cap*ncol overflow → -1");
    CHECK(face_perf_ring_init(NULL, buf, 4, 1) == -1, "init r NULL → -1");
    face_perf_ring_push(&z, one);
    CHECK(z.len == 0 && z.pushed == 0, "push on inert ring is no-op");
    face_perf_ring_push(&r, NULL);
    CHECK(r.len == 0, "push rec NULL is no-op");

    /* pushed 饱和 */
    face_perf_ring_init(&z, buf, 1, 1);
    z.pushed = UINT32_MAX - 1;
    face_perf_ring_push(&z, one);
    face_perf_ring_push(&z, one);
    CHECK(z.pushed == UINT32_MAX && z.len == 1, "pushed saturates");
    free(buf);
}

/* PBT 1: 汇总 vs qsort 参考实现 */
static void pbt_summarize(void)
{
    int bad = 0;
    for (int it = 0; it < 2000; it++) {
        uint32_t n = (it % 50 == 0) ? 0 : 1 + xs32() % 1500;
        uint32_t mode = xs32() % 4;
        uint32_t *v = malloc((n ? n : 1) * sizeof(uint32_t));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t x = xs32();
            switch (mode) {
            case 0: v[i] = x % 8; break;                 /* 大量重复 */
            case 1: v[i] = x % 150000; break;            /* 0..150ms(微秒) */
            case 2: v[i] = x; break;                     /* 全 uint32_t */
            default: v[i] = (x & 1) ? 33000 + x % 3 : x % 200000; break;
            }
        }
        uint32_t nthr = xs32() % (FACE_PERF_STATS_MAX_THR + 1);
        uint32_t thr[FACE_PERF_STATS_MAX_THR];
        for (uint32_t k = 0; k < nthr; k++) {
            /* 一半取样本中的值, 卡严格大于的边界 */
            thr[k] = (n && (xs32() & 1)) ? v[xs32() % n] : xs32() % 200000;
        }

        /* 参考: qsort + 同一下标规则 + 原数组线性计数 */
        uint32_t *ref = malloc((n ? n : 1) * sizeof(uint32_t));
        if (n) memcpy(ref, v, n * sizeof(uint32_t));
        qsort(ref, n, sizeof(uint32_t), cmp_u32);
        uint32_t over[FACE_PERF_STATS_MAX_THR] = {0};
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t k = 0; k < nthr; k++)
                if (v[i] > thr[k]) over[k]++;

        uint32_t *got = malloc((n ? n : 1) * sizeof(uint32_t));
        if (n) memcpy(got, v, n * sizeof(uint32_t));
        face_perf_summary_t s;
        int rc = face_perf_summarize(n ? got : NULL, n, thr, nthr, &s);

        int ok = rc == 0 && s.n == n && s.nthr == nthr;
        if (ok && n) {
            ok = s.p50 == ref[n / 2] && s.p95 == ref[(n * 95) / 100] &&
                 s.max == ref[n - 1] &&
                 memcmp(got, ref, n * sizeof(uint32_t)) == 0;
        } else if (ok) {
            ok = s.p50 == 0 && s.p95 == 0 && s.max == 0;
        }
        for (uint32_t k = 0; ok && k < nthr; k++)
            ok = s.thr[k] == thr[k] && s.over[k] == over[k];
        for (uint32_t k = nthr; ok && k < FACE_PERF_STATS_MAX_THR; k++)
            ok = s.over[k] == 0;
        if (!ok && bad++ < 3)
            printf("  case %d n=%u mode=%u nthr=%u: got p50=%u p95=%u max=%u\n",
                   it, n, mode, nthr, s.p50, s.p95, s.max);
        free(v); free(ref); free(got);
    }
    CHECK(bad == 0, "PBT summarize 2000 vs qsort reference (%d bad)", bad);
}

/* PBT 2: 样本环 vs 保存全部记录的朴素模型 */
static void pbt_ring(void)
{
    int bad = 0;
    for (int it = 0; it < 1000; it++) {
        uint32_t cap = 1 + xs32() % 64;
        uint32_t ncol = 1 + xs32() % FACE_PERF_STATS_MAX_COL;
        uint32_t pushes = xs32() % (3 * cap + 1);
        uint32_t *buf = malloc((size_t)cap * ncol * sizeof(uint32_t));
        uint32_t *all = malloc(((size_t)pushes + 1) * ncol * sizeof(uint32_t));
        face_perf_ring_t r;
        int ok = face_perf_ring_init(&r, buf, cap, ncol) == 0;
        uint32_t base = 0;                               /* 最近一次 reset 后的首条 */
        for (uint32_t p = 0; ok && p < pushes; p++) {
            uint32_t *rec = all + (size_t)p * ncol;
            for (uint32_t c = 0; c < ncol; c++) rec[c] = xs32();
            face_perf_ring_push(&r, rec);
            if (xs32() % 97 == 0) {                      /* 偶尔中途 reset */
                face_perf_ring_reset(&r);
                base = p + 1;
            }
        }
        uint32_t live = pushes - base;                   /* 模型: reset 后的 push 数 */
        uint32_t want_len = live < cap ? live : cap;
        ok = ok && r.len == want_len && r.pushed == live;
        uint32_t out_cap = xs32() % (cap + 2);
        uint32_t *out = malloc((out_cap ? out_cap : 1) * sizeof(uint32_t));
        for (uint32_t c = 0; ok && c < ncol; c++) {
            uint32_t m = face_perf_ring_column(&r, c, out_cap ? out : NULL,
                                               out_cap);
            uint32_t k = want_len < out_cap ? want_len : out_cap;
            ok = m == k;
            for (uint32_t i = 0; ok && i < k; i++) {
                uint32_t src = base + live - k + i;      /* 最新 k 条, 从旧到新 */
                ok = out[i] == all[(size_t)src * ncol + c];
            }
        }
        if (!ok && bad++ < 3)
            printf("  ring case %d cap=%u ncol=%u pushes=%u len=%u\n", it, cap,
                   ncol, pushes, r.len);
        free(buf); free(all); free(out);
    }
    CHECK(bad == 0, "PBT ring 1000 vs naive model (%d bad)", bad);
}

int main(void)
{
    printf("test_face_perf_stats: examples\n");
    test_examples();
    printf("test_face_perf_stats: index rule (baseline n/2, (n*95)/100)\n");
    test_index_rule();
    printf("test_face_perf_stats: ring\n");
    test_ring_examples();
    printf("test_face_perf_stats: PBT (seed 0x2611150F)\n");
    pbt_summarize();
    pbt_ring();
    printf("test_face_perf_stats: %d/%d checks passed\n", s_checks - s_fail,
           s_checks);
    if (s_fail) {
        printf("test_face_perf_stats: FAIL\n");
        return 1;
    }
    printf("test_face_perf_stats: PASS\n");
    return 0;
}
