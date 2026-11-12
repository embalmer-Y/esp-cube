/*
 * ui_widget.c —— 虚拟角色 UI 部件层: 全屏表情/状态栏/AI 状态/手势
 *
 * M4 重设计(2026-11-12):
 *   - 表情 = 设备状态唯一表达: EFC 常驻全屏, 帧速随会话状态
 *     (聆听常速/思考慢速/播报快速), 情绪底色随 soul mood_tag;
 *   - 下滑状态栏: 时间/日期/供电/WiFi/音量条(自绘 track+fill,
 *     免依赖 lv_slider 配置差异; 可拖动);
 *   - 上滑 AI 状态: 心情词 + 心情/活力/联结三条 + 对话字幕 + 待办;
 *   - 手势: 上下滑开合面板, 左右滑切台, 单击收起面板。
 */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#include "esp_log.h"
#endif
#if CONFIG_ESP_CUBE_ENABLE_EFC
#include "lv_efc.h"
extern const uint8_t giphy_z_efc_start[] asm("_binary_giphy_z_efc_start");
extern const uint8_t giphy_z_efc_end[]   asm("_binary_giphy_z_efc_end");
#endif

ui_widget_t ui_w;

enum { AXIS_NONE = 0, AXIS_V, AXIS_SWITCH };

/* ============================================================ */
/*                        小工具                                 */
/* ============================================================ */
uint32_t ui_rand(void)
{
    uint32_t x = ui_w.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return ui_w.rng = x;
}

void ui_anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

lv_obj_t *ui_label_make(lv_obj_t *parent, const lv_font_t *font,
                        uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* y 滑动(面板进出场, ease_out) */
static void panel_slide(lv_obj_t *obj, int32_t y_to, uint32_t dur)
{
    lv_anim_del(obj, ui_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, ui_anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(obj), y_to);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 呼吸透明度(聆听态底色; 单实例重启动画) */
static void breath_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
static void breath_start(lv_obj_t *obj)
{
#if CONFIG_ESP_CUBE_ENABLE_EFC
    lv_anim_del(obj, breath_opa_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, breath_opa_cb);
    lv_anim_set_values(&a, 24, 72);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
#endif
}
static void breath_stop(lv_obj_t *obj, int opa)
{
#if CONFIG_ESP_CUBE_ENABLE_EFC
    lv_anim_del(obj, breath_opa_cb);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)opa, 0);
#endif
}

/* ============================================================ */
/*                     面板控制                                   */
/* ============================================================ */
static void panel_auto_hide_cb(lv_timer_t *t)
{
    (void)t;
    ui_panel_show(0);
}

void ui_panel_show(int which)
{
    if (ui_w.panel == which) return;
    ui_w.panel = (int8_t)which;
    int32_t scr_h = (int32_t)lv_display_get_vertical_resolution(
        lv_display_get_default());
    /* 状态栏: 收起藏屏上方外, 展开贴顶; AI 状态: 藏屏下方外, 展开贴底 */
    panel_slide(ui_w.p_status,
                which == 1 ? 6 : -PANEL_H_STATUS - 12, PANEL_HIDE_MS);
    panel_slide(ui_w.p_soul,
                which == 2 ? scr_h - PANEL_H_SOUL - 6 : scr_h + 12,
                PANEL_HIDE_MS);
    if (which != 0) {
        if (ui_w.panel_timer == NULL) {
            ui_w.panel_timer = lv_timer_create(panel_auto_hide_cb,
                                               PANEL_AUTO_MS, NULL);
        }
        lv_timer_reset(ui_w.panel_timer);
    }
}

/* ============================================================ */
/*                     每帧同步                                   */
/* ============================================================ */
static uint32_t mood_color(const char *tag)
{
    static const struct { const char *tag; uint32_t c; } m[] = {
        { "happy",     0xFFD166 }, { "relaxed",   0x6BE3A2 },
        { "angry",     0xFF6B6B }, { "sad",       0x7FA8D9 },
        { "surprised", 0xFFA6F9 }, { "confused",  0xBFA4F9 },
        { "sleepy",    0x8B98AC },
    };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (strcmp(tag, m[i].tag) == 0) return m[i].c;
    }
    return 0x2A3550;                     /* 无灵魂数据: 暗蓝底 */
}

