/*
 * test_ui_face_fmt.c —— 表情纯 C 部分宿主验收(任务 31.2; 合并 face_perf_screen 用例)
 *
 * 编译运行(tools/host_tests.sh [18] 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -fno-omit-frame-pointer -ffp-contract=off -Wall -Wextra -Wno-unused-parameter \
 *       -I firmware/components/app_ui/ui -o /tmp/test_ui_face_fmt \
 *       firmware/main/test/test_ui_face_fmt.c \
 *   && /tmp/test_ui_face_fmt
 *
 * 契约见 firmware/components/app_ui/ui/ui_face_fmt.h 与 ui_face_expect.h。覆盖:
 * - alpha0 千分比: RGB565A8 布局下全 0 / 全 0xFF / 已知计数 / 向下取整; 颜色平面的 0 不计;
 *   stride=0 按 w*2; 行尾填充不计; 拒绝: data 为空、w/h=0、stride/2<w、少 1 字节 → -1
 * - lit 采样集: 812 点, 首点 (179,7)、末点 (280,452), 与 binary64 参考(同 efc_build.py
 *   lit_sample_index)逐点、同序相等
 * - lit 亮度判定: ui_face_px_lit() 与 ui_face_rgb565_luma() ≥ 128 对全部 65536 个 RGB565 值一致;
 *   恰好 128.0 的值(0xB29C)判为 lit
 * - lit 计数(A8): alpha 0 的点不计(即使颜色为白); 键色 0xF81F 亮度 105 不计; 圆外与非格点不计
 * - lit 计数(屏幕帧缓冲): 背景色 0x0842 不计; 首末点; 行尾填充不读; 参数非法 → -1;
 *   PBT: 随机帧宽高 460..520、行填充、噪声密度, 与参考逐例一致(原 face_perf_screen 对拍)
 * - PBT(固定种子 xorshift32): 随机 alpha 密度、颜色(含阈值附近值)、stride 填充, 与
 *   binary64 参考采样实现逐例一致; 缓冲按恰好大小分配, 越界由 ASan 捕获
 * - mood 标签 → 片段名(B6)、内置判定、mood 词表、片段名词表、内置表项
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_face_fmt.h"

static int s_fail;
static int s_checks;

#define CHECK(cond, ...) do { \
    s_checks++; \
    if (!(cond)) { \
        printf("  FAIL %s:%d ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); s_fail++; \
    } \
} while (0)

static uint32_t s_rng = 0x2611143Fu;                   /* 固定种子 */
static uint32_t xs32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

/* RGB565A8 发布布局: RGB565(w*h*2) + alpha(w*h), 颜色字节填 color, alpha 填 a */
static uint8_t *make_a8(uint32_t w, uint32_t h, uint8_t color, uint8_t a,
                        size_t *size_out)
{
    size_t sz = (size_t)w * h * 3;
    uint8_t *p = malloc(sz);
    memset(p, color, (size_t)w * h * 2);
    memset(p + (size_t)w * h * 2, a, (size_t)w * h);
    *size_out = sz;
    return p;
}

/* alpha 参考实现: 逐像素按 (x,y) 定位, 独立于被测的指针步进写法 */
static int ref_permille(const uint8_t *d, size_t size, uint32_t w, uint32_t h,
                        uint32_t stride)
{
    if (d == NULL || w == 0 || h == 0) return -1;
    uint64_t st = stride ? stride : (uint64_t)w * 2;
    uint64_t ast = st / 2;
    if (ast < w) return -1;
    uint64_t need = st * h + ast * (h - 1) + w;      /* 最后一个 alpha 字节 + 1 */
    if (size < need) return -1;
    uint64_t z = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            z += d[st * h + ast * y + x] == 0;
        }
    }
    return (int)(z * 1000 / ((uint64_t)w * h));
}

/* ---- lit 参考实现(独立于被测helper): 与 tools/efc_build.py lit_sample_index() 同式 ----
 * centres = (np.arange(32) + 0.5) * 460 / 32; 圆心 230, 半径 230, 判据 dx²+dy² <= r². */
