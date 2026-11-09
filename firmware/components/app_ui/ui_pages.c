/*
 * ui_pages.c —— 页面与控件实现(播放页: 立方体 + 频谱 + 控制条)
 *
 * 性能预算(设计文档 §4, 实测为准):
 *   - 频谱: 30fps(33ms lv_timer), 256点FFT ~0.3ms + 32柱绘制 ~1ms;
 *   - 3D立方体: 软件线框投影 ~0.5ms/帧, 30fps;
 *   - 全帧刷新 QSPI 80MHz: 一帧 ~13ms(理论), 全帧双缓冲无撕裂;
 *   总 CPU(核1) 约 30-45%, 满足 60Hz 目标。
 *
 * 所有 UI 对象仅在 LVGL 任务上下文访问(lv_timer/事件回调均在其中),
 * 跨任务入口(set_volume 等)必须在调用方加 lvgl_port_lock。
 */
#include "ui_pages.h"
#include "app_ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "app_bt.h"

static const char *TAG = "ui_pages";

/* ---- 电台列表(原型内置; M3 迁移到 NVS/云配置) ---- */
static const char *s_stations[] = {
    CONFIG_ESP_CUBE_DEFAULT_STATION,
    "http://ice1.somafm.com/groovesalad-128-mp3",
    "http://ice1.somafm.com/dronezone-128-mp3",
};
#define STATION_NUM (sizeof(s_stations) / sizeof(s_stations[0]))
static int s_station_idx = 0;

/* ---- 对象引用 ---- */
static lv_obj_t *s_lbl_source;      /* 音源状态行 */
static lv_obj_t *s_lbl_meta;        /* 曲目/电台标题 */
static lv_obj_t *s_canvas_spec;     /* 频谱 canvas */
static lv_obj_t *s_canvas_cube;     /* 立方体 canvas */
static lv_obj_t *s_slider_vol;      /* 音量滑条 */
static lv_obj_t *s_btn_play;        /* 播放/暂停 */
static lv_obj_t *s_lbl_play;        /* 播放键文本 */

/* ---- 频谱缓冲(PSRAM 动态分配, 避免 .bss 挤占内部 SRAM) ---- */
#define SPEC_BARS       32
#define SPEC_H          120
#define SPEC_W          440
#define CUBE_SIZE       140
static int16_t s_spec_pcm[256 * 2];         /* 抽样 PCM(双声道, 内部RAM 1KB) */
static float  s_spec_fft[128];              /* FFT 幅度(内部RAM 512B) */
static lv_color16_t *s_spec_buf;            /* 105KB → PSRAM */
static lv_color16_t *s_cube_buf;            /* 39KB  → PSRAM */

/* ================= 频谱绘制 ================= */
/* 直接写 canvas 像素缓冲: 32 根柱, 低频在左(按对数分组) */
static void spectrum_draw(void)
{
    /* 取混音器最近输出帧 → 下混单声道 → FFT */
    audio_mixer_take_spectrum(s_spec_pcm, 256);
    static int16_t mono[256];
    for (int i = 0; i < 256; i++) {
        mono[i] = (int16_t)(((int32_t)s_spec_pcm[i * 2] + s_spec_pcm[i * 2 + 1]) >> 1);
    }
    cube_fft_magnitude(mono, 256, s_spec_fft);

    memset(s_spec_buf, 0, sizeof(lv_color16_t) * SPEC_W * SPEC_H);
    const int bar_w = SPEC_W / SPEC_BARS - 2;
    for (int b = 0; b < SPEC_BARS; b++) {
        /* 对数频率映射: 第 b 根柱覆盖 FFT bin 区间(低密高疏, 符合听感) */
        int lo = (int)(powf((float)b / SPEC_BARS, 1.8f) * 100);
        int hi = (int)(powf((float)(b + 1) / SPEC_BARS, 1.8f) * 100);
        if (hi <= lo) hi = lo + 1;
        float m = 0;
        for (int k = lo; k < hi && k < 128; k++) {
            if (s_spec_fft[k] > m) m = s_spec_fft[k];
        }
        /* 幅度→柱高(压缩动态范围) */
        int h = (int)(sqrtf(m) * 4.0f);
        if (h > SPEC_H - 4) h = SPEC_H - 4;
        if (h < 2) h = 2;
        int x0 = b * (SPEC_W / SPEC_BARS) + 1;
        /* 青色渐变柱(RGB565: red5/green6/blue5) */
        for (int y = 0; y < h; y++) {
            uint8_t g = (uint8_t)(63 - (y * 63) / SPEC_H);
            for (int x = 0; x < bar_w; x++) {
                s_spec_buf[(SPEC_H - 1 - y) * SPEC_W + x0 + x] =
                    (lv_color16_t){ .red = 2, .green = g, .blue = 31 };
            }
        }
    }
    lv_obj_invalidate(s_canvas_spec);
}

