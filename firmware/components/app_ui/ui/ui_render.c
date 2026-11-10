/*
 * ui_render.c —— 音频直线渲染: 频谱带 + AA 光栅器 + 预烘焙光盘 sprite
 *
 * 从 ui_pages.c 拆出(docs/06 §5); 性能决策见 docs/06 §8(30fps 红线):
 * 混合无除法(倒数表)/sprite 免 sqrtf/脏带清屏/频带边界预计算。
 */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cube_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ui_gfx_t ui_gfx;

/* ================= 预计算表 ================= */
/* na 倒数表(替代混合除法): nr = num * recip[na] >> 16 */
static uint16_t s_recip[256];

/* 预渲染光盘 sprite: 三个半径恒定(辉光 9/内芯 3.5/倒影 2.2), 一次性
 * 距离场 AA 后逐像素 blit —— 免去每帧 ~15 万次 sqrtf 与除法 */
enum { SPR_GLOW_BIG = 0, SPR_GLOW_SMALL, SPR_REFLECT, SPR_NUM };
#define SPR_MAX_HALF 11                     /* rad 9 → ri 11 */
typedef struct {
    int half;
    int stride;
    uint8_t cov[(SPR_MAX_HALF * 2 + 1) * (SPR_MAX_HALF * 2 + 1)];
} disc_sprite_t;
static disc_sprite_t s_spr[SPR_NUM];

void ui_render_tables_init(void)
{
    s_recip[0] = 0;
    for (int i = 1; i < 256; i++) {
        s_recip[i] = (uint16_t)((65536 + i / 2) / i);
    }
    static const float radii[SPR_NUM] = { 9.0f, 3.5f, 2.2f };
    for (int i = 0; i < SPR_NUM; i++) {
        float rad = radii[i];
        int ri = (int)rad + 2;
        s_spr[i].half = ri;
        s_spr[i].stride = ri * 2 + 1;
        for (int y = -ri; y <= ri; y++) {
            for (int x = -ri; x <= ri; x++) {
                float d = sqrtf((float)(x * x + y * y));
                float edge = rad - d;
                float cov = edge > 1.0f ? 1.0f : (edge < 0 ? 0.0f : edge);
                s_spr[i].cov[(y + ri) * s_spr[i].stride + (x + ri)] =
                    (uint8_t)(cov * 255.0f + 0.5f);
            }
        }
    }
    /* 频带边界表(对数分组, 消每帧 2×SPEC_BANDS 次 powf) */
    for (int b = 0; b < SPEC_BANDS; b++) {
        int lo = (int)(powf((float)b / SPEC_BANDS, 1.8f) * 100);
        int hi = (int)(powf((float)(b + 1) / SPEC_BANDS, 1.8f) * 100);
        if (hi <= lo) hi = lo + 1;
        ui_gfx.band_lo[b] = (int16_t)lo;
        ui_gfx.band_hi[b] = (int16_t)hi;
    }
}

/* ================= AA 光栅器 ================= */
static inline void line_px(int x, int y, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a)
{
    if (x < 0 || x >= LINE_W || y < 0 || y >= LINE_H || a == 0) return;
    uint32_t *px = &ui_gfx.buf[y * LINE_W + x];
    uint32_t d = *px;
    uint32_t da = d >> 24;
    uint32_t rem = 255 - a;
    uint32_t wa = (da * rem) >> 8;               /* ≈ da*rem/255 */
    uint32_t na = a + wa;                        /* over 合成 */
    if (na == 0) return;
    uint32_t inv = s_recip[na];
    uint32_t nr = ((r * a + (((d >> 16) & 0xFF) * wa)) * inv) >> 16;
    uint32_t ng = ((g * a + (((d >> 8) & 0xFF) * wa)) * inv) >> 16;
    uint32_t nb = ((b * a + ((d & 0xFF) * wa)) * inv) >> 16;
    *px = (na << 24) | (nr << 16) | (ng << 8) | nb;
}

/* 光盘 blit(距离场 AA 已预烘焙进 sprite) */
static void line_disc(int cx, int cy, int spr, uint8_t r, uint8_t g,
                      uint8_t b, uint8_t a)
{
    const disc_sprite_t *s = &s_spr[spr];
    int ri = s->half;
    int x0 = cx - ri, x1 = cx + ri, y0 = cy - ri, y1 = cy + ri;
    if (x1 < 0 || x0 >= LINE_W || y1 < 0 || y0 >= LINE_H) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *row = &s->cov[(y - (cy - ri)) * s->stride];
        for (int x = x0; x <= x1; x++) {
            uint8_t c = row[x - (cx - ri)];
            if (c) line_px(x, y, r, g, b, (uint8_t)((c * a) >> 8));
        }
    }
}

