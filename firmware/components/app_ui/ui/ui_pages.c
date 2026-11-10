/*
 * ui_pages.c —— 极简氛围 UI(平台无关: 固件/模拟器同源编译)
 *
 * 设计原则(2026-11-11 定稿): 屏幕小(2寸 460×460), 不做传统控件界面 ——
 *   整屏就是一个"正在播放"的动画舞台:
 *   · 电台/纯音乐: 一条随音频跳动的发光直线(频谱动画, 全屏主角)
 *   · BT 音乐有歌词: 歌词动画(当前行渐入/下一行预示), 直线弱化为底部衬托
 *   · 待机: 直线呼吸 + 微弱品牌标
 *   · 上下滑动 = 调音量(实时 HUD), 左右滑动 = 切曲目/电台(带 toast)
 *
 * 流畅性技术(调研结论):
 *   · UI 状态过渡全部 lv_anim + 缓动(ease_out/ease_in), 不手工插值;
 *   · 音频直线 = 自绘 AA 光栅器(距离场覆盖率)+ 双层辉光(bloom),
 *     ARGB 画布由 LVGL 合成 —— 单路径场景比启用 ThorVG 矢量管线更省;
 *   · 30fps 主定时器 + 变更检测(label 不每帧重设)。
 */
#include "ui_pages.h"
#include "ui_model.h"
#include "ui_font_cjk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "cube_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================= 主题 ================= */
#define C_BG        0x0A0A12
#define C_ACCENT    0x40C8F8   /* 电台青 */
#define C_ACCENT_BT 0x8F7BFF   /* 蓝牙紫 */
#define C_TXT       0xE9EFF7
#define C_TXT2      0x8B98AC
#define C_DIM       0x3A4558

#define FONT_CJK  (&ui_font_cjk18)
#define FONT_CJK24 (&ui_font_cjk24)
#define FONT_M14  (&lv_font_montserrat_14)
#define FONT_M28  (&lv_font_montserrat_28)

/* ================= 几何 ================= */
#define LINE_W      460         /* 直线画布宽 */
#define LINE_H      220         /* 直线画布高(屏幕 y=160..380) */
#define LINE_CY     100         /* 直线中心(画布内) */
#define SPLASH_CUBE 150
#define SPLASH_MS   2400

#define SPEC_BANDS  40          /* 频谱带数 */
#define VOL_HUD_MS  700         /* 音量 HUD 停留 */
#define TOAST_MS    1600        /* 换台 toast 停留 */

/* ================= 对象引用 ================= */
static lv_obj_t *s_scr_main, *s_scr_splash;
static lv_obj_t *s_canvas_line;             /* ARGB 画布(音频直线) */
static lv_obj_t *s_lbl_brand;               /* 待机品牌标 */
static lv_obj_t *s_lbl_station;             /* 电台名(toast) */
static lv_obj_t *s_lbl_title;               /* 底部标题 */
static lv_obj_t *s_lbl_artist;              /* 底部艺术家/专辑 */
static lv_obj_t *s_lbl_lyr_cur;             /* 歌词当前行(飞入后停在中心) */
static lv_obj_t *s_lbl_lyr_next;            /* 歌词下一行(预示, 固定水平) */
static lv_obj_t *s_lbl_lyr_out;             /* 旧句克隆(向边缘加速飞离) */

/* 歌词飞行动画状态 */
static int  s_lyr_angle;                    /* 当前句倾角(0.1° 单位) */
static float s_lyr_dir_x, s_lyr_dir_y;      /* 当前句飞行方向(单位向量) */
static int  s_lyr_rest_x, s_lyr_rest_y;     /* 中心静止位(label 左上角) */
static int  s_screen_w = 460, s_screen_h = 460;
static lv_obj_t *s_hud;                     /* 音量 HUD 容器 */
static lv_obj_t *s_hud_fill;                /* 音量条填充 */
static lv_obj_t *s_hud_num;                 /* 音量数字 */
static lv_timer_t *s_hud_timer;             /* HUD 渐隐延时 */
static lv_timer_t *s_toast_timer;           /* toast 渐隐延时 */

/* 启动页 */
static lv_obj_t *s_canvas_splash_cube, *s_splash_title, *s_splash_sub;
static lv_obj_t *s_splash_dots[3];

/* ================= 缓冲 ================= */
static uint32_t     *s_buf_line;            /* LINE_W*LINE_H ARGB */
static lv_color16_t *s_buf_splash;          /* SPLASH_CUBE^2 */

/* ================= 动画状态 ================= */
static int16_t s_pcm[256 * 2];
static float    s_fft[128];
static float    s_band[SPEC_BANDS];         /* 平滑后 band 高度 0..1 */
static float    s_energy, s_bass;           /* 整体能量/低频(0..1) */
static float    s_curve[LINE_W];            /* 当前帧曲线 y(画布内) */
static bool     s_splash_active = true;
static uint32_t s_tick_cnt;

