/*
 * ui_face_fmt.h —— 表情素材的纯 C 部分(header-only, 零 LVGL/IDF 依赖)
 *
 * freeze-and-face-blackscreen-fix 任务 31.2 / 31.3(B2、B3、B6):
 *   - alpha0: RGB565A8 第 0 帧 alpha 平面的 0 值千分比, 与 MANIFEST frame0_key_permille 对照;
 *   - lit: 表情圆内 812 个采样点中亮度 ≥128 的点数, 与 MANIFEST frame0_lit_share_pct 对照。
 *     采样点与口径同 tools/efc_build.py 的 lit_sample_index() 与 rgb565_luma()(D-FACE-SCREEN);
 *   - mood 标签 → 片段名(B6 提议表); 片段是否内置由 ui_face_expect.h 的表决定;
 *   - mood 标签与片段名的固定词表(日志只输出词表与数值, NFR-03、3.15)。
 * 宿主测试: firmware/main/test/test_ui_face_fmt.c(host_tests.sh [18])。
 * header-only: 设备(app_ui、main)与宿主测试同源包含。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_face_expect.h"

/* 表情圆边长与屏幕自检采样口径(D-FACE-SCREEN, 2026-11-17)。
 * 与 tools/efc_build.py 的 SIZE、LIT_GRID、LIT_LUMA_MIN 同口径。 */
#define UI_FACE_SIZE          460u
#define UI_FACE_LIT_GRID      32u
#define UI_FACE_LIT_POINTS    812u
#define UI_FACE_LIT_LUMA_MIN  128.0      /* 参考口径(binary64), 见 ui_face_rgb565_luma() */
#define UI_FACE_LIT_SUM_MIN   128000u    /* 整数判据: 299R+587G+114B >= 128000 */

/* RGB565A8 alpha 平面 0 值占比, 千分比 0..1000(向下取整)。
 * 布局(与 esp_efc_codec 的 lv_efc 发布的 imgdsc 一致): stride×h 字节 RGB565
 * 之后紧跟 alpha 平面, 行跨度 stride/2; stride=0 按 w*2。
 * 返回 -1: data 为空、尺寸为 0、stride/2 < w, 或 data_size 不足以容纳
 * alpha 平面(不越界读)。 */
static inline int ui_face_alpha0_permille(const uint8_t *data,
                                          size_t data_size, uint32_t w,
                                          uint32_t h, uint32_t stride)
{
    if (data == NULL || w == 0 || h == 0) return -1;
    if (stride == 0) stride = w * 2;
    const size_t astride = stride / 2;
    const size_t off = (size_t)stride * h;
    if (astride < w) return -1;
    if (data_size < off || data_size - off < astride * (h - 1) + w) {
        return -1;
    }
    const uint8_t *a = data + off;
    uint64_t zeros = 0;
    for (uint32_t y = 0; y < h; y++, a += astride) {
        for (uint32_t x = 0; x < w; x++) {
            zeros += (a[x] == 0);
        }
    }
    return (int)(zeros * 1000u / ((uint64_t)w * h));
}

/* 采样格 i(0..31, 行或列)的像素坐标 floor((2i+1)·460/64)。与 PC 端 astype(intp)
 * 截断一致; 格中心 (i+0.5)·460/32 是二进制小数, 整数除法与浮点结果相同。 */
static inline uint32_t ui_face_lit_cell_px(uint32_t i)
{
    return ((2u * i + 1u) * UI_FACE_SIZE) / (2u * UI_FACE_LIT_GRID);
}

/* 格 (i, j) 是否在圆内(圆心 (230,230), 半径 230, 判据 dx²+dy² <= 230²)。
 * 中心偏移为 115·p/16 与 115·q/16(p = 2i-31, q = 2j-31), 判据化为 p²+q² <= 1024。
 * 等号不可达(两个奇数的平方和模 4 为 2), 故 <= 与 < 结果相同。 */
static inline bool ui_face_lit_cell_inside(uint32_t i, uint32_t j)
{
    const int32_t p = 2 * (int32_t)i - (int32_t)(UI_FACE_LIT_GRID - 1u);
    const int32_t q = 2 * (int32_t)j - (int32_t)(UI_FACE_LIT_GRID - 1u);
    return p * p + q * q <= 1024;
}

/* 单点亮度判定(设备上使用的整数形式)。R/G/B 为 RGB565 位复制展开的 8 位值。
 * 0.299R+0.587G+0.114B 乘以 1000 后为整数 299R+587G+114B, 判据 ≥ 128000 与
 * luma ≥ 128 在实数上完全等价, 不经过浮点。宿主测试对 65536 个 RGB565 值逐一
 * 与 ui_face_rgb565_luma() 的判定比对。 */
static inline bool ui_face_px_lit(uint16_t px)
{
    const uint32_t r5 = (px >> 11) & 31u;
    const uint32_t g6 = (px >> 5) & 63u;
    const uint32_t b5 = px & 31u;
    const uint32_t r = (r5 << 3) | (r5 >> 2);
    const uint32_t g = (g6 << 2) | (g6 >> 4);
    const uint32_t b = (b5 << 3) | (b5 >> 2);
    return 299u * r + 587u * g + 114u * b >= UI_FACE_LIT_SUM_MIN;
}

/* 参考实现(仅宿主测试使用, 设备上不调用): 亮度 0.299R + 0.587G + 0.114B 的 binary64
 * 计算, 运算顺序与 tools/efc_build.py rgb565_luma() 相同(自左至右相加)。
 * 宿主测试以它核对 ui_face_px_lit() 的判定。 */
