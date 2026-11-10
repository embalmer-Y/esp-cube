/*
 * ui_internal.h —— 氛围 UI 内部共享头(平台无关; 仅 ui/ 子树各 .c 使用)
 *
 * ui_pages.c(场景/主循环/splash) + ui_render.c(直线光栅) +
 * ui_lyrics.c(歌词飞行) + ui_widget.c(HUD/toast/手势/信息) 四文件
 * 经本头共享: 主题常量、归组状态结构(原 53 个散落 static)、函数原型。
 * 对外只暴露 ui_pages_create()(ui_pages.h)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_model.h"
#include "ui_font_cjk.h"

/* ================= 主题 ================= */
#define C_BG        0x0A0A12
#define C_ACCENT    0x40C8F8   /* 电台青 */
#define C_ACCENT_BT 0x8F7BFF   /* 蓝牙紫 */
#define C_AI        0x7FD4A8   /* AI 状态提示 */
#define C_TXT       0xE9EFF7
#define C_TXT2      0x8B98AC
#define C_DIM       0x3A4558

#define FONT_CJK   (&ui_font_cjk18)
#define FONT_CJK24 (&ui_font_cjk24)
#define FONT_M14   (&lv_font_montserrat_14)
#define FONT_M28   (&lv_font_montserrat_28)

/* ================= 几何/时序 ================= */
#define LINE_W      460         /* 直线画布宽 */
#define LINE_H      220         /* 直线画布高(屏幕 y=160..380) */
#define LINE_CY     100         /* 直线中心(画布内) */
#define LYR_W       430         /* 歌词排版宽 */
#define LYR_SNAP_BYTES  (96u * 1024u)   /* 快照池槽: 430 宽 ARGB+stride 余量 */
#define SPLASH_CUBE 150
#define SPLASH_MS   2400

#define SPEC_BANDS  40          /* 频谱带数 */
#define VOL_HUD_MS  700         /* 音量 HUD 停留 */
#define TOAST_MS    1600        /* 换台 toast 停留 */
#define LYR_FLY_IN_MS    480
#define LYR_FLY_OUT_MS   420
#define LYR_FLY_DIST     330                 /* 出发/终点半径 */

/* 手势阈值: 轴向判定 14px / 切曲触发 60px / 音量全程≈222px(45/100) */
#define GEST_AXIS_PX   14
#define GEST_SW_PX     60
#define GEST_VOL_PER   45

/* ================= 场景(派生) ================= */
enum { SCENE_IDLE = 0, SCENE_SPECTRUM, SCENE_LYRICS };

/* ---- 页面级(ui_pages.c 持有) ---- */
typedef struct {
    lv_obj_t *scr_main;
    int scene;
    int last_scene;
    int screen_w, screen_h;
    bool splash_active;
    uint32_t tick_cnt;
} ui_page_t;
extern ui_page_t ui_pg;

/* ---- 渲染状态(ui_render.c 持有) ---- */
typedef struct {
    lv_obj_t *canvas;                     /* ARGB 画布对象 */
    uint32_t *buf;                        /* LINE_W*LINE_H ARGB */
    int16_t pcm[512];                     /* 立体声帧缓冲 */
    float fft[128];
    float band[SPEC_BANDS];               /* 平滑后 band 高度 0..1 */
    int16_t band_lo[SPEC_BANDS], band_hi[SPEC_BANDS];   /* 预计算边界 */
    float energy, bass;
    float curve[LINE_W];                  /* 当前帧曲线 y(画布内) */
    int prev_y0, prev_y1;                 /* 脏带(上帧) */
} ui_gfx_t;
extern ui_gfx_t ui_gfx;

/* ---- 歌词状态(ui_lyrics.c 持有) ---- */
typedef struct {
    lv_obj_t *lbl_src;                    /* 排版源(离屏, 只供 snapshot) */
    lv_obj_t *lbl_next;                   /* 下一行预示(固定水平) */
    lv_obj_t *img_cur, *img_out;          /* 当前句/旧句(快照 image) */
    int angle;                            /* 当前句倾角(0.1° 单位) */
    float dir_x, dir_y;                   /* 飞行方向(单位向量) */
    int rest_x, rest_y;                   /* 中心静止位(label 左上角) */
    /* 快照池: 双槽交替(飞出句沿用当前槽, 新句写另一槽) */
    lv_draw_buf_t snap_dsc[2];
    uint8_t *snap_buf[2];
    int snap_slot;
    char last[UI_SUB_LEN], last_next[UI_SUB_LEN];   /* 变更检测 */
} ui_lyr_t;
extern ui_lyr_t ui_lyr;

/* ---- 部件/手势/快照缓存(ui_widget.c 持有) ---- */
typedef struct {
    lv_obj_t *brand, *station, *ai, *title, *artist;
    lv_obj_t *hud, *hud_fill, *hud_num;
    lv_timer_t *hud_timer, *toast_timer;
    /* 快照副本(变更检测) */
    char last_title[UI_STR_LEN];
    char last_artist[UI_STR_LEN];
    int last_volume, last_station, last_station_num;
    int last_source, last_player, last_ai_state;
    /* 手势(轴向锁定) */
    lv_point_t p0;
    int vol0;
    int8_t axis;                          /* 0=未定 1=上下(音量) 2=左右(切曲) */
    uint32_t rng;                         /* xorshift32 状态 */
} ui_widget_t;
extern ui_widget_t ui_w;

/* ================= 跨文件函数 ================= */
/* ui_widget.c */
void ui_anim_x_cb(void *var, int32_t v);   /* 歌词飞行复用 */
void ui_anim_y_cb(void *var, int32_t v);
uint32_t ui_rand(void);
lv_obj_t *ui_label_make(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
void ui_fade_in(lv_obj_t *obj, int32_t y_from, uint32_t dur);
void ui_fade_out(lv_obj_t *obj, uint32_t dur);
void ui_widget_create(lv_obj_t *scr);
void ui_widget_gesture_register(lv_obj_t *scr);
void ui_volume_sync(int vol);
void ui_toast_station(const ui_model_t *m);
void ui_info_apply(const ui_model_t *m);
void ui_ai_apply(const ui_model_t *m);

/* ui_render.c */
void ui_render_tables_init(void);
void ui_render_create(lv_obj_t *scr);
void ui_bands_update(bool playing);
void ui_line_draw(const ui_model_t *m);

/* ui_lyrics.c */
void ui_lyrics_create(int w);
void ui_lyric_fly_out(void);
void ui_lyrics_apply(const ui_model_t *m);