static int ref_samples(int xs[UI_FACE_LIT_POINTS], int ys[UI_FACE_LIT_POINTS])
{
    int n = 0;
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) {
            const double cy = ((double)i + 0.5) * 460.0 / 32.0;
            const double cx = ((double)j + 0.5) * 460.0 / 32.0;
            const double dx = cx - 230.0, dy = cy - 230.0;
            if (dx * dx + dy * dy <= 230.0 * 230.0) {
                if (n < (int)UI_FACE_LIT_POINTS) {
                    xs[n] = (int)cx;
                    ys[n] = (int)cy;
                }
                n++;
            }
        }
    }
    return n;
}

/* lit 参考计数: alpha 为 0 不计; 其余按 binary64 亮度 >= 128.0 计 lit */
static int ref_lit(const uint8_t *d, size_t size, uint32_t stride)
{
    if (stride == 0) stride = 920;
    const size_t astride = stride / 2;
    if (size < (size_t)stride * 460 + astride * 459 + 460) return -1;
    int xs[UI_FACE_LIT_POINTS], ys[UI_FACE_LIT_POINTS];
    const int n = ref_samples(xs, ys);
    int lit = 0;
    for (int k = 0; k < n; k++) {
        const size_t y = (size_t)ys[k], x = (size_t)xs[k];
        if (d[(size_t)stride * 460 + y * astride + x] == 0) continue;
        const uint8_t *p = d + y * stride + x * 2;
        const uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
        if (ui_face_rgb565_luma(c) >= 128.0) lit++;
    }
    return lit;
}

/* ================= alpha0 ================= */

static void test_alpha_examples(void)
{
    size_t sz;
    uint8_t *p;

    /* 460×460 全 0 alpha */
    p = make_a8(460, 460, 0x5A, 0x00, &sz);
    CHECK(ui_face_alpha0_permille(p, sz, 460, 460, 920) == 1000, "460 all-zero");
    CHECK(ui_face_alpha0_permille(p, sz, 460, 460, 0) == 1000, "stride=0 -> w*2");
    free(p);

    /* 全 0xFF alpha, 颜色平面全 0: 颜色字节不得计入 */
    p = make_a8(460, 460, 0x00, 0xFF, &sz);
    CHECK(ui_face_alpha0_permille(p, sz, 460, 460, 920) == 0, "opaque, black color");
    free(p);

    /* 已知计数: 4×2 中 3 个 0 → 375(alpha 平面起点 = w*h*2 = 16) */
    p = make_a8(4, 2, 0x00, 0xFF, &sz);
    p[16 + 0] = 0; p[16 + 5] = 0; p[16 + 7] = 0;
    CHECK(ui_face_alpha0_permille(p, sz, 4, 2, 8) == 375, "3/8");
    free(p);

    /* 向下取整: 3 像素中 1 个 0 → 333.3‰ → 333; 2 个 → 666 */
    p = make_a8(3, 1, 0x11, 0xFF, &sz);
    p[6] = 0;
    CHECK(ui_face_alpha0_permille(p, sz, 3, 1, 6) == 333, "1/3 floor");
    p[7] = 0;
    CHECK(ui_face_alpha0_permille(p, sz, 3, 1, 6) == 666, "2/3 floor");
    free(p);

    /* 行尾填充: stride = w*2+4 → alpha 行跨度 w+2, 填充字节为 0 也不计入 */
    {
        const uint32_t w = 5, h = 3, st = w * 2 + 4, ast = st / 2;
        size_t need = (size_t)st * h + (size_t)ast * (h - 1) + w;
        uint8_t *q = calloc(1, need);                /* 全 0(含填充) */
        for (uint32_t y = 0; y < h; y++) {
            memset(q + (size_t)st * h + (size_t)ast * y, 0xFF, w);
        }
        CHECK(ui_face_alpha0_permille(q, need, w, h, st) == 0, "padding ignored");
        q[(size_t)st * h + ast * 1 + 2] = 0;         /* 第 1 行一个 0 → 1/15 */
        CHECK(ui_face_alpha0_permille(q, need, w, h, st) == 66, "padded 1/15");
        free(q);
    }
}