static inline double ui_face_rgb565_luma(uint16_t px)
{
    const uint32_t r5 = (px >> 11) & 31u;
    const uint32_t g6 = (px >> 5) & 63u;
    const uint32_t b5 = px & 31u;
    const double r = (double)((r5 << 3) | (r5 >> 2));
    const double g = (double)((g6 << 2) | (g6 >> 4));
    const double b = (double)((b5 << 3) | (b5 >> 2));
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

/* 屏幕自检(RGB565A8 第 0 帧, D-READPOINT): 812 个采样点中 lit 的点数。
 * 布局与 ui_face_alpha0_permille 相同; w、h 必须为 UI_FACE_SIZE。
 * alpha 为 0 的像素计为 C_BG, 不计为 lit。成功返回采样点数 812 并写 *lit;
 * 参数非法返回 -1, 不写 *lit。 */
static inline int ui_face_lit_count(const uint8_t *data, size_t data_size,
                                    uint32_t w, uint32_t h, uint32_t stride,
                                    uint32_t *lit)
{
    if (data == NULL || lit == NULL || w != UI_FACE_SIZE || h != UI_FACE_SIZE) {
        return -1;
    }
    if (stride == 0) stride = w * 2u;
    const size_t astride = stride / 2u;
    if (astride < w) return -1;
    const size_t aoff = (size_t)stride * h;
    if (data_size < aoff || data_size - aoff < astride * (h - 1u) + w) {
        return -1;
    }
    const uint8_t *alpha = data + aoff;
    uint32_t n = 0;
    int pts = 0;
    for (uint32_t i = 0; i < UI_FACE_LIT_GRID; i++) {
        for (uint32_t j = 0; j < UI_FACE_LIT_GRID; j++) {
            if (!ui_face_lit_cell_inside(i, j)) continue;
            pts++;
            const uint32_t x = ui_face_lit_cell_px(j);
            const uint32_t y = ui_face_lit_cell_px(i);
            if (alpha[(size_t)y * astride + x] == 0) continue;      /* C_BG, 不计 lit */
            const uint8_t *p = data + (size_t)y * stride + (size_t)x * 2u;
            if (ui_face_px_lit((uint16_t)(p[0] | (p[1] << 8)))) n++; /* RGB565 小端 */
        }
    }
    *lit = n;
    return pts;
}

/* 屏幕自检(显示帧缓冲 RGB565, 无 alpha 平面): 812 个采样点中 lit 的点数。
 * buf 为帧首, stride 每行字节数(0 按 w*2); w、h 为帧宽高, 均不小于 UI_FACE_SIZE。
 * 成功返回 812 并写 *lit; 参数非法返回 -1, 不写 *lit。 */
static inline int ui_face_lit_screen(const uint8_t *buf, uint32_t stride,
                                     uint32_t w, uint32_t h, uint32_t *lit)
{
    if (buf == NULL || lit == NULL || w < UI_FACE_SIZE || h < UI_FACE_SIZE) {
        return -1;
    }
    if (stride == 0) stride = w * 2u;
    if (stride / 2u < w) return -1;
    uint32_t n = 0;
    int pts = 0;
    for (uint32_t i = 0; i < UI_FACE_LIT_GRID; i++) {
        for (uint32_t j = 0; j < UI_FACE_LIT_GRID; j++) {
            if (!ui_face_lit_cell_inside(i, j)) continue;
            pts++;
            const uint32_t x = ui_face_lit_cell_px(j);
            const uint32_t y = ui_face_lit_cell_px(i);
            const uint8_t *p = buf + (size_t)y * stride + (size_t)x * 2u;
            if (ui_face_px_lit((uint16_t)(p[0] | (p[1] << 8)))) n++;
        }
    }
    *lit = n;
    return pts;
}

/* mood 标签 → 片段名(design B6 提议表, 2026-11-15 采纳)。空、NULL 与词表外标签 → smile_static。
 * speak 不在表中: 说话沿用当前片段, 只改帧间隔(B6)。
 * 返回值不保证已内置: 调用方用 ui_face_clip_builtin() 判断, 未内置则回退 smile_static 并告警。 */
static inline const char *ui_face_mood_clip(const char *tag)
{
    static const struct { const char *tag; const char *clip; } map[] = {
        { "neutral",   "smile_static" },
        { "happy",     "laugh" },
        { "relaxed",   "leisure" },
        { "sad",       "sad" },
        { "angry",     "angry" },
        { "surprised", "shocked" },
        { "confused",  "question" },
        { "sleepy",    "yawn" },
    };
    if (tag != NULL) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcmp(tag, map[i].tag) == 0) return map[i].clip;
        }
    }
    return "smile_static";
}

/* 片段是否内置(即 ui_face_expect.h 的表中有该名称)。 */
static inline bool ui_face_clip_builtin(const char *name)
{
    return ui_face_expect_find(name) != NULL;
}

/* mood 标签 → 固定词表(soul anim_tag 词表 + neutral); 空 → none, 词表外 → other */
static inline const char *ui_face_mood_vocab(const char *tag)
{
    static const char *const known[] = {
        "happy", "relaxed", "angry", "sad", "surprised", "confused",
        "sleepy", "neutral",
    };
    if (tag == NULL || tag[0] == '\0') return "none";
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(tag, known[i]) == 0) return known[i];
    }
    return "other";
}

/* 片段名可入日志: [a-z0-9_-]{1,15}; 否则调用方记 invalid */
static inline bool ui_face_name_ok(const char *s)
{
    if (s == NULL) return false;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (n >= 15) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return n > 0;
}