static void cube_draw(lv_color16_t *buf, int size, float ax, float ay,
                      float scale, uint8_t r, uint8_t g, uint8_t b);

/* 场景(派生) */
enum { SCENE_IDLE = 0, SCENE_SPECTRUM, SCENE_LYRICS };
static int s_scene = SCENE_IDLE;
static int  s_last_scene = -1;

/* 快照副本(变更检测) */
static char s_last_title[UI_STR_LEN]  = "\x01";
static char s_last_artist[UI_STR_LEN] = "\x01";
static char s_last_lyr[UI_SUB_LEN]    = "\x01";
static char s_last_lyr_next[UI_SUB_LEN] = "\x01";
static int  s_last_volume = -1;
static int  s_last_station = -1;
static int  s_last_station_num = -1;
static int  s_last_source = -1, s_last_player = -1;

/* ================= 手势(轴向锁定) ================= */
static lv_point_t s_press_p0;
static int      s_press_vol0;
static int8_t   s_axis;                     /* 0=未定 1=上下(音量) 2=左右(切曲) */
enum { AXIS_NONE = 0, AXIS_VOL, AXIS_SWITCH };

/* ============================================================ */
/*                        小工具                                 */
/* ============================================================ */
static lv_obj_t *label_make(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* ============================================================ */
/*                     缓动动画辅助                              */
/* ============================================================ */
static void anim_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

/* 渐入(透明度+上滑): dur ms, ease_out */
static void fade_in(lv_obj_t *obj, int32_t y_from, uint32_t dur)
{
    int32_t y_rest = lv_obj_get_y(obj);
    lv_anim_del(obj, anim_opa_cb);
    lv_anim_del(obj, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, y_from, y_rest);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 渐出(仅透明度) */
static void fade_out(lv_obj_t *obj, uint32_t dur)
{
    lv_anim_del(obj, anim_opa_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, 0);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/* ============================================================ */
/*                 音频直线 AA 渲染(ARGB)                       */
/* ============================================================ */
static inline void line_px(int x, int y, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a)
{
    if (x < 0 || x >= LINE_W || y < 0 || y >= LINE_H || a == 0) return;
    uint32_t *px = &s_buf_line[y * LINE_W + x];
    uint32_t da = *px >> 24;
    uint8_t na = (uint8_t)(a + da * (255 - a) / 255);       /* over 合成 */
    if (na == 0) return;
    uint32_t dr = (*px >> 16) & 0xFF, dg = (*px >> 8) & 0xFF, db = *px & 0xFF;
    uint32_t nr = (r * a + dr * da * (255 - a) / 255) / na;
    uint32_t ng = (g * a + dg * da * (255 - a) / 255) / na;
    uint32_t nb = (b * a + db * da * (255 - a) / 255) / na;
    *px = ((uint32_t)na << 24) | (nr << 16) | (ng << 8) | nb;
}

/* 软圆盘(距离场 AA) */
static void line_disc(float cx, float cy, float rad, uint8_t r, uint8_t g,
                      uint8_t b, uint8_t a)
{
    int ri = (int)rad + 2;
    int x0 = (int)cx - ri, x1 = (int)cx + ri;
    int y0 = (int)cy - ri, y1 = (int)cy + ri;
    if (x1 < 0 || x0 >= LINE_W || y1 < 0 || y0 >= LINE_H) return;
    for (int y = y0; y <= y1; y++) {
        float dyy = y - cy;
        for (int x = x0; x <= x1; x++) {
            float dxx = x - cx;
            float d = sqrtf(dxx * dxx + dyy * dyy);
            float edge = rad - d;
            float cov = edge > 1.0f ? 1.0f : (edge < 0 ? 0.0f : edge);
            if (cov > 0) line_px(x, y, r, g, b, (uint8_t)(a * cov));
        }
    }
}

static void line_color(uint8_t *r, uint8_t *g, uint8_t *b, const ui_model_t *m)
{
    if (m->source == UI_SRC_BT)         { *r = 0x8F; *g = 0x7B; *b = 0xFF; }
    else if (m->source == UI_SRC_RADIO) { *r = 0x40; *g = 0xC8; *b = 0xF8; }
    else                                { *r = 0x46; *g = 0x54; *b = 0x6E; }
}

/* ============================================================ */
/*                     频谱带计算                                */
/* ============================================================ */
static void bands_update(bool playing)
{
    int got = playing ? ui_audio_take_pcm(s_pcm, 256) : 0;
    if (got > 0) {
        static int16_t mono[256];
        int32_t acc = 0;
        for (int i = 0; i < 256; i++) {
            mono[i] = (int16_t)(((int32_t)s_pcm[i * 2] + s_pcm[i * 2 + 1]) >> 1);
            acc += mono[i] > 0 ? mono[i] : -mono[i];
        }
        float level = (float)acc / 256 / 5000;
        if (level > 1) level = 1;
        s_energy = s_energy * 0.6f + level * 0.4f;
        cube_fft_magnitude(mono, 256, s_fft);
        float bass = 0;
        for (int k = 1; k <= 8; k++) bass += s_fft[k];
        s_bass = s_bass * 0.7f + (bass / 8 / 900000.0f) * 0.3f;
        if (s_bass > 1) s_bass = 1;

        for (int b = 0; b < SPEC_BANDS; b++) {
            /* 对数频率分组(与混音器抽头一致) */
            int lo = (int)(powf((float)b / SPEC_BANDS, 1.8f) * 100);
            int hi = (int)(powf((float)(b + 1) / SPEC_BANDS, 1.8f) * 100);
            if (hi <= lo) hi = lo + 1;
            float mag = 0;
            for (int k = lo; k < hi && k < 128; k++) {
                if (s_fft[k] > mag) mag = s_fft[k];
            }
            /* log 域压缩 + 高带倾斜补偿(音乐能量天然集中低频,
             * 每倍频程 -6dB, 不补偿高频纹理不可见) */
            mag *= 1.0f + 2.2f * (float)b / SPEC_BANDS;
            /* mag 1e3→0.31 / 1e5→0.85 / 2.5e5→1.0 */
            float h = log10f(1.0f + mag * 0.01f) / 3.4f;
            if (h > 1) h = 1;
            s_band[b] = h > s_band[b] ? h : s_band[b] * 0.80f;  /* 攻快放慢 */
        }
    } else {
        for (int b = 0; b < SPEC_BANDS; b++) s_band[b] *= 0.86f;
        s_energy *= 0.92f;
        s_bass *= 0.92f;
    }
}

/* ============================================================ */
/*                   直线绘制(每帧)                              */
/* ============================================================ */
static void line_draw(const ui_model_t *m)
{
    memset(s_buf_line, 0, sizeof(uint32_t) * LINE_W * LINE_H);

    bool idle = s_scene == SCENE_IDLE;
    bool lyrics_mode = s_scene == SCENE_LYRICS;

    /* 振幅: 频谱全开 / 歌词衬底 / 待机呼吸 */
    float amp = idle ? 3.0f + 1.2f * sinf(s_tick_cnt * 0.052f)
              : lyrics_mode ? 18.0f + 8.0f * s_energy
              : 40.0f + 60.0f * s_energy + 12.0f * s_bass;
    if (amp > 100) amp = 100;
    float cy = idle ? LINE_CY : lyrics_mode ? LINE_H - 34 : LINE_CY;

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
        float p0 = s_band[i0], p1 = s_band[i], p2 = s_band[i + 1], p3 = s_band[i3];
        /* Catmull-Rom 基函数 */
        float t2 = t * t, t3 = t2 * t;
        float h = (2 * t3 - 3 * t2 + 1) * p1 + (-2 * t3 + 3 * t2) * p2 +
                  (t3 - 2 * t2 + t) * 0.5f * (p2 - p0) +
                  (t3 - t2) * 0.5f * (p3 - p1);
        s_curve[x] = cy - h * amp;
    }

    uint8_t cr, cg, cb;
    line_color(&cr, &cg, &cb, m);
    float dim = idle ? 0.45f : 0.35f + 0.65f * s_energy;

    /* ---- 辉光层(软笔刷稀疏采样, 无节点亮斑) ---- */
    for (int x = 0; x < LINE_W; x += 2) {
        line_disc((float)x, s_curve[x], 9.0f, cr, cg, cb,
                  (uint8_t)(14 * dim));
        line_disc((float)x, s_curve[x], 3.5f, cr, cg, cb,
                  (uint8_t)(60 * dim));
    }
    /* ---- 芯线(逐列垂直 AA) ---- */
    int core = (int)(235 * (dim > 1.0f ? 1.0f : dim));
    for (int x = 0; x < LINE_W; x++) {
        float yf = s_curve[x];
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
            line_disc((float)x, 2 * cy - s_curve[x] + 14, 2.2f, cr, cg, cb,
                      (uint8_t)ra);
        }
    }
    lv_obj_invalidate(s_canvas_line);
}

/* ============================================================ */
/*                     音量 HUD                                  */
/* ============================================================ */
static void hud_hide_cb(lv_timer_t *t)
{
    (void)t;
    if ((int)lv_obj_get_style_opa(s_hud, 0) == 0) return;
    fade_out(s_hud, 450);
}

static void hud_show(void)
{
    int vol = ui_model()->volume;
    lv_obj_set_style_opa(s_hud, LV_OPA_COVER, 0);
    lv_obj_set_height(s_hud_fill, 150 * vol / 100);
    lv_label_set_text_fmt(s_hud_num, "%d", vol);
    lv_timer_reset(s_hud_timer);
}

static void volume_sync(int vol)
{
    if (vol == s_last_volume) return;
    bool first = s_last_volume == -1;
    s_last_volume = vol;
    if (!first) hud_show();                      /* 首次同步不弹 HUD */
}

/* ============================================================ */
/*                     手势(轴向锁定)                            */
/* ============================================================ */
static void gesture_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_indev_get_point(in, &s_press_p0);
    s_axis = AXIS_NONE;
    s_press_vol0 = ui_model()->volume;
}

