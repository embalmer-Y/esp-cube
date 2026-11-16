/*
 * test_face_perf_screen.c —— 表情圆网格采样宿主验收(任务 4.2)
 *
 * 编译运行(tools/host_tests.sh [18] 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
 *       -I firmware/main -o /tmp/test_face_perf_screen \
 *       firmware/main/test/test_face_perf_screen.c firmware/main/face_perf_screen.c \
 *   && /tmp/test_face_perf_screen
 *
 * 契约见 face_perf_screen.h。覆盖:
 * - 与 eaf_codec.py lint 同口径: 圆内点数 810, 首点 (182,21)、末点 (277,438)
 *   (python3 -c 'import eaf_codec as e; e.geom_sample_points(e.load_geom())'
 *   的输出); 非网格像素不影响结果
 * - 全背景 / 全非背景、行尾填充不被读取、小帧(帧外点按背景)、参数非法
 * - 随机对拍(固定种子 xorshift32, 1000 例): 随机宽高 1..520、随机行填充、
 *   随机背景值与噪声密度, 与二维下标参考实现逐例一致; 缓冲按恰好大小分配,
 *   越界由 ASan 捕获
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "face_perf_geom.h"
#include "face_perf_screen.h"

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

#define BG FACE_GEOM_BG_RGB565
#define LINT_PTS 810u                 /* eaf_codec.py geom_sample_points 点数 */

typedef struct {
    uint8_t *buf;
    uint32_t w, h, stride;
} frame_t;

static frame_t frame_new(uint32_t w, uint32_t h, uint32_t pad, uint16_t fill)
{
    frame_t f = { NULL, w, h, w * 2u + pad };
    f.buf = malloc((size_t)f.stride * h);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < f.stride / 2u; x++) {
            memcpy(f.buf + (size_t)y * f.stride + x * 2u, &fill, 2);
        }
    }
    return f;
}

static void px_set(frame_t *f, uint32_t x, uint32_t y, uint16_t v)
{
    memcpy(f->buf + (size_t)y * f->stride + x * 2u, &v, 2);
}

static uint32_t run(const frame_t *f, uint16_t bg, uint32_t *hit)
{
    return face_perf_screen_nonbg(f->buf, f->stride, f->w, f->h, bg, hit);
}

/* 参考实现: 显式二维下标, 网格坐标按 Python 镜像写法独立计算 */
static uint32_t ref(const frame_t *f, uint16_t bg, uint32_t *hit)
{
    const int cx = FACE_GEOM_CX, cy = FACE_GEOM_CY, r = FACE_GEOM_R;
    const int n = FACE_GEOM_GRID;
    uint32_t pts = 0;
    *hit = 0;
    for (int j = 0; j < n; j++) {
        int y = cy - r + ((2 * j + 1) * r) / n;
        for (int i = 0; i < n; i++) {
            int x = cx - r + ((2 * i + 1) * r) / n;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            pts++;
            if (x >= (int)f->w || y >= (int)f->h) continue;
            const uint16_t *row = (const uint16_t *)(f->buf + (size_t)y * f->stride);
            if (row[x] != bg) (*hit)++;
        }
    }
    return pts;
}