/* ================= 3D 立方体(线框投影) ================= */
static void cube_draw(float t)
{
    /* 8 顶点/12 棱, 绕 X/Y 轴旋转后正交投影到 140x140 */
    static const float verts[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
    };
    static const int edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    memset(s_cube_buf, 0, sizeof(lv_color16_t) * CUBE_SIZE * CUBE_SIZE);

    const float cx = 70, cy = 70, scale = 34.0f;
    float ax = t * 0.9f, ay = t * 1.3f;
    float cosx = cosf(ax), sinx = sinf(ax), cosy = cosf(ay), siny = sinf(ay);
    lv_point_t pts[8];
    for (int i = 0; i < 8; i++) {
        float x = verts[i][0], y = verts[i][1], z = verts[i][2];
        /* 绕 Y 旋转 */
        float x1 = x * cosy + z * siny;
        float z1 = -x * siny + z * cosy;
        /* 绕 X 旋转 */
        float y1 = y * cosx - z1 * sinx;
        pts[i].x = (int)(cx + x1 * scale);
        pts[i].y = (int)(cy + y1 * scale);
    }
    /* Bresenham 画 12 条棱(白色) */
    for (int e = 0; e < 12; e++) {
        int x0 = pts[edges[e][0]].x, y0 = pts[edges[e][0]].y;
        int x1 = pts[edges[e][1]].x, y1 = pts[edges[e][1]].y;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            if (x0 >= 0 && x0 < CUBE_SIZE && y0 >= 0 && y0 < CUBE_SIZE) {
                s_cube_buf[y0 * CUBE_SIZE + x0] = (lv_color16_t){ .red = 31, .green = 63, .blue = 31 };
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    lv_obj_invalidate(s_canvas_cube);
}

/* ================= 定时器(30fps) ================= */
static void anim_timer_cb(lv_timer_t *timer)
{
    static float t = 0;
    t += 0.03f * 2.0f;      /* 假定 30fps, 旋转速率 2rad/s */
    spectrum_draw();
    cube_draw(t);
}

/* ================= 控件事件(均在 LVGL 任务上下文, 无需加锁) ================= */
static void vol_slider_cb(lv_event_t *e)
{
    int vol = (int)lv_slider_get_value(lv_event_get_target(e));
    audio_mixer_set_master_volume(vol);
}

static void play_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 原型: 播放键控制网络电台(蓝牙播放由手机端控制; M3 加 AVRCP 透传) */
    if (app_player_get_state() == PLAYER_PLAYING) {
        app_player_stop();
    } else {
        app_player_start(s_stations[s_station_idx]);
    }
}

static void station_switch(int dir)
{
    s_station_idx = (s_station_idx + dir + STATION_NUM) % STATION_NUM;
    app_player_start(s_stations[s_station_idx]);
}

static void prev_btn_cb(lv_event_t *e) { (void)e; station_switch(-1); }
static void next_btn_cb(lv_event_t *e) { (void)e; station_switch(1); }

/* ================= 事件总线订阅(非 LVGL 任务, 需要加锁更新) ================= */
static void on_stream_title(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    /* 事件总线为全量派发: 只处理自己关心的事件(其余忽略) */
    if (id != CUBE_EVT_STREAM_TITLE || data == NULL) return;
    (void)len; (void)ctx;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_lbl_meta, "%s", (const char *)data);
    lvgl_port_unlock();
}

static void on_bt_meta(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)id; (void)len; (void)ctx;
    if (id != CUBE_EVT_BT_META || data == NULL) return;
    const cube_bt_meta_t *m = data;
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_lbl_meta, "%s - %s", m->artist, m->title);
    lvgl_port_unlock();
}