static void gesture_pressing_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int dx = p.x - s_press_p0.x;
    int dy = p.y - s_press_p0.y;

    if (s_axis == AXIS_NONE) {
        if (abs(dx) > 14 && abs(dx) > abs(dy)) s_axis = AXIS_SWITCH;
        else if (abs(dy) > 14) s_axis = AXIS_VOL;
        else return;
    }
    if (s_axis == AXIS_VOL) {
        ui_ctl_set_volume(s_press_vol0 - dy * 45 / 100);   /* 全程≈222px */
    }
}

static void gesture_release_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = s_press_p0;
    if (in != NULL) lv_indev_get_point(in, &p);
    int dx = p.x - s_press_p0.x;
    if (s_axis == AXIS_SWITCH && abs(dx) > 60) {
        ui_ctl_switch_station(dx < 0 ? 1 : -1);            /* 左滑=下一曲 */
    }
    s_axis = AXIS_NONE;
}

/* ============================================================ */
/*                     场景/信息同步                             */
/* ============================================================ */
static void lyric_fly_out(void);             /* 前向: scene_apply 尾句飞离 */

static void toast_station(const ui_model_t *m)
{
    int n = m->station_num > 0 ? m->station_num : 1;
    lv_label_set_text(s_lbl_station, m->station_names[m->station_idx % n]);
    lv_obj_set_style_opa(s_lbl_station, LV_OPA_COVER, 0);
    fade_in(s_lbl_station, 8, 260);
    lv_timer_reset(s_toast_timer);
}

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if ((int)lv_obj_get_style_opa(s_lbl_station, 0) == 0) return;
    fade_out(s_lbl_station, 400);
}