static void test_alpha_reject(void)
{
    size_t sz;
    uint8_t *p = make_a8(8, 8, 0, 0, &sz);
    CHECK(ui_face_alpha0_permille(NULL, sz, 8, 8, 16) == -1, "NULL data");
    CHECK(ui_face_alpha0_permille(p, sz, 0, 8, 16) == -1, "w=0");
    CHECK(ui_face_alpha0_permille(p, sz, 8, 0, 16) == -1, "h=0");
    CHECK(ui_face_alpha0_permille(p, sz, 8, 8, 14) == -1, "stride/2 < w");
    CHECK(ui_face_alpha0_permille(p, sz - 1, 8, 8, 16) == -1, "short by 1");
    CHECK(ui_face_alpha0_permille(p, 0, 8, 8, 16) == -1, "size 0");
    CHECK(ui_face_alpha0_permille(p, sz, 8, 8, 16) == 1000, "exact size ok");
    free(p);
}

static void test_alpha_pbt(void)
{
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t w = 1 + xs32() % 64;
        uint32_t h = 1 + xs32() % 64;
        uint32_t pad = (xs32() % 4) * 2;             /* 偶数填充 0..6 */
        uint32_t st = (xs32() % 8 == 0) ? 0 : w * 2 + pad;
        if (xs32() % 16 == 0 && w > 1) st = w * 2 - 2;   /* 偶发非法 stride */
        uint64_t est = st ? st : (uint64_t)w * 2;
        uint64_t ast = est / 2;
        uint64_t need = est * h + (ast >= w ? ast * (h - 1) + w : 0);
        int slack = (int)(xs32() % 11) - 2;          /* -2..+8 */
        size_t size = (slack < 0 && need < (uint64_t)-slack)
                          ? 0 : (size_t)((int64_t)need + slack);
        uint8_t *d = malloc(size ? size : 1);
        uint32_t zp = xs32() % 101;                  /* 0 值概率 0..100% */
        for (size_t k = 0; k < size; k++) {
            d[k] = (xs32() % 100 < zp) ? 0 : (uint8_t)(1 + xs32() % 255);
        }
        int got = ui_face_alpha0_permille(d, size, w, h, st);
        int exp = ref_permille(d, size, w, h, st);
        if (got != exp) {
            if (bad < 5) {
                printf("  PBT mismatch #%d: w=%u h=%u st=%u size=%zu got=%d exp=%d\n",
                       i, w, h, st, size, got, exp);
            }
            bad++;
        }
        free(d);
    }
    CHECK(bad == 0, "PBT alpha0 vs reference: %d/2000 mismatches", bad);
}

/* ================= lit ================= */

/* 采样集: 数量 812, 首末点, 与 binary64 参考逐点同序相等 */
static void test_lit_geometry(void)
{
    int rx[UI_FACE_LIT_POINTS], ry[UI_FACE_LIT_POINTS];
    const int rn = ref_samples(rx, ry);
    CHECK(rn == (int)UI_FACE_LIT_POINTS, "reference sample count %d", rn);
    int n = 0, first_x = -1, first_y = -1, last_x = -1, last_y = -1, same = 1;
    for (uint32_t i = 0; i < UI_FACE_LIT_GRID; i++) {
        for (uint32_t j = 0; j < UI_FACE_LIT_GRID; j++) {
            if (!ui_face_lit_cell_inside(i, j)) continue;
            const int x = (int)ui_face_lit_cell_px(j);
            const int y = (int)ui_face_lit_cell_px(i);
            if (n == 0) { first_x = x; first_y = y; }
            last_x = x; last_y = y;
            if (n < rn && (x != rx[n] || y != ry[n])) same = 0;
            n++;
        }
    }
    CHECK(n == (int)UI_FACE_LIT_POINTS, "device sample count %d", n);
    CHECK(first_x == 179 && first_y == 7, "first (%d,%d)", first_x, first_y);
    CHECK(last_x == 280 && last_y == 452, "last (%d,%d)", last_x, last_y);
    CHECK(same, "device sample sequence differs from binary64 reference");
}

/* 整数判据与 binary64 参考对全部 65536 个 RGB565 值一致 */
static void test_lit_luma_exhaustive(void)
{
    int bad = 0, lit_total = 0;
    for (uint32_t v = 0; v < 65536u; v++) {
        const int a = ui_face_px_lit((uint16_t)v);
        const int b = ui_face_rgb565_luma((uint16_t)v) >= UI_FACE_LIT_LUMA_MIN;
        lit_total += a;
        if (a != b) {
            if (bad < 5) printf("  luma mismatch px=0x%04X int=%d f64=%d\n", v, a, b);
            bad++;
        }
    }
    CHECK(bad == 0, "integer vs binary64 lit decision: %d/65536 mismatches", bad);
    CHECK(lit_total == 32541, "lit pixels of 65536 = %d (PC: 32541)", lit_total);
}