static const char *mood_cn(const char *tag)
{
    static const struct { const char *en, *cn; } m[] = {
        { "happy", "开心" }, { "relaxed", "放松" }, { "angry", "烦躁" },
        { "sad", "低落" }, { "surprised", "惊讶" }, { "confused", "困惑" },
        { "sleepy", "困倦" },
    };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (strcmp(tag, m[i].en) == 0) return m[i].cn;
    }
    return "平静";
}

/* 表情: 情绪底色 + 会话状态驱动帧速/呼吸(演示级映射, 多段素材就位后
 * 升级为"状态→片段"查表) */
/* 命令消费: panel_req 变化沿触发(无触摸期 CLI/语音开面板) */
static int last_panel_req;
static void panel_req_consume(const ui_model_t *m)
{
    if (m->panel_req == last_panel_req || m->panel_req == 0) return;
    last_panel_req = m->panel_req;
    if (m->panel_req >= 1 && m->panel_req <= 2) {
        ui_panel_show(m->panel_req);
    } else if (m->panel_req == 3) {
        ui_panel_show(0);
    }
}

void ui_face_apply(const ui_model_t *m)
{
#if CONFIG_ESP_CUBE_ENABLE_EFC
    if (strcmp(m->mood_tag, ui_w.mood_tag_last) != 0) {
        snprintf(ui_w.mood_tag_last, sizeof(ui_w.mood_tag_last), "%s",
                 m->mood_tag);
        lv_obj_set_style_bg_color(ui_w.face_bg,
                                  lv_color_hex(mood_color(ui_w.mood_tag_last)), 0);
    }
    if ((int)m->ai_state != ui_w.last_ai_state) {
        int prev = ui_w.last_ai_state;
        ui_w.last_ai_state = (int)m->ai_state;
        switch (m->ai_state) {
        case UI_AI_LISTENING:
            lv_efc_set_frame_delay(ui_w.face_efc, 55);
            breath_start(ui_w.face_bg);
            break;
        case UI_AI_THINKING:
            lv_efc_set_frame_delay(ui_w.face_efc, 130);   /* 慢速=琢磨 */
            breath_stop(ui_w.face_bg, 30);
            break;
        case UI_AI_SPEAKING:
            lv_efc_set_frame_delay(ui_w.face_efc, 38);    /* 快速=活泛 */
            breath_stop(ui_w.face_bg, 56);
            break;
        default:
            lv_efc_set_frame_delay(ui_w.face_efc, 55);
            breath_stop(ui_w.face_bg, 40);
            if (prev == UI_AI_IDLE || prev < 0) break;
            break;
        }
    }
#endif
}

void ui_status_apply(const ui_model_t *m)
{
    panel_req_consume(m);
    /* 时钟(分钟粒度) + 日期(日粒度) */
    if (m->time_synced && m->min != ui_w.last_min) {
        ui_w.last_min = m->min;
        lv_label_set_text_fmt(ui_w.clock, "%02d:%02d", m->hour, m->min);
    }
    static const char *wd[] = { "日","一","二","三","四","五","六" };
    if (m->time_synced && m->day != ui_w.last_day) {
        ui_w.last_day = m->day;
        lv_label_set_text_fmt(ui_w.dateline, "%d月%d日 周%s",
                              m->mon, m->day, wd[m->wday % 7]);
    }
    /* 网络/供电行(不渲染原始 SSID: CJK 子集字体对任意 ASCII 串无字形
     * 保障, ssid 又不受控 —— sim 截图实测 'y' 渲染成乱码, 2026-11-12) */
    char net[UI_STR_LEN];
    snprintf(net, sizeof(net), "%s",
             m->wifi_up ? "无线网络已连接" : "网络已断开");
    if (strcmp(net, ui_w.last_net) != 0) {
        snprintf(ui_w.last_net, sizeof(ui_w.last_net), "%s", net);
        lv_label_set_text(ui_w.net, net);
    }
    /* 音量条(外部变化才重绘; 拖动路径见手势) */
    if (m->volume != ui_w.last_volume) {
        ui_w.last_volume = m->volume;
        lv_obj_set_width(ui_w.vol_fill,
                         ui_w.vol_w * m->volume / 100);
        lv_label_set_text_fmt(ui_w.vol_label, "音量 %d", m->volume);
    }
}