static void scene_apply(int scene)
{
    if (scene == s_last_scene) return;
    int prev = s_last_scene;
    s_last_scene = scene;
    s_scene = scene;

    if (prev == -1) return;                      /* 启动页后首帧不做退场动画 */
    if (scene == SCENE_IDLE) {
        fade_in(s_lbl_brand, 6, 300);
        if (prev == SCENE_LYRICS) lyric_fly_out();   /* 尾句飞离 */
        else fade_out(s_lbl_lyr_cur, 260);
        fade_out(s_lbl_lyr_next, 260);
        fade_out(s_lbl_title, 260);
        fade_out(s_lbl_artist, 260);
        if (prev == SCENE_SPECTRUM || prev == SCENE_LYRICS) {
            fade_out(s_lbl_station, 400);
        }
    } else {
        fade_out(s_lbl_brand, 200);
        if (scene == SCENE_LYRICS) {
            /* 歌词是唯一主角: 直线让位(每帧不再画), 清掉残留帧 */
            if (s_buf_line != NULL) {
                memset(s_buf_line, 0, sizeof(uint32_t) * LINE_W * LINE_H);
                lv_obj_invalidate(s_canvas_line);
            }
            lv_obj_set_style_opa(s_lbl_lyr_next, 255, 0);
        } else {
            if (prev == SCENE_LYRICS) lyric_fly_out();   /* 切纯音乐: 句尾飞离 */
            else fade_out(s_lbl_lyr_cur, 260);
            fade_out(s_lbl_lyr_next, 260);
        }
    }
}

/* ---- 歌词飞行(流星式):
 *   飞入: 从随机边缘方向出发, ease_out —— 越接近中心越慢;
 *   飞离: 沿同方向穿过中心加速飞向对侧边缘, ease_in;
 *   倾角: 每句随机 ±4~13°, 不与飞行方向绑定(错落感)。
 *   旧句由 s_lbl_lyr_out 克隆接管飞行, 新句同时飞入 —— 交叉而过。
 *   出发/终点钳制在屏内: 旋转 label 完全出屏时 LVGL 9.6.0 的
 *   离屏 layer 裁剪区会退化 → 缓冲分配失败 → 该句不渲染。 */
