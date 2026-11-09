/*
 * app_bt.c —— A2DP Sink + AVRCP 实现(基于 IDF v6.1 Bluedroid Classic API)
 *
 * 说明(与调研结论对应, 见《01_技术方案.md》§1.4):
 *   - S31 原生 BT 5.4 含 BR/EDR,A2DP Sink 为官方支持路径;
 *   - 默认使用 Bluedroid 内置 SBC 解码器: esp_a2d_sink_register_data_callback
 *     直接得到 PCM(参考官方 a2dp_sink_stream 例程);
 *   - 内置解码器输出采样率无显式上报, 按手机通用默认 44.1kHz 处理
 *     (Kconfig 可改 48k);里程碑2 可切换为"外置解码"模式
 *     (esp_a2d_sink_register_stream_endpoint + esp_audio_codec SBC 解码),
 *     从协商参数中取得准确采样率;
 *   - AVRCP CT(Controller)拉取曲目元数据/播放状态上报 UI, 并在切歌
 *     (TRACK_CHANGE 通知)时自动刷新;
 *   - AVRCP TG(Target)接收手机 absolute volume 下发映射为混音器主音量,
 *     本地改音量(语音/UI)经 CUBE_EVT_VOLUME_CHANGED 回传手机(M1.8 补齐)。
 */
#include "app_bt.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "audio_mixer.h"

static const char *TAG = "bt";

#define BT_DEVICE_NAME      "EspCube"
/* 内置 SBC 解码输出(手机 A2DP 常见 44.1k; 可经 Kconfig 调整) */
#define BT_SRC_RATE         CONFIG_ESP_CUBE_BT_SRC_RATE
/* 重采样工作缓冲: SBC 帧粒度 ~ (128*16) 样本, 8k 帧余量充足 */
#define BT_PCM_BUF_SAMPLES  1024   /* SBC 单帧 ≤512样本, 1024 足够且省 48KB 内部RAM */

static struct {
    bool connected;
    bool first_data_logged;     /* 本次连接是否已记录首包(每次连接复位) */
    bool vol_notify_pending;    /* 手机已注册音量通知, 允许回传一次 CHANGED */
    audio_source_handle_t src;
    cube_resampler_t rs;
    /* 重采样输入缓冲(回调上下文, BT栈任务) */
    int16_t in[BT_PCM_BUF_SAMPLES * 2];
    int16_t out[BT_PCM_BUF_SAMPLES * 2];
} s_bt;

/* ================= PCM 数据回调(BT栈任务上下文) =================
 * 内置 SBC 解码后的 PCM, 交错 int16 双声道。
 * 注意: 此回调在蓝牙栈任务执行, 必须短平快 —— 只做重采样+推缓冲。
 */
static void bt_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (s_bt.src == NULL) return;
    /* 首包日志: 确认手机端媒体流真的开始推送(排查"配对成功但无声") */
    if (!s_bt.first_data_logged) {
        s_bt.first_data_logged = true;
        ESP_LOGI(TAG, "A2DP first audio data: %u bytes (stream flowing)", (unsigned)len);
        /* 首次音频数据 = 手机真在放歌: 通知播放器让出电台(事件总线解耦) */
        bool active = true;
        cube_event_publish(CUBE_EVT_BT_AUDIO_ACTIVE, &active, sizeof(active));
    }
    size_t in_frames = len / 4;                     /* 16bit×2ch */
    if (in_frames > BT_PCM_BUF_SAMPLES) in_frames = BT_PCM_BUF_SAMPLES;
    memcpy(s_bt.in, data, in_frames * 4);

    size_t out_frames = cube_resampler_process_stereo(
            &s_bt.rs, s_bt.in, in_frames, s_bt.out, BT_PCM_BUF_SAMPLES);
    if (out_frames > 0) {
        audio_mixer_source_write(s_bt.src, s_bt.out,
                                 out_frames * 2 * sizeof(int16_t), 0);
    }
}

/* ================= AVRCP ================= */
/* 曲目元数据累计缓冲: 每次事件只携带一个属性, 累齐后发布 */
static cube_bt_meta_t s_bt_meta;

/* AVRCP absolute volume(0..127) 与混音器主音量百分比(0..100) 互转(四舍五入) */
static int absvol_to_pct(uint8_t v)   { return ((int)v * 100 + 63) / 127; }
static uint8_t pct_to_absvol(int pct) { return (uint8_t)((pct * 127 + 50) / 100); }

/* 拉取当前曲目元数据 + 注册一次性通知(AVRCP 规范: 通知触发后须重新注册) */
static void bt_avrc_refresh_track(void)
{
    memset(&s_bt_meta, 0, sizeof(s_bt_meta));
    esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE |
                                    ESP_AVRC_MD_ATTR_ARTIST |
                                    ESP_AVRC_MD_ATTR_ALBUM);
    esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_CHANGE, 0);
}

