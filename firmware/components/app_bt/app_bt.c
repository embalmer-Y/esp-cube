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
 *   - AVRCP Controller 负责拉取曲目元数据/播放状态上报 UI。
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
    } else if (event == ESP_AVRC_CT_CHANGE_NOTIFY_EVT) {
        if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            bool playing = (param->change_ntf.event_parameter.playback ==
                            ESP_AVRC_PLAYBACK_PLAYING);
            cube_event_publish(CUBE_EVT_BT_PLAY_STATE, &playing, sizeof(playing));
        }
    }
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
            /* 元数据累计缓冲清零, 重新拉取曲目元数据 + 注册播放状态通知 */
            memset(&s_bt_meta, 0, sizeof(s_bt_meta));
            esp_avrc_ct_send_metadata_cmd(0, ESP_AVRC_MD_ATTR_TITLE |
                                            ESP_AVRC_MD_ATTR_ARTIST |
                                            ESP_AVRC_MD_ATTR_ALBUM);
            esp_avrc_ct_send_register_notification_cmd(1,
                    ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_bt.connected = false;
            ESP_LOGI(TAG, "A2DP disconnected");
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

    /* A2DP Sink: 内置 SBC 解码 → PCM 回调 */
    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(bt_a2d_evt_cb), TAG, "a2d cb");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_register_data_callback(bt_a2d_data_cb), TAG, "a2d data cb");
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2d sink init");

    /* 可连接+可发现: 等待手机配对连接 */
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                 ESP_BT_GENERAL_DISCOVERABLE),
                        TAG, "scan mode");
    ESP_LOGI(TAG, "A2DP sink \"%s\" ready, waiting for phone...", BT_DEVICE_NAME);
    return ESP_OK;
}

bool app_bt_is_connected(void)
{
    return s_bt.connected;
}