#define LYR_FLY_IN_MS    480
#define LYR_FLY_OUT_MS   420
#define LYR_FLY_DIST     330                 /* 出发/终点半径 */

static int lyr_clamp_x(int x)
{
    if (x < 8) x = 8;
    if (x > s_screen_w - 320) x = s_screen_w - 320;   /* label 宽 430, 留一角在屏内 */
    return x;
}

static int lyr_clamp_y(int y)
{
    if (y < 6) y = 6;
    if (y > s_screen_h - 46) y = s_screen_h - 46;
    return y;
}

static void lyric_out_anim_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_style_opa(s_lbl_lyr_out, 0, 0);       /* 到边缘即隐(非动画) */
}

static void lyric_fly_out(void)
{
    /* 无旧句在中心则忽略 */
    if ((int)lv_obj_get_style_opa(s_lbl_lyr_cur, 0) == 0) return;

    /* 克隆旧句(文本/倾角/位置), 沿同方向加速飞离 */
    lv_label_set_text(s_lbl_lyr_out, lv_label_get_text(s_lbl_lyr_cur));
    lv_obj_set_style_transform_rotation(s_lbl_lyr_out, s_lyr_angle, 0);
    lv_obj_set_style_opa(s_lbl_lyr_out, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_lbl_lyr_out, s_lyr_rest_x, s_lyr_rest_y);
    int ex = lyr_clamp_x(s_lyr_rest_x + (int)(s_lyr_dir_x * LYR_FLY_DIST));
    int ey = lyr_clamp_y(s_lyr_rest_y + (int)(s_lyr_dir_y * LYR_FLY_DIST));

    /* 只做位移(至边缘即隐); rotation+opa 组合会触发 9.6.0
     * 离屏 layer 渲染缺陷, 勿加淡出 */
    lv_anim_del(s_lbl_lyr_out, anim_x_cb);
    lv_anim_del(s_lbl_lyr_out, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lbl_lyr_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, s_lyr_rest_x, ex);
    lv_anim_set_duration(&a, LYR_FLY_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, lyric_out_anim_done_cb);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, s_lyr_rest_y, ey);
    lv_anim_start(&a);

    /* 当前句立刻让位, 等新句飞入 */
    lv_anim_del(s_lbl_lyr_cur, anim_x_cb);
    lv_anim_del(s_lbl_lyr_cur, anim_y_cb);
    lv_obj_set_style_opa(s_lbl_lyr_cur, 0, 0);
}

static void lyric_fly_in(const char *text)
{
    lv_label_set_text(s_lbl_lyr_cur, text);

    /* 随机倾角 ±4~13°(0.1° 单位) */
    int deg = 40 + rand() % 90;                  /* 40..130 = 4..13° */
    s_lyr_angle = (rand() % 2 ? 1 : -1) * deg;
    lv_obj_set_style_transform_rotation(s_lbl_lyr_cur, s_lyr_angle, 0);

    /* 随机飞入方向(单位向量) */
    float ang = (float)(rand() % 360) * (float)M_PI / 180.0f;
    s_lyr_dir_x = cosf(ang);
    s_lyr_dir_y = sinf(ang);

    /* ease_out 飞入: 边缘 → 中心(越接近越慢)。
     * 只做位移不叠 opa 动画(rotation+opa 组合触发 9.6.0 离屏
     * layer 渲染缺陷); 出发点在屏外, 入场感由位移本身提供 */
    int sx = lyr_clamp_x(s_lyr_rest_x - (int)(s_lyr_dir_x * LYR_FLY_DIST));
    int sy = lyr_clamp_y(s_lyr_rest_y - (int)(s_lyr_dir_y * LYR_FLY_DIST));
    lv_obj_set_pos(s_lbl_lyr_cur, sx, sy);
    lv_obj_set_style_opa(s_lbl_lyr_cur, LV_OPA_COVER, 0);

    lv_anim_del(s_lbl_lyr_cur, anim_x_cb);
    lv_anim_del(s_lbl_lyr_cur, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lbl_lyr_cur);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, sx, s_lyr_rest_x);
    lv_anim_set_duration(&a, LYR_FLY_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, sy, s_lyr_rest_y);
    lv_anim_start(&a);
}

static void lyrics_apply(const ui_model_t *m)
{
    if (m->lyric_cur[0] && strcmp(m->lyric_cur, s_last_lyr) != 0) {
        strncpy(s_last_lyr, m->lyric_cur, sizeof(s_last_lyr) - 1);
        s_last_lyr[sizeof(s_last_lyr) - 1] = 0;
        lyric_fly_out();                         /* 旧句加速飞离 */
        lyric_fly_in(m->lyric_cur);              /* 新句减速飞入 */
    }
    if (strcmp(m->lyric_next, s_last_lyr_next) != 0) {
        strncpy(s_last_lyr_next, m->lyric_next, sizeof(s_last_lyr_next) - 1);
        s_last_lyr_next[sizeof(s_last_lyr_next) - 1] = 0;
        lv_label_set_text(s_lbl_lyr_next,
                          m->lyric_next[0] ? m->lyric_next : " ");
    }
}