static void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_METADATA_RSP_EVT) {
        /* v6.1 语义: 每次回调仅含一个属性(attr_id + attr_text) */
        const char *text = (const char *)param->meta_rsp.attr_text;
        int len = param->meta_rsp.attr_length;
        if (text == NULL || len <= 0) {
            return;
        }
        switch (param->meta_rsp.attr_id) {
        case ESP_AVRC_MD_ATTR_TITLE: {
            size_t n = (size_t)len < sizeof(s_bt_meta.title) - 1
                       ? (size_t)len : sizeof(s_bt_meta.title) - 1;
            memcpy(s_bt_meta.title, text, n);
            s_bt_meta.title[n] = 0;
            /* 无屏期间日志即验收手段: 曲名/歌手直接可见 */
            ESP_LOGI(TAG, "track: \"%s\" / %s [%s]", s_bt_meta.title,
                     s_bt_meta.artist[0] ? s_bt_meta.artist : "-",
                     s_bt_meta.album[0] ? s_bt_meta.album : "-");
            break;
        }
        case ESP_AVRC_MD_ATTR_ARTIST: {
            size_t n = (size_t)len < sizeof(s_bt_meta.artist) - 1
                       ? (size_t)len : sizeof(s_bt_meta.artist) - 1;
            memcpy(s_bt_meta.artist, text, n);
            s_bt_meta.artist[n] = 0;
            break;
        }
        case ESP_AVRC_MD_ATTR_ALBUM: {
            size_t n = (size_t)len < sizeof(s_bt_meta.album) - 1
                       ? (size_t)len : sizeof(s_bt_meta.album) - 1;
            memcpy(s_bt_meta.album, text, n);
            s_bt_meta.album[n] = 0;
            break;
        }
        default:
            return;                             /* 其他属性(封面等)忽略 */
        }
        cube_event_publish(CUBE_EVT_BT_META, &s_bt_meta, sizeof(s_bt_meta));
    } else if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        if (param->conn_stat.connected) {
            /* AVRCP 通道就绪: 元数据拉取/通知注册统一在此
             * (A2DP 先于 AVRCP 建立, 在 A2DP 事件里发 AVRCP 命令会因通道未就绪而失败) */
            ESP_LOGI(TAG, "AVRCP connected");
            bt_avrc_refresh_track();
        }
    } else if (event == ESP_AVRC_CT_CHANGE_NOTIFY_EVT) {
        if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            bool playing = (param->change_ntf.event_parameter.playback ==
                            ESP_AVRC_PLAYBACK_PLAYING);
            cube_event_publish(CUBE_EVT_BT_PLAY_STATE, &playing, sizeof(playing));
            /* 通知为一次性: 触发后重注册, 否则后续播放状态变化收不到 */
            esp_avrc_ct_send_register_notification_cmd(1,
                    ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        } else if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            /* 切歌: 刷新曲名/歌手并重注册 */
            bt_avrc_refresh_track();
        }
    }
}

/* ================= AVRCP TG(目标端: absolute volume 接收) =================
 * 手机 = CT, 音箱 = TG。手机音量滑条下发 SET_ABSOLUTE_VOLUME(0-127) 映射为
 * 混音器主音量; 本地改音量(语音/UI)→ mixer 发布 CUBE_EVT_VOLUME_CHANGED →
 * bt_evt_cb 回传 CHANGED(每次注册仅一次, 手机按规范会自动重注册)。
 */
static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP TG %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        int pct = absvol_to_pct(param->set_abs_vol.volume);
        ESP_LOGI(TAG, "phone absolute volume: %u/127 -> %d%%",
                 param->set_abs_vol.volume, pct);
        s_bt.vol_notify_pending = false;    /* 手机发起, 无需回传 */
        audio_mixer_set_master_volume(pct);
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            /* 规范要求先回 INTERIM(当前值); 之后本地第一次改音量回 CHANGED */
            esp_avrc_rn_param_t rn = {
                .volume = pct_to_absvol(audio_mixer_get_master_volume())
            };
            s_bt.vol_notify_pending = true;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                    ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        ESP_LOGD(TAG, "passthrough key %u %s", param->psth_cmd.key_code,
                 param->psth_cmd.key_state ? "released" : "pressed");
        break;
    default:
        break;
    }
}