static void line_color(uint8_t *r, uint8_t *g, uint8_t *b, const ui_model_t *m)
{
    if (m->source == UI_SRC_BT)         { *r = 0x8F; *g = 0x7B; *b = 0xFF; }
    else if (m->source == UI_SRC_RADIO) { *r = 0x40; *g = 0xC8; *b = 0xF8; }
    else                                { *r = 0x46; *g = 0x54; *b = 0x6E; }
}

/* ================= 频谱带计算 ================= */
void ui_bands_update(bool playing)
{
    int got = playing ? ui_audio_take_pcm(ui_gfx.pcm, 256) : 0;
    if (got > 0) {
        static int16_t mono[256];
        int32_t acc = 0;
        for (int i = 0; i < 256; i++) {
            mono[i] = (int16_t)(((int32_t)ui_gfx.pcm[i * 2] +
                                 ui_gfx.pcm[i * 2 + 1]) >> 1);
            acc += mono[i] > 0 ? mono[i] : -mono[i];
        }
        float level = (float)acc / 256 / 5000;
        if (level > 1) level = 1;
        ui_gfx.energy = ui_gfx.energy * 0.6f + level * 0.4f;
        cube_fft_magnitude(mono, 256, ui_gfx.fft);
        float bass = 0;
        for (int k = 1; k <= 8; k++) bass += ui_gfx.fft[k];
        ui_gfx.bass = ui_gfx.bass * 0.7f + (bass / 8 / 900000.0f) * 0.3f;
        if (ui_gfx.bass > 1) ui_gfx.bass = 1;

        for (int b = 0; b < SPEC_BANDS; b++) {
            /* 对数频率分组(边界已预计算, 见 ui_render_tables_init) */
            int lo = ui_gfx.band_lo[b], hi = ui_gfx.band_hi[b];
            float mag = 0;
            for (int k = lo; k < hi && k < 128; k++) {
                if (ui_gfx.fft[k] > mag) mag = ui_gfx.fft[k];
            }
            /* log 域压缩 + 高带倾斜补偿(音乐能量天然集中低频,
             * 每倍频程 -6dB, 不补偿高频纹理不可见) */
            mag *= 1.0f + 2.2f * (float)b / SPEC_BANDS;
            /* mag 1e3→0.31 / 1e5→0.85 / 2.5e5→1.0 */
            float h = log10f(1.0f + mag * 0.01f) / 3.4f;
            if (h > 1) h = 1;
            ui_gfx.band[b] = h > ui_gfx.band[b] ? h : ui_gfx.band[b] * 0.80f;
        }
    } else {
        for (int b = 0; b < SPEC_BANDS; b++) ui_gfx.band[b] *= 0.86f;
        ui_gfx.energy *= 0.92f;
        ui_gfx.bass *= 0.92f;
    }
}