/* AI 状态面板: 心情词/三条/字幕/待办 */
void ui_soul_apply(const ui_model_t *m)
{
    /* 心情词 + 英文 tag */
    static char last_word[16];
    const char *word = m->mood_tag[0] ? mood_cn(m->mood_tag) : "苏醒中";
    if (strcmp(word, last_word) != 0) {
        snprintf(last_word, sizeof(last_word), "%s", word);
        lv_label_set_text(ui_w.mood_word, word);
        lv_label_set_text(ui_w.mood_tag_lbl, m->mood_tag[0] ? m->mood_tag : "");
    }
    /* 字幕 */
    if (strcmp(m->ai_text, ui_w.last_sub) != 0) {
        snprintf(ui_w.last_sub, sizeof(ui_w.last_sub), "%s", m->ai_text);
        lv_label_set_text(ui_w.sub, ui_w.last_sub[0] ? ui_w.last_sub : " ");
    }
    /* 待办条 */
    char bd[UI_STR_LEN];
    if (m->remind_cnt > 0 && m->next_remind_in_min >= 0) {
        snprintf(bd, sizeof(bd), "提醒 %d 分钟后 · 共%d条%s%d条琐事",
                 (int)m->next_remind_in_min, (int)m->remind_cnt,
                 m->memo_cnt ? " · " : "", (int)m->memo_cnt);
    } else if (m->memo_cnt > 0) {
        snprintf(bd, sizeof(bd), "琐事 %d 条", (int)m->memo_cnt);
    } else {
        snprintf(bd, sizeof(bd), "待办清爽 · 有事叫我");
    }
    if (strcmp(bd, ui_w.last_badge) != 0) {
        snprintf(ui_w.last_badge, sizeof(ui_w.last_badge), "%s", bd);
        lv_label_set_text(ui_w.badge, bd);
    }
    /* 三条: 心情(valence -1..1 → 0..1)/活力/联结 —— model 未带数值,
     * 由 tag 粗映射演示(bar 动效待 M4+ model 扩数值后接真值) */
    int v = 50, a = 40, b = 30;
    if (strcmp(m->mood_tag, "happy") == 0) { v = 78; a = 62; }
    else if (strcmp(m->mood_tag, "relaxed") == 0) { v = 64; a = 34; }
    else if (strcmp(m->mood_tag, "sad") == 0) { v = 22; a = 26; }
    else if (strcmp(m->mood_tag, "sleepy") == 0) { v = 45; a = 14; }
    else if (strcmp(m->mood_tag, "angry") == 0) { v = 18; a = 84; }
    else if (strcmp(m->mood_tag, "surprised") == 0) { v = 60; a = 88; }
    lv_obj_set_width(ui_w.bar_v, 260 * v / 100);
    lv_obj_set_width(ui_w.bar_a, 260 * a / 100);
    lv_obj_set_width(ui_w.bar_b, 260 * b / 100);
}

/* ============================================================ */
/*                     手势                                       */
/* ============================================================ */
static void gesture_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_indev_get_point(in, &ui_w.p0);
    ui_w.axis = AXIS_NONE;
    ui_w.on_volbar = false;
    /* 按下点落在音量条上 → 直接进入拖音量模式 */
    lv_area_t ta;
    lv_obj_get_coords(ui_w.vol_track, &ta);
    if (ui_w.p0.x >= ta.x1 - 14 && ui_w.p0.x <= ta.x2 + 14 &&
        ui_w.p0.y >= ta.y1 - 22 && ui_w.p0.y <= ta.y2 + 22) {
        ui_w.on_volbar = true;
        ui_w.vol0 = -1;                  /* 标记: 由 x 位置直接定值 */
    }
}