static void on_player_state(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    if (id != CUBE_EVT_PLAYER_STATE || data == NULL) return;
    (void)len; (void)ctx;
    int state = *(const int *)data;
    lvgl_port_lock(0);
    lv_label_set_text(s_lbl_play, state == PLAYER_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    const char *src = (state == PLAYER_PLAYING) ? "NET Radio" : (app_bt_is_connected() ? "BT" : "Idle");
    lv_label_set_text(s_lbl_source, src);
    lvgl_port_unlock();
}

/* ================= 页面构建 ================= */
void ui_pages_create(int w, int h)
{
    (void)h;
    /* canvas 缓冲从 PSRAM 分配(约144KB): .bss 只留内部 SRAM 给协议栈 */
    s_spec_buf = heap_caps_malloc(sizeof(lv_color16_t) * SPEC_W * SPEC_H,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cube_buf = heap_caps_malloc(sizeof(lv_color16_t) * CUBE_SIZE * CUBE_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_spec_buf == NULL || s_cube_buf == NULL) {
        ESP_LOGE(TAG, "canvas buffer alloc failed");
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A12), 0);   /* AMOLED 深黑 */

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "EspCube");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_source = lv_label_create(scr);
    lv_label_set_text(s_lbl_source, "Idle");
    lv_obj_set_style_text_color(s_lbl_source, lv_color_hex(0x8090A0), 0);
    lv_obj_align(s_lbl_source, LV_ALIGN_TOP_MID, 0, 40);

    s_lbl_meta = lv_label_create(scr);
    lv_label_set_text(s_lbl_meta, "-");
    lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(0x40C8F8), 0);
    lv_obj_set_width(s_lbl_meta, 420);
    lv_label_set_long_mode(s_lbl_meta, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_meta, LV_ALIGN_TOP_MID, 0, 66);

    /* 频谱 canvas */
    s_canvas_spec = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas_spec, s_spec_buf, SPEC_W, SPEC_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas_spec, LV_ALIGN_CENTER, 0, -10);

    /* 立方体 canvas */
    s_canvas_cube = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas_cube, s_cube_buf, CUBE_SIZE, CUBE_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas_cube, LV_ALIGN_TOP_MID, 0, 96);

    /* 音量滑条 */
    s_slider_vol = lv_slider_create(scr);
    lv_slider_set_range(s_slider_vol, 0, 100);
    lv_slider_set_value(s_slider_vol, audio_mixer_get_master_volume(), LV_ANIM_OFF);
    lv_obj_set_width(s_slider_vol, 240);
    lv_obj_align(s_slider_vol, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_add_event_cb(s_slider_vol, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 控制按钮行 */
    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_bg_color(&btn_style, lv_color_hex(0x182030));
    lv_style_set_radius(&btn_style, 12);

    s_btn_play = lv_btn_create(scr);
    lv_obj_add_style(s_btn_play, &btn_style, 0);
    lv_obj_set_size(s_btn_play, 84, 60);
    lv_obj_align(s_btn_play, LV_ALIGN_BOTTOM_MID, 0, -130);
    lv_obj_add_event_cb(s_btn_play, play_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_play = lv_label_create(s_btn_play);
    lv_obj_set_style_text_font(s_lbl_play, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
    lv_obj_center(s_lbl_play);

    lv_obj_t *btn_prev = lv_btn_create(scr);
    lv_obj_add_style(btn_prev, &btn_style, 0);
    lv_obj_set_size(btn_prev, 84, 60);
    lv_obj_align_to(btn_prev, s_btn_play, LV_ALIGN_OUT_LEFT_MID, -16, 0);
    lv_obj_add_event_cb(btn_prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lp = lv_label_create(btn_prev);
    lv_label_set_text(lp, LV_SYMBOL_PREV);
    lv_obj_center(lp);

    lv_obj_t *btn_next = lv_btn_create(scr);
    lv_obj_add_style(btn_next, &btn_style, 0);
    lv_obj_set_size(btn_next, 84, 60);
    lv_obj_align_to(btn_next, s_btn_play, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_add_event_cb(btn_next, next_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ln = lv_label_create(btn_next);
    lv_label_set_text(ln, LV_SYMBOL_NEXT);
    lv_obj_center(ln);

    /* 30fps 动画定时器 */
    lv_timer_create(anim_timer_cb, 33, NULL);

    /* 订阅跨模块事件 */
    cube_event_subscribe(on_stream_title, NULL);
    cube_event_subscribe(on_bt_meta, NULL);
    cube_event_subscribe(on_player_state, NULL);
}