static void test_lit_luma_examples(void)
{
    CHECK(ui_face_px_lit(0xB29C) == true, "0xB29C luma == 128.0 is lit");
    CHECK(ui_face_px_lit(0x746B) == false, "0x746B luma 127.999 not lit");
    CHECK(ui_face_px_lit(0x9B55) == true, "0x9B55 luma 128.001 lit");
    CHECK(ui_face_px_lit(0xFFFF) == true && ui_face_px_lit(0x0000) == false, "white/black");
    CHECK(ui_face_px_lit(0xF81F) == false, "colour key 0xF81F (luma 105) not lit");
    CHECK(ui_face_px_lit(0x0842) == false, "C_BG 0x0842 not lit");
}

static void test_lit_count_examples(void)
{
    size_t sz;
    /* alpha 全 0: 颜色为白也不计 lit */
    uint8_t *p = make_a8(460, 460, 0xFF, 0x00, &sz);
    uint32_t lit = 99;
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812 && lit == 0,
          "all keyed: lit=%u", lit);
    free(p);

    /* alpha 全 0xFF, 颜色全白: 812 点全 lit */
    p = make_a8(460, 460, 0xFF, 0xFF, &sz);
    CHECK(ui_face_lit_count(p, sz, 460, 460, 0, &lit) == 812 && lit == 812,
          "all white opaque: lit=%u", lit);
    free(p);

    /* alpha 全 0xFF, 颜色全黑: 0 */
    p = make_a8(460, 460, 0x00, 0xFF, &sz);
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812 && lit == 0,
          "all black opaque: lit=%u", lit);
    free(p);

    /* 键色 0xF81F 且 alpha 255: 亮度 105, 不计 lit */
    p = make_a8(460, 460, 0x00, 0xFF, &sz);
    for (size_t k = 0; k < (size_t)460 * 460; k++) { p[2 * k] = 0x1F; p[2 * k + 1] = 0xF8; }
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812 && lit == 0,
          "colour key opaque: lit=%u", lit);
    free(p);

    /* 只有首末采样点为白且不透明 */
    p = make_a8(460, 460, 0x00, 0x00, &sz);
    {
        const size_t off = (size_t)460 * 460 * 2;
        p[off + 7 * 460 + 179] = 0xFF;                 /* alpha 在 (179,7) */
        p[(size_t)7 * 920 + 179 * 2] = 0xFF;           /* 颜色 0xFFFF */
        p[(size_t)7 * 920 + 179 * 2 + 1] = 0xFF;
        p[off + 452 * 460 + 280] = 0xFF;
        p[(size_t)452 * 920 + 280 * 2] = 0xFF;
        p[(size_t)452 * 920 + 280 * 2 + 1] = 0xFF;
    }
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812 && lit == 2,
          "first and last points: lit=%u", lit);
    /* 白色但落在格点之外(183,7)与圆外(15,15): 不计 */
    memset(p, 0, sz);
    for (size_t k = 0; k < (size_t)460 * 460; k++) p[(size_t)460 * 460 * 2 + k] = 0xFF;
    p[(size_t)7 * 920 + 183 * 2] = 0xFF;
    p[(size_t)7 * 920 + 183 * 2 + 1] = 0xFF;
    p[(size_t)15 * 920 + 15 * 2] = 0xFF;
    p[(size_t)15 * 920 + 15 * 2 + 1] = 0xFF;
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812 && lit == 0,
          "non-grid and outside-circle pixels: lit=%u", lit);
    free(p);
}

static void test_lit_count_reject(void)
{
    size_t sz;
    uint8_t *p = make_a8(460, 460, 0xFF, 0xFF, &sz);
    uint32_t lit = 7;
    CHECK(ui_face_lit_count(NULL, sz, 460, 460, 920, &lit) == -1, "NULL data");
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, NULL) == -1, "lit NULL");
    CHECK(ui_face_lit_count(p, sz, 459, 460, 918, &lit) == -1 && lit == 7, "w=459");
    CHECK(ui_face_lit_count(p, sz, 460, 461, 920, &lit) == -1 && lit == 7, "h=461");
    CHECK(ui_face_lit_count(p, sz, 460, 460, 918, &lit) == -1, "stride/2 < w");
    CHECK(ui_face_lit_count(p, sz - 1, 460, 460, 920, &lit) == -1, "short by 1");
    CHECK(ui_face_lit_count(p, sz, 460, 460, 920, &lit) == 812, "exact size ok");
    free(p);
}