static void info_apply(const ui_model_t *m)
{
    /* 底部标题(播放态) */
    const char *ti = m->title;
    if (strcmp(ti, s_last_title) != 0) {
        strncpy(s_last_title, ti, sizeof(s_last_title) - 1);
        s_last_title[sizeof(s_last_title) - 1] = 0;
        lv_label_set_text(s_lbl_title, ti[0] ? ti : " ");
    }
    /* 底部第二行: BT=艺术家·专辑; 电台=空(台名走 toast) */
    char line[UI_STR_LEN + 8];
    if (m->source == UI_SRC_BT) {
        snprintf(line, sizeof(line), "%s%s%s", m->artist,
                 m->album[0] ? " · " : "", m->album);
    } else {
        line[0] = 0;
    }
    if (strcmp(line, s_last_artist) != 0) {
        strncpy(s_last_artist, line, sizeof(s_last_artist) - 1);
        s_last_artist[sizeof(s_last_artist) - 1] = 0;
        lv_label_set_text(s_lbl_artist, line[0] ? line : " ");
    }

    /* 音源切换 → 电台顶部 toast 常驻态/BT 隐藏 */
    if (m->source != s_last_source) {
        s_last_source = m->source;
        s_last_station = -1;
        if (m->source == UI_SRC_RADIO) toast_station(m);
        else if (m->source == UI_SRC_BT) fade_out(s_lbl_station, 200);
    }
    /* 电台换台 toast */
    if (m->source == UI_SRC_RADIO &&
        (m->station_idx != s_last_station ||
         m->station_num != s_last_station_num)) {
        s_last_station = m->station_idx;
        s_last_station_num = m->station_num;
        if (s_last_station_num > 0) toast_station(m);
    }
    if (m->player != s_last_player) s_last_player = m->player;
}

static void model_sync(const ui_model_t *m)
{
    bool playing = m->player == UI_PLAYER_PLAYING ||
                   (m->source == UI_SRC_BT && m->bt_connected);
    int scene = !playing ? SCENE_IDLE
              : (m->source == UI_SRC_BT && m->has_lyrics) ? SCENE_LYRICS
              : SCENE_SPECTRUM;
    scene_apply(scene);
    info_apply(m);
    lyrics_apply(m);
    volume_sync(m->volume);
}

/* ============================================================ */
/*                     主定时器(30fps)                           */
/* ============================================================ */
static void master_timer_cb(lv_timer_t *t)
{
    (void)t;
    const ui_model_t *m = ui_model();
    s_tick_cnt++;

    bool playing = m->player == UI_PLAYER_PLAYING ||
                   (m->source == UI_SRC_BT && m->bt_connected);
    bands_update(playing);

    if (s_splash_active) {
        uint32_t el = s_tick_cnt * 33;
        if (s_buf_splash != NULL) {
            memset(s_buf_splash, 0,
                   sizeof(lv_color16_t) * SPLASH_CUBE * SPLASH_CUBE);
            cube_draw(s_buf_splash, SPLASH_CUBE, 0.5f, el * 0.0022f,
                      46 + 4 * sinf(el * 0.004f), 0x40, 0xC8, 0xF8);
            lv_obj_invalidate(s_canvas_splash_cube);
        }
        if (el > 300) {
            int opa = (int)(el - 300) * 255 / 500;
            if (opa > 255) opa = 255;
            lv_obj_set_style_text_opa(s_splash_title, (lv_opa_t)opa, 0);
            int opa2 = opa > 100 ? (opa - 100) * 255 / 155 : 0;
            lv_obj_set_style_text_opa(s_splash_sub, (lv_opa_t)opa2, 0);
        }
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(el / 800.0f + i * 0.33f, 1.0f);
            int opa = (int)(255 * (ph < 0.5f ? ph * 2 : 2 - ph * 2));
            lv_obj_set_style_bg_opa(s_splash_dots[i], (lv_opa_t)opa, 0);
        }
        if (el >= SPLASH_MS) {
            s_splash_active = false;
            lv_screen_load_anim(s_scr_main, LV_SCR_LOAD_ANIM_FADE_IN,
                                350, 0, true);
        }
        return;
    }

    if (s_scene != SCENE_LYRICS) {
        line_draw(m);                            /* 歌词场景: 直线让位 */
    }
    model_sync(m);
}

