/*
 * ui_internal.h —— 虚拟角色 UI 内部共享头(平台无关; 仅 ui/ 子树各 .c 使用)
 *
 * M4 重设计(2026-11-12 用户定案):
 *   主界面 = 常驻全屏动画表情(EFC) —— 设备状态的唯一表达中枢;
 *   下滑 = 状态栏面板(时间/供电/WiFi/音量条);
 *   上滑 = AI 角色状态面板(心情/驱力/字幕/待办);
 *   音频可视化(频谱直线/歌词飞行/音量 HUD)全部移除;
 *   配置页走 app/web(设备端不做)。
 *
 * ui_pages.c(场景/主循环/启动页) + ui_widget.c(表情/面板/手势)
 * 经本头共享; 对外只暴露 ui_pages_create()(ui_pages.h)。
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
#define C_AI        0x7FD4A8   /* AI 状态提示 */
#define C_TXT       0xE9EFF7
#define C_TXT2      0x8B98AC
#define C_DIM       0x3A4558
#define C_PANEL_BG  0x111826   /* 面板底色 */

#define FONT_CJK   (&ui_font_cjk18)
#define FONT_CJK24 (&ui_font_cjk24)
#define FONT_M14   (&lv_font_montserrat_14)
#define FONT_M28   (&lv_font_montserrat_28)

/* ================= 几何/时序 ================= */
#define SPLASH_CUBE 150
#define SPLASH_MS   2400
#define PANEL_W     420              /* 面板宽(圆屏内接安全区) */
#define PANEL_H_STATUS 232           /* 状态栏面板高 */
#define PANEL_H_SOUL   264           /* AI 状态面板高 */
#define PANEL_HIDE_MS  180           /* 面板滑入/滑出时长(跟手性调优) */
#define PANEL_AUTO_MS  8000          /* 面板无操作自动收回 */

/* 手势阈值: 轴向判定 14px / 面板触发 46px / 切曲 60px */
#define GEST_AXIS_PX   14
#define GEST_PANEL_PX  30
#define GEST_SW_PX     60

/* ================= 页面级(ui_pages.c 持有) ================= */
typedef struct {
    lv_obj_t *scr_main;
    int screen_w, screen_h;
    bool splash_active;
    uint32_t tick_cnt;
} ui_page_t;
extern ui_page_t ui_pg;

/* ================= 表情/面板(ui_widget.c 持有) ================= */
typedef struct {
    /* 表情层 */
    lv_obj_t *face_bg;                    /* 情绪底色圆(EFC 背后) */
    lv_obj_t *face_efc;                   /* EFC 控件(EFC=n 时不存在) */
    int face_state;                       /* 最近 ai_state(驱动帧速/呼吸) */
    char mood_tag_last[16];
    /* 状态栏(下滑) */
    lv_obj_t *p_status;                   /* 容器(y 动画进出) */
    lv_obj_t *clock, *dateline, *net, *power;
    lv_obj_t *vol_track, *vol_fill;       /* 自绘音量条(track+fill) */
    lv_obj_t *vol_label;
    int vol_w;                            /* 音量条可拖宽度缓存 */
    /* AI 状态(上滑) */
    lv_obj_t *p_soul;
    lv_obj_t *mood_word, *mood_tag_lbl;
    lv_obj_t *bar_v, *bar_a, *bar_b;      /* 心情/活力/联结 fill */
    lv_obj_t *sub, *badge;
    /* 面板状态 */
    int8_t panel;                         /* 0=都收起 1=状态栏 2=AI 状态 */
    lv_timer_t *panel_timer;
    /* 快照缓存(变更检测) */
    int last_min, last_day;
    char last_sub[UI_SUB_LEN];
    char last_badge[UI_STR_LEN];
    char last_net[UI_STR_LEN];
    int last_volume;
    int last_ai_state;
    /* 手势(轴向锁定) */
    lv_point_t p0;
    int vol0;                             /* 拖动音量条时的起始值 */
    int8_t axis;                          /* 0=未定 1=竖(面板) 2=横(切曲) */
    bool panel_acted;                     /* 本手势已触发过面板动作(防连发) */
    bool on_volbar;                       /* 按下点在音量条上 */
    uint32_t rng;                         /* xorshift32 状态 */
} ui_widget_t;
extern ui_widget_t ui_w;

/* ================= 跨文件函数(ui_widget.c) ================= */
void ui_anim_y_cb(void *var, int32_t v);
uint32_t ui_rand(void);
lv_obj_t *ui_label_make(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
void ui_widget_create(lv_obj_t *scr);
void ui_widget_gesture_register(lv_obj_t *scr);
/* 每帧同步(model 快照 → 表情/面板内容; 值变化才碰 LVGL) */
void ui_face_apply(const ui_model_t *m);
void ui_status_apply(const ui_model_t *m);
void ui_soul_apply(const ui_model_t *m);
/* 面板控制(手势/自动收回) */
void ui_panel_show(int which);            /* 0=都收起 1=状态栏 2=AI 状态 */