static void gesture_pressing_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (ui_w.on_volbar) {
        /* 音量条拖动: x 位置 → 百分比 */
        lv_area_t ta;
        lv_obj_get_coords(ui_w.vol_track, &ta);
        int pct = (p.x - ta.x1) * 100 / (ta.x2 - ta.x1);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        ui_ctl_set_volume(pct);
        return;
    }
    int dx = p.x - ui_w.p0.x;
    int dy = p.y - ui_w.p0.y;
    if (ui_w.axis == AXIS_NONE) {
        if (abs(dx) > GEST_AXIS_PX && abs(dx) > abs(dy)) ui_w.axis = AXIS_SWITCH;
        else if (abs(dy) > GEST_AXIS_PX) ui_w.axis = AXIS_V;
        else return;
    }
    if (ui_w.axis == AXIS_V && abs(dy) > GEST_PANEL_PX) {
        ui_panel_show(dy > 0 ? 1 : 2);   /* 下滑=状态栏 上滑=AI 状态 */
        ui_w.p0.y = p.y;                 /* 防连触: 移动基准 */
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
    } else if (ui_w.axis == AXIS_NONE && !ui_w.on_volbar) {
        ui_panel_show(0);                /* 单击: 收起面板 */
    }
    ui_w.axis = AXIS_NONE;
    ui_w.on_volbar = false;
}

void ui_widget_gesture_register(lv_obj_t *scr)
{
    lv_obj_add_event_cb(scr, gesture_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, gesture_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, gesture_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, gesture_release_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* ============================================================ */
/*                     部件构建                                   */
/* ============================================================ */
static lv_obj_t *panel_make(lv_obj_t *scr, int h)
{
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, PANEL_W, h);
    lv_obj_set_style_radius(box, 28, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_PANEL_BG), 0);
    lv_obj_set_style_bg_opa(box, 235, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x223050), 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    return box;
}

/* 状态条(label + 自绘 track/fill), 返回 fill 供更新 */
static lv_obj_t *bar_make(lv_obj_t *parent, const char *name,
                          lv_obj_t **track_out)
{
    lv_obj_t *lbl = ui_label_make(parent, FONT_CJK, C_TXT2);
    lv_label_set_text(lbl, name);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 260, 8);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(fill, 8);
    lv_obj_set_width(fill, 0);
    lv_obj_set_style_radius(fill, 4, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(C_AI), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    if (track_out) *track_out = track;
    return fill;
}