/* PBT: 随机 alpha 密度、颜色(含阈值附近值)、行尾填充, 与 binary64 参考逐例一致 */
static void test_lit_pbt(void)
{
    static const uint16_t near[] = { 0xB29C, 0x746B, 0x9B55, 0x6470, 0x8448, 0x6D01, 0xF81F, 0x0842 };
    const int cases = 200;
    int bad = 0;
    for (int k = 0; k < cases; k++) {
        const uint32_t pad = (xs32() % 4u) * 2u * (xs32() % 9u);
        const uint32_t stride = 920u + pad;
        const size_t astride = stride / 2u;
        const size_t size = (size_t)stride * 460 + astride * 459 + 460;
        uint8_t *d = malloc(size);
        const uint32_t zp = xs32() % 101u;           /* alpha 0 概率 0..100% */
        for (size_t t = 0; t < size; t++) d[t] = 0xA5;   /* 填充与未用字节 */
        for (uint32_t y = 0; y < 460; y++) {
            for (uint32_t x = 0; x < 460; x++) {
                uint16_t c = (xs32() % 4u == 0) ? near[xs32() % 8u] : (uint16_t)xs32();
                d[(size_t)y * stride + (size_t)x * 2u] = (uint8_t)c;
                d[(size_t)y * stride + (size_t)x * 2u + 1] = (uint8_t)(c >> 8);
                d[(size_t)stride * 460 + (size_t)y * astride + x] =
                    (xs32() % 100u < zp) ? 0u : 0xFFu;
            }
        }
        uint32_t lit = 0;
        const int pts = ui_face_lit_count(d, size, 460, 460, stride, &lit);
        const int ref = ref_lit(d, size, stride);
        if (pts != (int)UI_FACE_LIT_POINTS || (int)lit != ref) {
            if (bad < 5) {
                printf("  lit PBT mismatch #%d: stride=%u zp=%u pts=%d lit=%u ref=%d\n",
                       k, stride, zp, pts, lit, ref);
            }
            bad++;
        }
        free(d);
    }
    CHECK(bad == 0, "PBT lit vs binary64 reference: %d/%d mismatches", bad, cases);
}

static void test_lit_screen(void)
{
    /* 背景色 0x0842 全屏: 0 */
    const uint32_t w = 460, h = 460;
    uint8_t *f = malloc((size_t)w * h * 2);
    for (size_t k = 0; k < (size_t)w * h; k++) { f[2 * k] = 0x42; f[2 * k + 1] = 0x08; }
    uint32_t lit = 9;
    CHECK(ui_face_lit_screen(f, 0, w, h, &lit) == 812 && lit == 0, "bg frame: lit=%u", lit);
    /* 首点 (179,7) 置白 */
    f[(7 * w + 179) * 2] = 0xFF; f[(7 * w + 179) * 2 + 1] = 0xFF;
    CHECK(ui_face_lit_screen(f, 0, w, h, &lit) == 812 && lit == 1, "first point: lit=%u", lit);
    free(f);

    /* 大帧 + 行尾填充: 填充不读; 全白采样区 812 */
    const uint32_t bw = 480, bh = 480, st = bw * 2 + 6;
    uint8_t *g = malloc((size_t)st * bh);
    memset(g, 0xFF, (size_t)st * bh);
    CHECK(ui_face_lit_screen(g, st, bw, bh, &lit) == 812 && lit == 812,
          "padded white frame: lit=%u", lit);
    free(g);

    CHECK(ui_face_lit_screen(NULL, 0, w, h, &lit) == -1, "NULL buf");
    uint8_t one[4] = { 0 };
    CHECK(ui_face_lit_screen(one, 0, 459, 460, &lit) == -1, "w=459");
    CHECK(ui_face_lit_screen(one, 918, 460, 460, &lit) == -1, "stride < w*2");
    CHECK(ui_face_lit_screen(one, 920, 460, 460, NULL) == -1, "lit NULL");
    lit = 77;
    CHECK(ui_face_lit_screen(one, 800, 400, 400, &lit) == -1 && lit == 77,
          "frame smaller than 460 rejected, lit untouched");
}

