/*
 * ui_port_esp.c —— ui_model.h 设备后端(L5 装配层实现)
 *
 * 职责(契约见 app_ui/ui/ui_model.h 头注释):
 *   - 订阅事件总线 → 更新快照(字符串写入以 lvgl_port_lock 与 UI 任务互斥;
 *     事件回调在发布者任务上下文, 锁超时 100ms, 失败仅告警降级);
 *   - ui_ctl_* 控制 → 转发 app_player/app_bt/audio_mixer/drv_display;
 *   - ui_audio_take_pcm → 混音器频谱抽头;
 *   - 电台列表取自 app_player(单一来源), 不在本层存副本。
 *
 * 快照单写者: 本文件(事件回调+ctl); UI 任务只读轮询。
 */
#include "ui_model.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "app_bt.h"
#include "app_ui.h"
#include "drv_display.h"
#include "esp_lvgl_port.h"

static const char *TAG = "uiport";

static ui_model_t s_model;                 /* 单写者快照(本文件) */

/* ---- 字符串安全写入(与 UI 任务互斥; int/bool 字段天然原子免锁) ---- */
static void model_set_str(char *dst, const char *src, size_t cap)
{
    if (src == NULL) src = "";
    if (strcmp(dst, src) == 0) return;     /* 无变化免锁 */
    if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = 0;
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "lvgl lock timeout, str update dropped");
    }
}

/* ---- 音源推导(BT 接管优先; 与模拟器 mock 语义一致) ---- */
static void source_recalc(void)
{
    if (s_model.bt_connected) {
        s_model.source = UI_SRC_BT;
    } else if (s_model.player == UI_PLAYER_PLAYING ||
               s_model.player == UI_PLAYER_CONNECTING) {
        s_model.source = UI_SRC_RADIO;
    } else {
        s_model.source = UI_SRC_IDLE;
    }
}

/* ---- 事件回调(发布者任务上下文) ---- */
static void port_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                        void *ctx)
{
    (void)ctx;
    switch (id) {
    case CUBE_EVT_VOLUME_CHANGED:
        if (data != NULL) s_model.volume = *(const int *)data;
        break;
    case CUBE_EVT_PLAYER_STATE:
        if (data != NULL) {
            s_model.player = (ui_player_state_t)(*(const int *)data);
            source_recalc();
        }
        break;
    case CUBE_EVT_STREAM_TITLE:            /* ICY 元数据(char*, 回调期有效) */
        if (data != NULL) model_set_str(s_model.title, (const char *)data,
                                        sizeof(s_model.title));
        break;
    case CUBE_EVT_BT_CONNECTED:
        s_model.bt_connected = true;
        source_recalc();
        break;
    case CUBE_EVT_BT_DISCONNECTED:
        s_model.bt_connected = false;
        s_model.has_lyrics = false;
        s_model.lyric_cur[0] = s_model.lyric_next[0] = 0;
        source_recalc();
        break;
    case CUBE_EVT_BT_META: {
        const cube_bt_meta_t *m = (const cube_bt_meta_t *)data;
        if (m != NULL && len >= sizeof(*m)) {
            model_set_str(s_model.title, m->title, sizeof(s_model.title));
            model_set_str(s_model.artist, m->artist, sizeof(s_model.artist));
            model_set_str(s_model.album, m->album, sizeof(s_model.album));
            /* 歌词源待 M3 接入(BT 传输/本地 LRC); 当前 BT 曲目按纯音乐 */
            s_model.has_lyrics = false;
        }
        break;
    }
    case CUBE_EVT_WIFI_CONNECTED:
        s_model.wifi_up = true;
        break;
    case CUBE_EVT_WIFI_DISCONNECTED:
        s_model.wifi_up = false;
        break;
    case CUBE_EVT_AI_STATE:
        if (data != NULL) s_model.ai_state = *(const int *)data;
        break;
    case CUBE_EVT_AI_TEXT:
        if (data != NULL) model_set_str(s_model.ai_text, (const char *)data,
                                        sizeof(s_model.ai_text));
        break;
    default:
        break;
    }
}

/* ---- 初始化(main 在 app_ui_start 之前调用) ---- */
esp_err_t ui_port_init(void)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.player = UI_PLAYER_IDLE;
    s_model.source = UI_SRC_IDLE;
    s_model.volume = audio_mixer_get_master_volume();
    s_model.brightness = 100;
    s_model.station_num = app_player_station_num();
    if (s_model.station_num > UI_MAX_STATIONS) {
        s_model.station_num = UI_MAX_STATIONS;
    }
    for (int i = 0; i < s_model.station_num; i++) {
        s_model.station_names[i] = app_player_station_name(i);
    }
    s_model.station_idx = 0;
    s_model.buffer_pct = -1;               /* <0 = 无数据(网络缓冲遥测 M3) */
    strncpy(s_model.fw_info, "EspCube dev", sizeof(s_model.fw_info) - 1);

    esp_err_t err = cube_event_subscribe(port_evt_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "evt subscribe: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ui_port ready (stations=%d vol=%d)",
             s_model.station_num, s_model.volume);
    return ESP_OK;
}

/* ---- ui_model.h 契约实现 ---- */
const ui_model_t *ui_model(void)
{
    return &s_model;
}

void ui_ctl_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    audio_mixer_set_master_volume(percent);
}

void ui_ctl_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    drv_display_set_brightness(percent);
    s_model.brightness = percent;
}

void ui_ctl_toggle_play(void)
{
    if (s_model.source == UI_SRC_BT) {
        ESP_LOGI(TAG, "toggle in BT: AVRCP passthrough = M3");
        return;                             /* BT 播放由手机控制(M3 透传) */
    }
    if (s_model.player == UI_PLAYER_PLAYING ||
        s_model.player == UI_PLAYER_CONNECTING) {
        app_player_stop();
    } else {
        app_player_start(NULL);             /* 列表轮播(记住台号) */
    }
}

void ui_ctl_switch_station(int dir)
{
    if (s_model.source == UI_SRC_BT) return; /* BT 接管期不切台 */
    int n = s_model.station_num > 0 ? s_model.station_num : 1;
    int idx = s_model.station_idx;
    app_player_play_index(((idx + dir) % n + n) % n);
}

void ui_ctl_select_station(int idx)
{
    if (idx < 0 || idx >= s_model.station_num) return;
    app_player_play_index(idx);
}

int ui_audio_take_pcm(int16_t *stereo, int frames)
{
    return audio_mixer_take_spectrum(stereo, frames) ? frames : 0;
}
