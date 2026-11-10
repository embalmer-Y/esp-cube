/*
 * ui_pages.c —— 极简氛围 UI: 场景切换/主循环/启动页/构建入口
 *
 * 设计原则(2026-11-11 定稿): 屏幕小(2寸 460×460), 不做传统控件界面 ——
 *   整屏就是一个"正在播放"的动画舞台:
 *   · 电台/纯音乐: 一条随音频跳动的发光直线(频谱动画, 全屏主角)
 *   · BT 音乐有歌词: 歌词动画(当前行飞入/下一行预示), 直线让位
 *   · 待机: 直线呼吸 + 微弱品牌标
 *   · 上下滑动 = 调音量(实时 HUD), 左右滑动 = 切曲目/电台(带 toast)
 *
 * 本文件只做场景编排; 渲染在 ui_render.c, 歌词在 ui_lyrics.c,
 * 部件/手势在 ui_widget.c(共享状态见 ui_internal.h)。
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

/* ============================================================ */
/*                     场景切换                                  */
/* ============================================================ */
static void scene_apply(int scene)
{
    if (scene == ui_pg.last_scene) return;
    int prev = ui_pg.last_scene;
    ui_pg.last_scene = scene;
    ui_pg.scene = scene;

    if (prev == -1) return;                 /* 启动页后首帧不做退场动画 */
    if (scene == SCENE_IDLE) {
        ui_fade_in(ui_w.brand, 6, 300);
        if (prev == SCENE_LYRICS) ui_lyric_fly_out();   /* 尾句飞离 */
        else lv_obj_add_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN);
        ui_fade_out(ui_lyr.lbl_next, 260);
        ui_fade_out(ui_w.title, 260);
        ui_fade_out(ui_w.artist, 260);
        if (prev == SCENE_SPECTRUM || prev == SCENE_LYRICS) {
            ui_fade_out(ui_w.station, 400);
        }
    } else {
        ui_fade_out(ui_w.brand, 200);
        if (scene == SCENE_LYRICS) {
            /* 歌词是唯一主角: 直线让位(每帧不再画), 清掉残留帧 */
            if (ui_gfx.buf != NULL) {
                memset(ui_gfx.buf, 0, sizeof(uint32_t) * LINE_W * LINE_H);
                lv_obj_invalidate(ui_gfx.canvas);
            }
            lv_obj_set_style_opa(ui_lyr.lbl_next, 255, 0);
        } else {
            if (prev == SCENE_LYRICS) ui_lyric_fly_out();  /* 切纯音乐: 句尾飞离 */
            else lv_obj_add_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN);
            ui_fade_out(ui_lyr.lbl_next, 260);
        }
    }
}

/* 快照轮询(30fps) + 变更检测 */
static void model_sync(const ui_model_t *m)
{
    bool playing = m->player == UI_PLAYER_PLAYING ||
                   (m->source == UI_SRC_BT && m->bt_connected);
    int scene = !playing ? SCENE_IDLE
              : (m->source == UI_SRC_BT && m->has_lyrics) ? SCENE_LYRICS
              : SCENE_SPECTRUM;
    scene_apply(scene);
    ui_info_apply(m);
    ui_lyrics_apply(m);
    ui_ai_apply(m);
    ui_volume_sync(m->volume);
}

/* ============================================================ */
/*                     主定时器(30fps)                           */
/* ============================================================ */
static void master_timer_cb(lv_timer_t *t)
{
    (void)t;
    const ui_model_t *m = ui_model();
    ui_pg.tick_cnt++;

    bool playing = m->player == UI_PLAYER_PLAYING ||
                   (m->source == UI_SRC_BT && m->bt_connected);
    ui_bands_update(playing);

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

    if (ui_pg.scene != SCENE_LYRICS) {
        ui_line_draw(m);                    /* 歌词场景: 直线让位 */
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
    lv_label_set_text(s_splash_sub, "AI CUBE SPEAKER");
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
    /* 状态初值(等价原 static 声明处的初始化器; 全局零 init 不够) */
    ui_pg.splash_active = true;
    ui_pg.last_scene = -1;
    ui_pg.screen_w = w > 0 ? w : 460;
    ui_pg.screen_h = h > 0 ? h : 460;
    ui_gfx.prev_y0 = LINE_H;
    ui_gfx.prev_y1 = -1;
    memset(ui_w.last_title, 0, sizeof(ui_w.last_title));
    ui_w.last_title[0] = 0x01;          /* 非空哨兵: 首次同步必触发 */
    ui_w.last_artist[0] = 0x01;
    ui_lyr.last[0] = 0x01;
    ui_lyr.last_next[0] = 0x01;
    ui_w.last_volume = -1;
    ui_w.last_station = -1;
    ui_w.last_station_num = -1;
    ui_w.last_source = -1;
    ui_w.last_player = -1;
    ui_w.last_ai_state = -1;
    ui_w.rng = lv_tick_get() | 1u;      /* |1 保证非零种子 */
    ui_render_tables_init();            /* 倒数表/光盘 sprite/频带边界 */

    /* ---- 主屏 ---- */
    ui_pg.scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_pg.scr_main, lv_color_hex(C_BG), 0);

    ui_render_create(ui_pg.scr_main);      /* 音频直线画布(ARGB) */
    ui_widget_create(ui_pg.scr_main);      /* 品牌标/toast/AI/标题/HUD */
    ui_lyrics_create(w);                   /* 歌词层(layer_top) */
    ui_widget_gesture_register(ui_pg.scr_main);

    /* ---- 启动页 ---- */
    lv_obj_t *scr_splash = splash_create();
    lv_screen_load(scr_splash);

    lv_timer_create(master_timer_cb, 33, NULL);
}
