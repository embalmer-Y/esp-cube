/*
 * ui_pages.c —— 虚拟角色 UI: 主循环/启动页/构建入口
 *
 * M4 重设计(2026-11-12 用户定案): 无多场景机器 —— 全屏动画表情常驻,
 * 下滑状态栏 / 上滑 AI 状态(见 ui_widget.c); 音频可视化(频谱直线/
 * 歌词飞行)已退役。本文件只做: splash → 主屏 → 30fps 快照同步。
 * 平台无关: 固件/模拟器同源编译。
 */
#include "ui_pages.h"
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ui_page_t ui_pg;

/* ================= 启动页(仅本文件使用) ================= */
static lv_obj_t *s_canvas_splash_cube, *s_splash_title, *s_splash_sub;
static lv_obj_t *s_splash_dots[3];
static lv_color16_t *s_buf_splash;          /* SPLASH_CUBE^2 */

static void cube_draw(lv_color16_t *buf, int size, float ax, float ay,
                      float scale, uint8_t r, uint8_t g, uint8_t b);

/* 快照轮询(30fps) + 变更检测 */
static void model_sync(const ui_model_t *m)
{
    ui_face_apply(m);       /* 表情: 情绪底色 + 会话状态帧速 */
    ui_status_apply(m);     /* 状态栏: 时钟/网络/音量条 */
    ui_soul_apply(m);       /* AI 状态: 心情/三条/字幕/待办 */
}

/* ============================================================ */
/*                     主定时器(30fps)                           */
/* ============================================================ */
static void master_timer_cb(lv_timer_t *t)
{
    (void)t;
    const ui_model_t *m = ui_model();
    ui_pg.tick_cnt++;

    if (ui_pg.splash_active) {
        uint32_t el = ui_pg.tick_cnt * 33;
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
            ui_pg.splash_active = false;
            lv_screen_load_anim(ui_pg.scr_main, LV_SCR_LOAD_ANIM_FADE_IN,
                                350, 0, true);
        }
        return;
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

static lv_obj_t *splash_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    s_buf_splash = malloc(sizeof(lv_color16_t) * SPLASH_CUBE * SPLASH_CUBE);
    if (s_buf_splash != NULL) {
        s_canvas_splash_cube = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_canvas_splash_cube, s_buf_splash, SPLASH_CUBE,
                             SPLASH_CUBE, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_canvas_splash_cube, LV_ALIGN_CENTER, 0, -34);
    }

    s_splash_title = ui_label_make(scr, FONT_M28, C_TXT);
    lv_label_set_text(s_splash_title, "EspCube");
    lv_obj_set_style_text_opa(s_splash_title, 0, 0);
    lv_obj_align(s_splash_title, LV_ALIGN_CENTER, 0, 66);

    s_splash_sub = ui_label_make(scr, FONT_M14, C_TXT2);
    lv_label_set_text(s_splash_sub, "AI VOICE COMPANION");
    lv_obj_set_style_text_opa(s_splash_sub, 0, 0);
    lv_obj_align(s_splash_sub, LV_ALIGN_CENTER, 0, 100);

    for (int i = 0; i < 3; i++) {
        s_splash_dots[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_splash_dots[i]);
        lv_obj_set_size(s_splash_dots[i], 8, 8);
        lv_obj_set_style_radius(s_splash_dots[i], 4, 0);
        lv_obj_set_style_bg_opa(s_splash_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_splash_dots[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_align_to(s_splash_dots[i], s_splash_title,
                        LV_ALIGN_OUT_BOTTOM_MID, (i - 1) * 18, 24);
    }
    lv_obj_add_event_cb(scr, splash_free_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ============================================================ */
/*                     构建入口                                  */
/* ============================================================ */
void ui_pages_create(int w, int h)
{
    ui_pg.splash_active = true;
    ui_pg.screen_w = w > 0 ? w : 460;
    ui_pg.screen_h = h > 0 ? h : 460;
    memset(&ui_w, 0, sizeof(ui_w));        /* 面板收起态/缓存全零 */
    ui_w.rng = lv_tick_get() | 1u;          /* |1 保证非零种子 */

    /* ---- 主屏 ---- */
    ui_pg.scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_pg.scr_main, lv_color_hex(C_BG), 0);

    ui_widget_create(ui_pg.scr_main);       /* 表情层 + 双面板 */
    ui_widget_gesture_register(ui_pg.scr_main);

    /* ---- 启动页 ---- */
    lv_obj_t *scr_splash = splash_create();
    lv_screen_load(scr_splash);

    lv_timer_create(master_timer_cb, 33, NULL);
}