void ui_widget_create(lv_obj_t *scr)
{
    /* ---- 表情层(全屏): 情绪底色圆 + EFC 常驻 ---- */
#if CONFIG_ESP_CUBE_ENABLE_EFC
    ui_w.face_bg = lv_obj_create(scr);
    lv_obj_remove_style_all(ui_w.face_bg);
    lv_obj_set_size(ui_w.face_bg, 430, 430);
    lv_obj_set_style_radius(ui_w.face_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ui_w.face_bg, 40, 0);
    lv_obj_set_style_bg_color(ui_w.face_bg, lv_color_hex(0x2A3550), 0);
    lv_obj_align(ui_w.face_bg, LV_ALIGN_CENTER, 0, 0);
    ui_w.face_efc = lv_efc_create(scr);
    lv_obj_set_size(ui_w.face_efc, 460, 460);
    lv_obj_align(ui_w.face_efc, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_inner_align(ui_w.face_efc,
                             LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE);
    lv_efc_set_src_data(ui_w.face_efc, giphy_z_efc_start,
                        (size_t)(giphy_z_efc_end - giphy_z_efc_start));
    lv_efc_set_loop_enabled(ui_w.face_efc, true);
    lv_efc_set_frame_delay(ui_w.face_efc, 55);
    if (lv_efc_is_loaded(ui_w.face_efc)) {
        lv_efc_play(ui_w.face_efc);
        ESP_LOGI("ui_face", "efc clip ok: %d frames @%dms",
                 (int)lv_efc_get_total_frames(ui_w.face_efc), 55);
    } else {
        LV_LOG_WARN("efc: clip load failed, face falls back to bg only");
        ESP_LOGE("ui_face", "efc clip load FAILED (bg-only face)");
        lv_obj_add_flag(ui_w.face_efc, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    /* ---- 状态栏面板(下滑; 初始藏屏上方外) ---- */
    ui_w.p_status = panel_make(scr, PANEL_H_STATUS);
    lv_obj_align(ui_w.p_status, LV_ALIGN_TOP_MID, 0, -PANEL_H_STATUS - 12);
    /* 圆屏几何: 顶部可视带窄(y=30 处仅中心 ±114px) —— 全部水平居中,
     * 角落内容会掉出圆外(2026-11-12 sim 截图定位) */
    ui_w.clock = ui_label_make(ui_w.p_status, FONT_M28, C_TXT);
    lv_obj_align(ui_w.clock, LV_ALIGN_TOP_MID, 0, 0);
    ui_w.dateline = ui_label_make(ui_w.p_status, FONT_CJK, C_TXT2);
    lv_obj_align(ui_w.dateline, LV_ALIGN_TOP_MID, 0, 44);
    ui_w.net = ui_label_make(ui_w.p_status, FONT_CJK, C_TXT2);
    lv_obj_align(ui_w.net, LV_ALIGN_TOP_MID, 0, 78);
    ui_w.power = ui_label_make(ui_w.p_status, FONT_CJK, C_AI);
    lv_label_set_text(ui_w.power, "外接供电");
    lv_obj_align(ui_w.power, LV_ALIGN_TOP_MID, 0, 104);

    /* 音量行: label + 自绘条 水平排布(y≈150 圆宽足够) */
    ui_w.vol_label = ui_label_make(ui_w.p_status, FONT_CJK, C_TXT);
    lv_label_set_text(ui_w.vol_label, "音量 --");
    ui_w.vol_fill = bar_make(ui_w.p_status, "", &ui_w.vol_track);
    lv_obj_align(ui_w.vol_track, LV_ALIGN_TOP_MID, 60, 158);
    lv_obj_align_to(ui_w.vol_label, ui_w.vol_track,
                    LV_ALIGN_OUT_LEFT_MID, -16, 0);
    ui_w.vol_w = 240;

    /* ---- AI 状态面板(上滑; 初始藏屏下方外) ---- */
    ui_w.p_soul = panel_make(scr, PANEL_H_SOUL);
    lv_obj_align(ui_w.p_soul, LV_ALIGN_BOTTOM_MID, 0, PANEL_H_SOUL + 16);
    ui_w.mood_word = ui_label_make(ui_w.p_soul, FONT_CJK24, C_TXT);
    lv_label_set_text(ui_w.mood_word, "苏醒中");
    lv_obj_align(ui_w.mood_word, LV_ALIGN_TOP_MID, 0, 2);
    ui_w.mood_tag_lbl = ui_label_make(ui_w.p_soul, FONT_M14, 0x8B98AC);
    lv_label_set_text(ui_w.mood_tag_lbl, "");
    lv_obj_align(ui_w.mood_tag_lbl, LV_ALIGN_TOP_MID, 0, 34);

    /* 三条(心情/活力/联结): 纯布局行, 点击穿透到手势层 */
    static const struct { int y; const char *name; } bars[3] = {
        { 62, "心情" }, { 92, "活力" }, { 122, "联结" },
    };
    lv_obj_t **bar_out[3] = { &ui_w.bar_v, &ui_w.bar_a, &ui_w.bar_b };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(ui_w.p_soul);
        lv_obj_remove_style_all(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, PANEL_W - 48, 26);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, bars[i].y);
        *bar_out[i] = bar_make(row, bars[i].name, NULL);
    }

    ui_w.sub = ui_label_make(ui_w.p_soul, FONT_CJK, C_TXT);
    lv_obj_set_width(ui_w.sub, 320);
    lv_label_set_long_mode(ui_w.sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui_w.sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_w.sub, LV_ALIGN_TOP_MID, 0, 156);
    ui_w.badge = ui_label_make(ui_w.p_soul, FONT_CJK, C_ACCENT);
    lv_obj_align(ui_w.badge, LV_ALIGN_TOP_MID, 0, 212);

    /* 面板本体接收手势(子控件已穿透; 在其上滑动/点按同屏语义) */
    ui_widget_gesture_register(ui_w.p_status);
    ui_widget_gesture_register(ui_w.p_soul);
}
