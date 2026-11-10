/*
 * ui_widget.c —— 部件层: 标签/HUD/toast/手势/动画工具/信息同步
 *
 * 从 ui_pages.c 拆出(docs/06 §5 目录布局); 纯搬移, 行为不变。
 */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

ui_widget_t ui_w;

enum { AXIS_NONE = 0, AXIS_VOL, AXIS_SWITCH };

/* ============================================================ */
/*                        小工具                                 */
/* ============================================================ */
/* xorshift32: 平台无关伪随机(libc rand 无种子每次开机同序列;
 * 也不引 esp_random 破坏 UI 代码的平台无关性), lv_tick 播种 */
uint32_t ui_rand(void)
{
    uint32_t x = ui_w.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return ui_w.rng = x;
}

lv_obj_t *ui_label_make(lv_obj_t *parent, const lv_font_t *font,
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

void ui_anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

void ui_anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

/* 渐入(透明度+上滑): dur ms, ease_out */
void ui_fade_in(lv_obj_t *obj, int32_t y_from, uint32_t dur)
{
    int32_t y_rest = lv_obj_get_y(obj);
    lv_anim_del(obj, anim_opa_cb);
    lv_anim_del(obj, ui_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, ui_anim_y_cb);
    lv_anim_set_values(&a, y_rest + y_from, y_rest);
    lv_anim_start(&a);
}

/* 渐隐: dur ms, ease_in */
void ui_fade_out(lv_obj_t *obj, uint32_t dur)
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
/*                     音量 HUD                                  */
/* ============================================================ */
static void hud_hide_cb(lv_timer_t *t)
{
    (void)t;
    if ((int)lv_obj_get_style_opa(ui_w.hud, 0) == 0) return;
    ui_fade_out(ui_w.hud, 450);
}

static void hud_show(void)
{
    int vol = ui_model()->volume;
    lv_obj_set_style_opa(ui_w.hud, LV_OPA_COVER, 0);
    lv_obj_set_height(ui_w.hud_fill, 150 * vol / 100);
    lv_label_set_text_fmt(ui_w.hud_num, "%d", vol);
    if (ui_w.hud_timer != NULL) lv_timer_reset(ui_w.hud_timer);
}

void ui_volume_sync(int vol)
{
    if (vol == ui_w.last_volume) return;
    bool first = ui_w.last_volume == -1;
    ui_w.last_volume = vol;
    if (!first) hud_show();                      /* 首次同步不弹 HUD */
}

/* ============================================================ */
/*                     电台 toast                                */
/* ============================================================ */
void ui_toast_station(const ui_model_t *m)
{
    int n = m->station_num > 0 ? m->station_num : 1;
    lv_label_set_text(ui_w.station, m->station_names[m->station_idx % n]);
    lv_obj_set_style_opa(ui_w.station, LV_OPA_COVER, 0);
    ui_fade_in(ui_w.station, 8, 260);
    if (ui_w.toast_timer != NULL) lv_timer_reset(ui_w.toast_timer);
}

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if ((int)lv_obj_get_style_opa(ui_w.station, 0) == 0) return;
    ui_fade_out(ui_w.station, 400);
}

/* ============================================================ */
/*                     手势(轴向锁定)                            */
/* ============================================================ */
static void gesture_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_indev_get_point(in, &ui_w.p0);
    ui_w.axis = AXIS_NONE;
    ui_w.vol0 = ui_model()->volume;
}

static void gesture_pressing_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int dx = p.x - ui_w.p0.x;
    int dy = p.y - ui_w.p0.y;

    if (ui_w.axis == AXIS_NONE) {
        if (abs(dx) > GEST_AXIS_PX && abs(dx) > abs(dy)) ui_w.axis = AXIS_SWITCH;
        else if (abs(dy) > GEST_AXIS_PX) ui_w.axis = AXIS_VOL;
        else return;
    }
    if (ui_w.axis == AXIS_VOL) {
        ui_ctl_set_volume(ui_w.vol0 - dy * GEST_VOL_PER / 100);
    }
}

static void gesture_release_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = ui_w.p0;
    if (in != NULL) lv_indev_get_point(in, &p);
    int dx = p.x - ui_w.p0.x;
    if (ui_w.axis == AXIS_SWITCH && abs(dx) > GEST_SW_PX) {
        ui_ctl_switch_station(dx < 0 ? 1 : -1);          /* 左滑=下一曲 */
    }
    ui_w.axis = AXIS_NONE;
}