static void test_examples(void)
{
    uint32_t hit = 99;
    frame_t f = frame_new(FACE_GEOM_SCR_W, FACE_GEOM_SCR_H, 0, BG);
    CHECK(run(&f, BG, &hit) == LINT_PTS && hit == 0,
          "全背景: pts=%u hit=%u", run(&f, BG, &hit), hit);

    px_set(&f, 182, 21, 0xFFFF);              /* 首个网格点 */
    run(&f, BG, &hit);
    CHECK(hit == 1, "首点 (182,21) 非背景: hit=%u", hit);
    px_set(&f, 182, 21, BG);
    px_set(&f, 277, 438, 0x0001);             /* 末个网格点 */
    run(&f, BG, &hit);
    CHECK(hit == 1, "末点 (277,438) 非背景: hit=%u", hit);
    px_set(&f, 277, 438, BG);
    px_set(&f, 183, 21, 0xFFFF);              /* 相邻非网格像素 */
    px_set(&f, 230, 230, 0xFFFF);             /* 圆心不在网格上 */
    px_set(&f, 15, 15, 0xFFFF);               /* 圆外 */
    run(&f, BG, &hit);
    CHECK(hit == 0, "非网格/圆外像素不计: hit=%u", hit);
    CHECK(run(&f, BG, NULL) == LINT_PTS, "hit=NULL 仍返回点数");
    free(f.buf);

    f = frame_new(FACE_GEOM_SCR_W, FACE_GEOM_SCR_H, 0, 0x1234);
    CHECK(run(&f, BG, &hit) == LINT_PTS && hit == LINT_PTS,
          "全非背景: hit=%u", hit);
    free(f.buf);

    /* 行尾填充为非背景, 但不在 w 之内 → 不得读到 */
    f = frame_new(FACE_GEOM_SCR_W, FACE_GEOM_SCR_H, 64, BG);
    for (uint32_t y = 0; y < f.h; y++) {
        for (uint32_t x = f.w; x < f.stride / 2u; x++) px_set(&f, x, y, 0xFFFF);
    }
    CHECK(run(&f, BG, &hit) == LINT_PTS && hit == 0, "行填充: hit=%u", hit);
    free(f.buf);

    /* 小帧: 帧外点按背景, 点数仍为 810 */
    f = frame_new(200, 200, 0, 0x1234);
    uint32_t rh = 0;
    uint32_t pts = run(&f, BG, &hit);
    CHECK(pts == LINT_PTS && pts == ref(&f, BG, &rh) && hit == rh && hit > 0 &&
          hit < LINT_PTS, "小帧: pts=%u hit=%u ref=%u", pts, hit, rh);
    free(f.buf);

    /* 参数非法 */
    uint8_t one[4] = { 0 };
    hit = 7;
    CHECK(face_perf_screen_nonbg(NULL, 920, 460, 460, BG, &hit) == 0 && hit == 0,
          "buf NULL");
    CHECK(face_perf_screen_nonbg(one, 0, 0, 460, BG, &hit) == 0, "w=0");
    CHECK(face_perf_screen_nonbg(one, 920, 460, 0, BG, &hit) == 0, "h=0");
    CHECK(face_perf_screen_nonbg(one, 919, 460, 460, BG, &hit) == 0,
          "stride < w*2");
}

static void test_random(void)
{
    const int cases = 1000;
    int ok = 0;
    for (int k = 0; k < cases; k++) {
        const uint32_t w = 1u + xs32() % 520u;
        const uint32_t h = 1u + xs32() % 520u;
        const uint32_t pad = (xs32() % 4u) * 2u * (xs32() % 17u);
        const uint16_t bg = (k % 3 == 0) ? (uint16_t)BG : (uint16_t)xs32();
        frame_t f = frame_new(w, h, pad, bg);
        const uint32_t dens = xs32() % 101u;           /* 0..100% 噪声 */
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < f.stride / 2u; x++) {
                if (xs32() % 100u < dens) {
                    uint16_t v = (uint16_t)xs32();
                    px_set(&f, x, y, v);
                }
            }
        }
        uint32_t hit = 0, rh = 0;
        const uint32_t pts = run(&f, bg, &hit);
        const uint32_t rp = ref(&f, bg, &rh);
        if (pts == rp && hit == rh && pts == LINT_PTS && hit <= pts) {
            ok++;
        } else if (s_fail < 5) {
            printf("  counterexample: w=%u h=%u pad=%u bg=0x%04x dens=%u%% "
                   "pts=%u/%u hit=%u/%u\n", w, h, pad, bg, dens, pts, rp, hit, rh);
        }
        free(f.buf);
    }
    CHECK(ok == cases, "随机对拍 %d/%d 例一致", ok, cases);
    printf("test_face_perf_screen: random cross-check %d/%d\n", ok, cases);
}

int main(void)
{
    printf("test_face_perf_screen: examples\n");
    test_examples();
    printf("test_face_perf_screen: random (seed 0x2611150F)\n");
    test_random();
    printf("test_face_perf_screen: %d/%d checks passed\n",
           s_checks - s_fail, s_checks);
    if (s_fail) {
        printf("test_face_perf_screen: FAIL\n");
        return 1;
    }
    printf("test_face_perf_screen: PASS\n");
    return 0;
}