/* ================= 直线绘制(每帧) ================= */
void ui_line_draw(const ui_model_t *m)
{
    if (ui_gfx.buf == NULL || ui_gfx.canvas == NULL) return;  /* OOM 守卫 */

    bool idle = ui_pg.scene == SCENE_IDLE;

    /* 振幅: 频谱全开 / 待机呼吸(歌词场景不进本函数) */
    float amp = idle ? 3.0f + 1.2f * sinf(ui_pg.tick_cnt * 0.052f)
                     : 40.0f + 60.0f * ui_gfx.energy + 12.0f * ui_gfx.bass;
    if (amp > 100) amp = 100;
    float cy = LINE_CY;

    /* ---- 曲线逐 x 求值(band 高度 Catmull-Rom 插值, x 单调可按列解析) ---- */
    const float mgn = 22.0f;
    float step = (LINE_W - 2 * mgn) / (SPEC_BANDS - 1);
    for (int x = 0; x < LINE_W; x++) {
        float fx = (x - mgn) / step;
        if (fx < 0) fx = 0;
        if (fx > SPEC_BANDS - 1) fx = SPEC_BANDS - 1;
        int i = (int)fx;
        if (i >= SPEC_BANDS - 1) i = SPEC_BANDS - 2;
        float t = fx - i;
        int i0 = i > 0 ? i - 1 : 0;
        int i3 = i + 2 < SPEC_BANDS ? i + 2 : SPEC_BANDS - 1;
        float p0 = ui_gfx.band[i0], p1 = ui_gfx.band[i], p2 = ui_gfx.band[i + 1],
              p3 = ui_gfx.band[i3];
        /* Catmull-Rom 基函数 */
        float t2 = t * t, t3 = t2 * t;
        float h = (2 * t3 - 3 * t2 + 1) * p1 + (-2 * t3 + 3 * t2) * p2 +
                  (t3 - 2 * t2 + t) * 0.5f * (p2 - p0) +
                  (t3 - t2) * 0.5f * (p3 - p1);
        ui_gfx.curve[x] = cy - h * amp;
    }

    /* ---- 脏带清屏(曲线 y 范围 + 倒影带, 与上一帧取并集) ---- */
    int y0 = LINE_H, y1 = 0;
    for (int x = 0; x < LINE_W; x++) {
        int yy = (int)ui_gfx.curve[x];
        if (yy < y0) y0 = yy;
        if (yy > y1) y1 = yy;
        if (!idle) {                       /* 倒影带(cy 镜像 + 下移 14) */
            int ry = (int)(2 * cy - ui_gfx.curve[x] + 14);
            if (ry < y0) y0 = ry;
            if (ry > y1) y1 = ry;
        }
    }
    int cur_y0 = y0 - 13, cur_y1 = y1 + 13;  /* 辉光半径 + AA 余量 */
    if (cur_y0 < 0) cur_y0 = 0;
    if (cur_y1 >= LINE_H) cur_y1 = LINE_H - 1;
    y0 = cur_y0; y1 = cur_y1;
    if (ui_gfx.prev_y0 < y0) y0 = ui_gfx.prev_y0;    /* 并集: 上帧带可能更宽 */
    if (ui_gfx.prev_y1 > y1) y1 = ui_gfx.prev_y1;
    if (y1 >= y0) {
        memset(&ui_gfx.buf[(size_t)y0 * LINE_W], 0,
               (size_t)(y1 - y0 + 1) * LINE_W * sizeof(uint32_t));
    }
    ui_gfx.prev_y0 = cur_y0; ui_gfx.prev_y1 = cur_y1;

    uint8_t cr, cg, cb;
    line_color(&cr, &cg, &cb, m);
    float dim = idle ? 0.45f : 0.35f + 0.65f * ui_gfx.energy;

    /* ---- 辉光层(预烘焙 sprite, 无节点亮斑) ---- */
    for (int x = 0; x < LINE_W; x += 2) {
        line_disc(x, (int)ui_gfx.curve[x], SPR_GLOW_BIG, cr, cg, cb,
                  (uint8_t)(14 * dim));
        line_disc(x, (int)ui_gfx.curve[x], SPR_GLOW_SMALL, cr, cg, cb,
                  (uint8_t)(60 * dim));
    }
    /* ---- 芯线(逐列垂直 AA) ---- */
    int core = (int)(235 * (dim > 1.0f ? 1.0f : dim));
    for (int x = 0; x < LINE_W; x++) {
        float yf = ui_gfx.curve[x];
        int y0i = (int)yf;
        float sub = yf - y0i;
        line_px(x, y0i, cr, cg, cb, (uint8_t)core);
        line_px(x, y0i - 1, cr, cg, cb, (uint8_t)(core * sub));
        line_px(x, y0i + 1, cr, cg, cb, (uint8_t)(core * (1 - sub)));
    }
    /* ---- 倒影(下方镜像 35%, 弱化; 非待机) ---- */
    if (!idle) {
        int ra = (int)(26 * dim);
        for (int x = 0; x < LINE_W; x += 3) {
            line_disc(x, (int)(2 * cy - ui_gfx.curve[x] + 14), SPR_REFLECT,
                      cr, cg, cb, (uint8_t)ra);
        }
    }
    lv_obj_invalidate(ui_gfx.canvas);
}

/* ================= 画布构建 ================= */
void ui_render_create(lv_obj_t *scr)
{
    ui_gfx.buf = malloc(sizeof(uint32_t) * LINE_W * LINE_H);
    if (ui_gfx.buf != NULL) {
        ui_gfx.canvas = lv_canvas_create(scr);
        lv_canvas_set_buffer(ui_gfx.canvas, ui_gfx.buf, LINE_W, LINE_H,
                             LV_COLOR_FORMAT_ARGB8888);
        lv_obj_align(ui_gfx.canvas, LV_ALIGN_TOP_MID, 0, 160);
    }
}