void ui_widget_gesture_register(lv_obj_t *scr)
{
    lv_obj_add_event_cb(scr, gesture_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, gesture_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, gesture_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, gesture_release_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* ============================================================ */
/*                     信息/AI 状态同步                          */
/* ============================================================ */
void ui_info_apply(const ui_model_t *m)
{
    /* 底部标题(播放态) */
    const char *ti = m->title;
    if (strcmp(ti, ui_w.last_title) != 0) {
        strncpy(ui_w.last_title, ti, sizeof(ui_w.last_title) - 1);
        ui_w.last_title[sizeof(ui_w.last_title) - 1] = 0;
        lv_label_set_text(ui_w.title, ti[0] ? ti : " ");
    }
    /* 底部第二行: BT=艺术家·专辑; 电台=空(台名走 toast) */
    char line[UI_STR_LEN * 2 + 8];    /* 容纳 artist+"·"+album 全长不截断 */
    if (m->source == UI_SRC_BT) {
        snprintf(line, sizeof(line), "%s%s%s", m->artist,
                 m->album[0] ? " · " : "", m->album);
    } else {
        line[0] = 0;
    }
    if (strcmp(line, ui_w.last_artist) != 0) {
        strncpy(ui_w.last_artist, line, sizeof(ui_w.last_artist) - 1);
        ui_w.last_artist[sizeof(ui_w.last_artist) - 1] = 0;
        lv_label_set_text(ui_w.artist, line[0] ? line : " ");
    }

    /* 音源切换 → 电台顶部 toast 常驻态/BT 隐藏 */
    if (m->source != ui_w.last_source) {
        ui_w.last_source = m->source;
        ui_w.last_station = -1;
        if (m->source == UI_SRC_RADIO) ui_toast_station(m);
        else if (m->source == UI_SRC_BT) ui_fade_out(ui_w.station, 200);
    }
    /* 电台换台 toast */
    if (m->source == UI_SRC_RADIO &&
        (m->station_idx != ui_w.last_station ||
         m->station_num != ui_w.last_station_num)) {
        ui_w.last_station = m->station_idx;
        ui_w.last_station_num = m->station_num;
        if (ui_w.last_station_num > 0) ui_toast_station(m);
    }
    if (m->player != ui_w.last_player) ui_w.last_player = m->player;
}

/* AI 会话状态提示(极简: 顶部第二行点标文字) */
void ui_ai_apply(const ui_model_t *m)
{
    if ((int)m->ai_state == ui_w.last_ai_state) return;
    ui_w.last_ai_state = (int)m->ai_state;
    switch (m->ai_state) {
    case UI_AI_LISTENING:
        lv_label_set_text(ui_w.ai, "· 聆听中 ·");
        ui_fade_in(ui_w.ai, 8, 260);
        break;
    case UI_AI_THINKING:
        lv_label_set_text(ui_w.ai, "· 思考中 ·");
        break;                              /* 已亮, 仅换字 */
    case UI_AI_SPEAKING:
        lv_label_set_text(ui_w.ai, "· 回答中 ·");
        break;
    default:
        ui_fade_out(ui_w.ai, 300);
        break;
    }
}

/* ============================================================ */
/*                     部件构建                                  */
/* ============================================================ */
void ui_widget_create(lv_obj_t *scr)
{
    /* 待机品牌标 */
    ui_w.brand = ui_label_make(scr, FONT_M14, C_DIM);
    lv_label_set_text(ui_w.brand, "EspCube");
    lv_obj_set_style_opa(ui_w.brand, 0, 0);
    lv_obj_align(ui_w.brand, LV_ALIGN_TOP_MID, 0, 30);

    /* 电台 toast(顶部) */
    ui_w.station = ui_label_make(scr, FONT_CJK, C_TXT2);
    lv_obj_set_style_opa(ui_w.station, 0, 0);
    lv_obj_align(ui_w.station, LV_ALIGN_TOP_MID, 0, 26);

    /* AI 会话状态提示(顶部第二行, 点标文字; 见 ui_ai_apply) */
    ui_w.ai = ui_label_make(scr, FONT_CJK, C_AI);
    lv_obj_set_style_opa(ui_w.ai, 0, 0);
    lv_obj_align(ui_w.ai, LV_ALIGN_TOP_MID, 0, 56);

    /* 底部标题/艺术家 */
    ui_w.title = ui_label_make(scr, FONT_CJK24, C_TXT);
    lv_obj_set_width(ui_w.title, 430);
    lv_label_set_long_mode(ui_w.title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_opa(ui_w.title, 0, 0);
    lv_obj_align(ui_w.title, LV_ALIGN_BOTTOM_MID, 0, -58);
    ui_w.artist = ui_label_make(scr, FONT_CJK, C_TXT2);
    lv_obj_set_width(ui_w.artist, 420);
    lv_label_set_long_mode(ui_w.artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ui_w.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(ui_w.artist, 0, 0);
    lv_obj_align(ui_w.artist, LV_ALIGN_BOTTOM_MID, 0, -28);

    /* 音量 HUD(右缘竖条+数字) */
    ui_w.hud = lv_obj_create(scr);
    lv_obj_remove_style_all(ui_w.hud);
    lv_obj_set_size(ui_w.hud, 64, 180);
    lv_obj_align(ui_w.hud, LV_ALIGN_RIGHT_MID, -12, 36);   /* 下移避让直线 */
    lv_obj_set_style_opa(ui_w.hud, 0, 0);
    lv_obj_t *track = lv_obj_create(ui_w.hud);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 5, 150);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
    ui_w.hud_fill = lv_obj_create(track);
    lv_obj_remove_style_all(ui_w.hud_fill);
    lv_obj_set_width(ui_w.hud_fill, 5);
    lv_obj_set_style_bg_color(ui_w.hud_fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(ui_w.hud_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui_w.hud_fill, 3, 0);
    lv_obj_align(ui_w.hud_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_w.hud_num = ui_label_make(ui_w.hud, FONT_M28, C_TXT);
    lv_obj_align(ui_w.hud_num, LV_ALIGN_LEFT_MID, 0, 0);

    /* 常驻运行 + reset 续期; cb 内已有 opa==0 短路, 空转无害 */
    ui_w.hud_timer = lv_timer_create(hud_hide_cb, VOL_HUD_MS, NULL);
    ui_w.toast_timer = lv_timer_create(toast_hide_cb, TOAST_MS, NULL);
    if (ui_w.hud_timer == NULL || ui_w.toast_timer == NULL) {
        /* 无定时器则 HUD/toast 常驻(功能降级不崩); 引用处判空 */
    }
}