/* ============================================================ */
/*                     3D 立方体(启动页用)                       */
/* ============================================================ */
static void cube_draw(lv_color16_t *buf, int size, float ax, float ay,
                      float scale, uint8_t r, uint8_t g, uint8_t b)
{
    static const float verts[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
    };
    static const int edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    const float c = size / 2.0f;
    const lv_color16_t col = { .red = r >> 3, .green = g >> 2, .blue = b >> 3 };
    float cosx = cosf(ax), sinx = sinf(ax);
    float cosy = cosf(ay), siny = sinf(ay);
    lv_point_t pts[8];
    for (int i = 0; i < 8; i++) {
        float x = verts[i][0], y = verts[i][1], z = verts[i][2];
        float x1 = x * cosy + z * siny;
        float z1 = -x * siny + z * cosy;
        float y1 = y * cosx - z1 * sinx;
        pts[i].x = (int)(c + x1 * scale);
        pts[i].y = (int)(c + y1 * scale);
    }
    for (int e = 0; e < 12; e++) {
        int x0 = pts[edges[e][0]].x, y0 = pts[edges[e][0]].y;
        int x1 = pts[edges[e][1]].x, y1 = pts[edges[e][1]].y;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            if (x0 >= 0 && x0 < size && y0 >= 0 && y0 < size) {
                buf[y0 * size + x0] = col;
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

/* ============================================================ */
/*                     启动页                                    */
/* ============================================================ */
static void splash_free_cb(lv_event_t *e)
{
    (void)e;
    free(s_buf_splash);
    s_buf_splash = NULL;
}

static void splash_create(void)
{
    s_scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_splash, lv_color_hex(C_BG), 0);

    s_buf_splash = malloc(sizeof(lv_color16_t) * SPLASH_CUBE * SPLASH_CUBE);
    if (s_buf_splash != NULL) {
        s_canvas_splash_cube = lv_canvas_create(s_scr_splash);
        lv_canvas_set_buffer(s_canvas_splash_cube, s_buf_splash, SPLASH_CUBE,
                             SPLASH_CUBE, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_canvas_splash_cube, LV_ALIGN_CENTER, 0, -34);
    }

    s_splash_title = label_make(s_scr_splash, FONT_M28, C_TXT);
    lv_label_set_text(s_splash_title, "EspCube");
    lv_obj_set_style_text_opa(s_splash_title, 0, 0);
    lv_obj_align(s_splash_title, LV_ALIGN_CENTER, 0, 66);

    s_splash_sub = label_make(s_scr_splash, FONT_M14, C_TXT2);
    lv_label_set_text(s_splash_sub, "AI CUBE SPEAKER");
    lv_obj_set_style_text_opa(s_splash_sub, 0, 0);
    lv_obj_align(s_splash_sub, LV_ALIGN_CENTER, 0, 100);

    for (int i = 0; i < 3; i++) {
        s_splash_dots[i] = lv_obj_create(s_scr_splash);
        lv_obj_remove_style_all(s_splash_dots[i]);
        lv_obj_set_size(s_splash_dots[i], 8, 8);
        lv_obj_set_style_radius(s_splash_dots[i], 4, 0);
        lv_obj_set_style_bg_opa(s_splash_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_splash_dots[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_align_to(s_splash_dots[i], s_splash_title,
                        LV_ALIGN_OUT_BOTTOM_MID, (i - 1) * 18, 24);
    }
    lv_obj_add_event_cb(s_scr_splash, splash_free_cb, LV_EVENT_DELETE, NULL);
}

/* ============================================================ */
/*                     构建入口                                  */
/* ============================================================ */
void ui_pages_create(int w, int h)
{
    (void)w; (void)h;

    /* ---- 主屏 ---- */
    s_scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_main, lv_color_hex(C_BG), 0);

    /* 音频直线画布(ARGB, LVGL 合成) */
    s_buf_line = malloc(sizeof(uint32_t) * LINE_W * LINE_H);
    if (s_buf_line != NULL) {
        s_canvas_line = lv_canvas_create(s_scr_main);
        lv_canvas_set_buffer(s_canvas_line, s_buf_line, LINE_W, LINE_H,
                             LV_COLOR_FORMAT_ARGB8888);
        lv_obj_align(s_canvas_line, LV_ALIGN_TOP_MID, 0, 160);
    }

    /* 待机品牌标 */
    s_lbl_brand = label_make(s_scr_main, FONT_M14, C_DIM);
    lv_label_set_text(s_lbl_brand, "EspCube");
    lv_obj_set_style_opa(s_lbl_brand, 0, 0);
    lv_obj_align(s_lbl_brand, LV_ALIGN_TOP_MID, 0, 30);

    /* 电台 toast(顶部) */
    s_lbl_station = label_make(s_scr_main, FONT_CJK, C_TXT2);
    lv_obj_set_style_opa(s_lbl_station, 0, 0);
    lv_obj_align(s_lbl_station, LV_ALIGN_TOP_MID, 0, 26);

    /* 歌词(当前行 24px, 飞行/倾斜; 下一行 18px 暗色预示, 固定水平) */
    s_screen_w = w;
    s_screen_h = h;
    s_lyr_rest_x = (w - 430) / 2;                /* 中心静止位(label 左上角) */
    s_lyr_rest_y = 172;
    s_lbl_lyr_cur = label_make(s_scr_main, FONT_CJK24, C_TXT);
    lv_obj_set_size(s_lbl_lyr_cur, 430, 34);     /* 固定高: 旋转 layer 的
                                                 bbox 依赖内容高, LV_SIZE_CONTENT
                                                 在 DOT/空文本时可为 0 → 分配失败 */
    lv_label_set_long_mode(s_lbl_lyr_cur, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_lyr_cur, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(s_lbl_lyr_cur, 0, 0);
    /* 旋转轴心 = label 几何中心(倾斜绕中心, 不绕左上角甩尾) */
    lv_obj_set_style_transform_pivot_x(s_lbl_lyr_cur, 215, 0);
    lv_obj_set_style_transform_pivot_y(s_lbl_lyr_cur, 17, 0);
    lv_obj_set_pos(s_lbl_lyr_cur, s_lyr_rest_x, s_lyr_rest_y);
    /* 旧句克隆: 只做飞离动画, 常态隐藏 */
    s_lbl_lyr_out = label_make(s_scr_main, FONT_CJK24, C_TXT);
    lv_obj_set_size(s_lbl_lyr_out, 430, 34);
    lv_label_set_long_mode(s_lbl_lyr_out, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_lyr_out, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_pivot_x(s_lbl_lyr_out, 215, 0);
    lv_obj_set_style_transform_pivot_y(s_lbl_lyr_out, 17, 0);
    lv_obj_set_style_opa(s_lbl_lyr_out, 0, 0);
    lv_obj_set_pos(s_lbl_lyr_out, s_lyr_rest_x, s_lyr_rest_y);
    s_lbl_lyr_next = label_make(s_scr_main, FONT_CJK, 0x5E6C80);
    lv_obj_set_width(s_lbl_lyr_next, 430);
    lv_label_set_long_mode(s_lbl_lyr_next, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_lyr_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(s_lbl_lyr_next, 0, 0);
    lv_obj_align(s_lbl_lyr_next, LV_ALIGN_TOP_MID, 0, 224);

    /* 底部标题/艺术家 */
    s_lbl_title = label_make(s_scr_main, FONT_CJK24, C_TXT);
    lv_obj_set_width(s_lbl_title, 430);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_opa(s_lbl_title, 0, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_BOTTOM_MID, 0, -58);
    s_lbl_artist = label_make(s_scr_main, FONT_CJK, C_TXT2);
    lv_obj_set_width(s_lbl_artist, 420);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(s_lbl_artist, 0, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_BOTTOM_MID, 0, -28);

    /* 音量 HUD(右缘竖条+数字) */
    s_hud = lv_obj_create(s_scr_main);
    lv_obj_remove_style_all(s_hud);
    lv_obj_set_size(s_hud, 64, 180);
    lv_obj_align(s_hud, LV_ALIGN_RIGHT_MID, -12, 36);   /* 下移避让直线 */
    lv_obj_set_style_opa(s_hud, 0, 0);
    lv_obj_t *track = lv_obj_create(s_hud);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 5, 150);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
    s_hud_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_hud_fill);
    lv_obj_set_width(s_hud_fill, 5);
    lv_obj_set_style_bg_color(s_hud_fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_hud_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_hud_fill, 3, 0);
    lv_obj_align(s_hud_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_hud_num = label_make(s_hud, FONT_M28, C_TXT);
    lv_obj_align(s_hud_num, LV_ALIGN_LEFT_MID, 0, 0);

    /* 常驻运行 + reset 续期; cb 内已有 opa==0 短路, 空转无害 */
    s_hud_timer = lv_timer_create(hud_hide_cb, VOL_HUD_MS, NULL);
    s_toast_timer = lv_timer_create(toast_hide_cb, TOAST_MS, NULL);

    /* 手势 */
    lv_obj_add_event_cb(s_scr_main, gesture_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr_main, gesture_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr_main, gesture_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr_main, gesture_release_cb, LV_EVENT_PRESS_LOST, NULL);

    /* ---- 启动页 ---- */
    splash_create();
    lv_screen_load(s_scr_splash);

    lv_timer_create(master_timer_cb, 33, NULL);
}