/* 事件总线订阅: 本地音量变化(语音 ±10 / 未来 UI 滑条) → 回传手机 */
static void bt_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    if (id != CUBE_EVT_VOLUME_CHANGED || data == NULL || len != sizeof(int)) {
        return;                             /* 事件总线全量派发: 必须按 ID 过滤 */
    }
    if (!s_bt.connected || !s_bt.vol_notify_pending) {
        return;                             /* 未注册通知时不回传(避免栈未就绪) */
    }
    esp_avrc_rn_param_t rn = { .volume = pct_to_absvol(*(const int *)data) };
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
    s_bt.vol_notify_pending = false;
}

/* ================= A2DP 事件 ================= */
static void bt_a2d_evt_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_bt.connected = true;
            s_bt.first_data_logged = false;
            ESP_LOGI(TAG, "A2DP connected");
            cube_event_publish(CUBE_EVT_BT_CONNECTED, NULL, 0);
            /* 元数据/通知注册移至 AVRCP CT 连接事件(bt_avrc_refresh_track) */
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_bt.connected = false;
            ESP_LOGI(TAG, "A2DP disconnected");
            bool active = false;
            cube_event_publish(CUBE_EVT_BT_AUDIO_ACTIVE, &active, sizeof(active));
            cube_event_publish(CUBE_EVT_BT_DISCONNECTED, NULL, 0);
        }
        break;
    default:
        break;
    }
}

/* ================= GAP 事件(配对/连接管理, M1 仅日志) ================= */
static void bt_gap_evt_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "auth complete: %s, status=%d",
                 param->auth_cmpl.device_name, param->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_DISC_RES_EVT:
        /* 设备发现(日志级调试, M1 不处理具体发现流程) */
        ESP_LOGD(TAG, "disc res, %d results", param->disc_res.num_prop);
        break;
    default:
        break;
    }
}

/* ================= 协议栈启动 ================= */
esp_err_t app_bt_start(void)
{
    if (s_bt.src != NULL) {
        return ESP_OK;                          /* 幂等 */
    }
    /* 混音器音源通道(300ms 缓冲, 抗蓝牙抖动) */
    /* 蓝牙同样按实时速率推送: 2秒缓冲+500ms预填充, 显著抗抖动 */
    audio_source_cfg_t scfg = { .name = "bt", .type = AUDIO_SRC_BT,
                                .buffer_ms = 2000, .prefill_ms = 500 };
    s_bt.src = audio_mixer_add_source(&scfg);
    ESP_RETURN_ON_FALSE(s_bt.src != NULL, ESP_ERR_NO_MEM, TAG, "add source fail");
    cube_resampler_init(&s_bt.rs, (float)BT_SRC_RATE, (float)CONFIG_ESP_CUBE_MIXER_RATE);

    /* ---- 控制器(纯经典蓝牙模式, 见 sdkconfig.defaults BTDM_CTRL_MODE) ----
     * 释放 BLE 独占内存: M1 无 BLE 需求, 给协议栈/音频腾内部 RAM */
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "ctrl init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "ctrl enable");

    /* ---- Bluedroid ---- */
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");

    ESP_RETURN_ON_ERROR(esp_bt_gap_set_device_name(BT_DEVICE_NAME), TAG, "set name");
    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(bt_gap_evt_cb), TAG, "gap cb");

    /* AVRCP Controller 必须先于 A2DP 初始化(栈要求), 提供元数据/播放状态 */
    ESP_RETURN_ON_ERROR(esp_avrc_ct_register_callback(bt_avrc_ct_cb), TAG, "avrc cb");
    ESP_RETURN_ON_ERROR(esp_avrc_ct_init(), TAG, "avrc init");
    /* AVRCP Target: 接收手机 absolute volume / 回传本地音量变化 */
    ESP_RETURN_ON_ERROR(esp_avrc_tg_register_callback(bt_avrc_tg_cb), TAG, "avrc tg cb");
    ESP_RETURN_ON_ERROR(esp_avrc_tg_init(), TAG, "avrc tg init");

    /* A2DP Sink: 内置 SBC 解码 → PCM 回调 */
    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(bt_a2d_evt_cb), TAG, "a2d cb");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_register_data_callback(bt_a2d_data_cb), TAG, "a2d data cb");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2d sink init");

    /* 本地音量变化(语音/UI) → AVRCP CHANGED 回传手机 */
    ESP_RETURN_ON_ERROR(cube_event_subscribe(bt_evt_cb, NULL), TAG, "subscribe vol");

    /* 可连接+可发现: 等待手机配对连接 */
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                 ESP_BT_GENERAL_DISCOVERABLE),
                        TAG, "scan mode");
    ESP_LOGI(TAG, "A2DP sink \"%s\" ready (AVRCP meta+absvol), waiting for phone...",
             BT_DEVICE_NAME);
    return ESP_OK;
}

bool app_bt_is_connected(void)
{
    return s_bt.connected;
}