/* 屏幕帧缓冲 PBT(原 face_perf_screen 随机对拍的口径): 随机帧宽高 460..520、随机行填充、
 * 随机噪声密度, 与 binary64 参考逐例一致。 */
static void test_lit_screen_pbt(void)
{
    const int cases = 200;
    int bad = 0;
    for (int k = 0; k < cases; k++) {
        const uint32_t w = 460u + xs32() % 61u;
        const uint32_t h = 460u + xs32() % 61u;
        const uint32_t stride = w * 2u + (xs32() % 4u) * 2u * (xs32() % 9u);
        uint8_t *f = malloc((size_t)stride * h);
        const uint32_t dens = xs32() % 101u;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < stride / 2u; x++) {
                const uint16_t v = (xs32() % 100u < dens) ? (uint16_t)xs32() : 0x0842u;
                f[(size_t)y * stride + (size_t)x * 2u] = (uint8_t)v;
                f[(size_t)y * stride + (size_t)x * 2u + 1] = (uint8_t)(v >> 8);
            }
        }
        uint32_t lit = 0;
        const int pts = ui_face_lit_screen(f, stride, w, h, &lit);
        int xs[UI_FACE_LIT_POINTS], ys[UI_FACE_LIT_POINTS];
        const int rn = ref_samples(xs, ys);
        uint32_t rlit = 0;
        for (int q = 0; q < rn; q++) {
            const uint8_t *p = f + (size_t)ys[q] * stride + (size_t)xs[q] * 2u;
            if (ui_face_rgb565_luma((uint16_t)(p[0] | (p[1] << 8))) >= 128.0) rlit++;
        }
        if (pts != (int)UI_FACE_LIT_POINTS || lit != rlit) {
            if (bad < 5) {
                printf("  screen PBT mismatch #%d: w=%u h=%u stride=%u lit=%u ref=%u\n",
                       k, w, h, stride, lit, rlit);
            }
            bad++;
        }
        free(f);
    }
    CHECK(bad == 0, "PBT screen lit vs reference: %d/%d mismatches", bad, cases);
}

/* ================= mood / 片段 / 内置表 ================= */

static void test_mood_clip(void)
{
    CHECK(strcmp(ui_face_mood_clip("neutral"), "smile_static") == 0, "neutral");
    CHECK(strcmp(ui_face_mood_clip("happy"), "laugh") == 0, "happy");
    CHECK(strcmp(ui_face_mood_clip("relaxed"), "leisure") == 0, "relaxed");
    CHECK(strcmp(ui_face_mood_clip("sad"), "sad") == 0, "sad");
    CHECK(strcmp(ui_face_mood_clip("angry"), "angry") == 0, "angry");
    CHECK(strcmp(ui_face_mood_clip("surprised"), "shocked") == 0, "surprised");
    CHECK(strcmp(ui_face_mood_clip("confused"), "question") == 0, "confused");
    CHECK(strcmp(ui_face_mood_clip("sleepy"), "yawn") == 0, "sleepy");
    CHECK(strcmp(ui_face_mood_clip(""), "smile_static") == 0, "empty");
    CHECK(strcmp(ui_face_mood_clip(NULL), "smile_static") == 0, "NULL");
    CHECK(strcmp(ui_face_mood_clip("Happy"), "smile_static") == 0, "case");
    CHECK(strcmp(ui_face_mood_clip("speak"), "smile_static") == 0, "speak has no clip row");
}

static void test_builtin(void)
{
    CHECK(ui_face_clip_builtin("smile_static"), "smile_static built in");
    CHECK(ui_face_clip_builtin("laugh"), "laugh built in");
    CHECK(ui_face_clip_builtin("investigate"), "investigate built in");
    CHECK(ui_face_clip_builtin("shocked"), "shocked built in");
    CHECK(!ui_face_clip_builtin("leisure") && !ui_face_clip_builtin("sad") &&
          !ui_face_clip_builtin("angry") && !ui_face_clip_builtin("question") &&
          !ui_face_clip_builtin("yawn") && !ui_face_clip_builtin("ponder"),
          "pool clips not built in");
    CHECK(!ui_face_clip_builtin("") && !ui_face_clip_builtin(NULL), "empty/NULL");
    /* 设备的实际选择 = 映射结果若内置则用之, 否则回退 smile_static; 任何 mood 都必须落在内置集合内 */
    static const char *const moods[] = { "neutral", "happy", "relaxed", "sad", "angry",
                                         "surprised", "confused", "sleepy" };
    int fallbacks = 0;
    for (size_t i = 0; i < sizeof(moods) / sizeof(moods[0]); i++) {
        const char *c = ui_face_mood_clip(moods[i]);
        const char *eff = ui_face_clip_builtin(c) ? c : "smile_static";
        if (eff != c) fallbacks++;
        CHECK(ui_face_clip_builtin(eff), "mood %s -> %s -> %s", moods[i], c, eff);
    }
    CHECK(fallbacks == 5, "moods that fall back to smile_static: %d (expect 5)", fallbacks);
}

static void test_expect_table(void)
{
    const ui_face_expect_t *e = ui_face_expect_find("laugh");
    CHECK(e != NULL && e->key_permille_x100 == 83413u && e->lit_pct_x100 == 2094u,
          "laugh row");
    e = ui_face_expect_find("smile_static");
    CHECK(e != NULL && e->key_permille_x100 == 83408u && e->lit_pct_x100 == 2069u,
          "smile_static row");
    CHECK(ui_face_expect_find("ponder") == NULL && ui_face_expect_find(NULL) == NULL,
          "non-builtin and NULL -> NULL");
    CHECK(UI_FACE_ALPHA0_TOL_X100 == 500u && UI_FACE_LIT_TOL_X100 == 500u,
          "tolerances 0.5 and 5 points");
}

static void test_mood_vocab(void)
{
    static const char *known[] = {
        "happy", "relaxed", "angry", "sad", "surprised", "confused",
        "sleepy", "neutral",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%s", known[i]);  /* 非字面量指针 */
        const char *v = ui_face_mood_vocab(buf);
        CHECK(strcmp(v, known[i]) == 0 && v != buf, "vocab %s", known[i]);
    }
    CHECK(strcmp(ui_face_mood_vocab(""), "none") == 0, "empty");
    CHECK(strcmp(ui_face_mood_vocab(NULL), "none") == 0, "NULL");
    CHECK(strcmp(ui_face_mood_vocab("Happy"), "other") == 0, "case");
    CHECK(strcmp(ui_face_mood_vocab("happy "), "other") == 0, "trailing space");
    CHECK(strcmp(ui_face_mood_vocab("\xe4\xbd\xa0\xe5\xa5\xbd"), "other") == 0, "CJK");
    CHECK(strcmp(ui_face_mood_vocab("shocked"), "other") == 0, "unknown tag");
}

static void test_name_ok(void)
{
    CHECK(ui_face_name_ok("neutral"), "neutral");
    CHECK(ui_face_name_ok("idle"), "idle");
    CHECK(ui_face_name_ok("a"), "1 char");
    CHECK(ui_face_name_ok("face_2-b"), "_ and -");
    CHECK(ui_face_name_ok("abcdefghijklmno"), "15 chars");
    CHECK(!ui_face_name_ok("abcdefghijklmnop"), "16 chars");
    CHECK(!ui_face_name_ok(""), "empty");
    CHECK(!ui_face_name_ok(NULL), "NULL");
    CHECK(!ui_face_name_ok("Idle"), "upper");
    CHECK(!ui_face_name_ok("a b"), "space");
    CHECK(!ui_face_name_ok("../x"), "path");
    CHECK(!ui_face_name_ok("a.b"), "dot");
    CHECK(!ui_face_name_ok("a%sb"), "format char");
}

int main(void)
{
    test_alpha_examples();
    test_alpha_reject();
    test_alpha_pbt();
    test_lit_geometry();
    test_lit_luma_exhaustive();
    test_lit_luma_examples();
    test_lit_count_examples();
    test_lit_count_reject();
    test_lit_pbt();
    test_lit_screen();
    test_lit_screen_pbt();
    test_mood_clip();
    test_builtin();
    test_expect_table();
    test_mood_vocab();
    test_name_ok();
    if (s_fail) {
        printf("test_ui_face_fmt: %d/%d checks FAILED\n", s_fail, s_checks);
        return 1;
    }
    printf("test_ui_face_fmt: PASS (%d checks)\n", s_checks);
    return 0;
}
