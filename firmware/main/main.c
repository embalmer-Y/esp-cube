/*
 * main.c —— EspCube 装配层
 *
 * 只负责模块初始化顺序(依赖关系见《02_详细设计.md》§2), 不含业务逻辑。
 *
 * 初始化顺序(关键约束):
 *   1. cubecore 事件总线(最先, 其余模块靠它解耦);
 *   2. drv_audio(codec 就绪, 静音态) → audio_mixer(混音任务=节拍源);
 *   3. app_ui(显示; 独立失败域, 屏幕异常不阻断音频);
 *   4. app_net(WiFi 异步连接) / app_bt(蓝牙可连接态) —— 二者共存;
 *   5. WiFi 就绪后启动默认网络电台(演示入口)。
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "cubecore.h"
#include "board_config.h"
#include "nvs_flash.h"
#include "esp_coexist.h"
#include "drv_audio.h"
#include "drv_display.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "app_net.h"
#include "app_ui.h"
#include "app_bt.h"

#if CONFIG_ESP_CUBE_ENABLE_VOICE
#include "app_voice.h"
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
#include "ai_session.h"
#endif

/* ui_model.h 设备后端(本目录 ui_port_esp.c, L5 实现 docs/06 §2.1) */
extern esp_err_t ui_port_init(void);

static const char *TAG = "main";

/* WiFi 就绪 → 启动播放列表(一次性任务: app_player_start 内含停止等待,
 * 不可在事件循环任务里同步调用 —— 会阻塞所有事件分发) */
static void player_start_task(void *arg)
{
    (void)arg;
    app_player_start(NULL);         /* 无参 = 播放列表轮播 */
    vTaskDelete(NULL);
}

/* ---- 演示事件观察者(仅策略与日志; 跨模块功能由各模块自订阅,
 *      如唤醒→AI 会话由 ai_session 自订阅 CUBE_EVT_WAKE_DETECTED) ---- */
static void demo_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WIFI_CONNECTED) {
        ESP_LOGI(TAG, "EVT: WiFi connected");
        BaseType_t r = xTaskCreate(player_start_task, "pstart", 4096, NULL, 8, NULL);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "player start task create failed");
        }
    }
}

static void app_start_delay_hint(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " EspCube cube-speaker prototype");
    ESP_LOGI(TAG, " MCU : %s (16MB Flash + %dMB PSRAM)", "ESP32-S31-WROOM-3", BOARD_PSRAM_SIZE_MB);
    ESP_LOGI(TAG, " Audio: %s + %s (board builtin)", BOARD_AUDIO_DAC, BOARD_AUDIO_AMP);
    ESP_LOGI(TAG, " BT  : A2DP sink \"EspCube\" | NET: %s", CONFIG_ESP_CUBE_WIFI_SSID);
    ESP_LOGI(TAG, "=================================================");
}

void app_main(void)
{
    app_start_delay_hint();

    /* ---- 1. 公共设施: 事件总线为静态实现; NVS 最先(WiFi/BT 都依赖) ---- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* ---- 2. 音频链路(核心) ---- */
    drv_audio_cfg_t acfg = {
        .sample_rate = CONFIG_ESP_CUBE_MIXER_RATE,
        .channels = 2,
        .mic_sample_rate = CONFIG_ESP_CUBE_MIXER_RATE,  /* 全双工同 LRCK, 上层降采样 */
    };
    ESP_ERROR_CHECK(drv_audio_init(&acfg));
    audio_mixer_cfg_t mcfg = {
        .sample_rate = CONFIG_ESP_CUBE_MIXER_RATE,
        .channels = 2,
    };
    ESP_ERROR_CHECK(audio_mixer_init(&mcfg));
    audio_mixer_set_master_volume(CONFIG_ESP_CUBE_DEFAULT_VOLUME);
    /* 混音任务先起, 成功后才开功放: 否则 mixer 启动失败 → abort → 重启
     * 循环, 每次循环 PA 都"噗"一声(失败域审查 2026-11-12) */
    ESP_ERROR_CHECK(audio_mixer_start());
    ESP_ERROR_CHECK(drv_audio_pa_set(true));

    /* ---- 3. 语音 AI(里程碑2): 先于显示/蓝牙初始化 —— esp-sr 模型解密
     * 需要大块连续内部 RAM, 放最后会被 WiFi/BT/LVGL 挤死(2026-11-12
     * 实测 free heap≈14.8MB 全是 PSRAM, 内部仅剩 6.5KB → wakenet 解密
     * 失败/HLS 重连 20KB 分配失败/ai 任务创建失败; Kaleido V-04 同序)。
     * 可选功能一律降级不 abort。 ---- */
#if CONFIG_ESP_CUBE_ENABLE_VOICE
    esp_err_t verr = app_voice_start();
    if (verr != ESP_OK) {
        ESP_LOGE(TAG, "voice degraded: %s", esp_err_to_name(verr));
    }
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
    esp_err_t aerr = ai_session_start();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "ai session degraded: %s", esp_err_to_name(aerr));
    }
#endif

#if CONFIG_ESP_CUBE_ENABLE_UI
    /* ---- 4. 显示(独立失败域) ---- */
#if CONFIG_ESP_CUBE_LCD_SINGLE_LANE_TEST
    drv_display_single_lane_test(CONFIG_ESP_CUBE_LCD_H_RES,
                                 CONFIG_ESP_CUBE_LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(5000));     /* 观察窗: 人工判定单线是否点亮 */
#endif
    drv_display_cfg_t dcfg = {
        .pixel_clock_hz = CONFIG_ESP_CUBE_LCD_PIXEL_CLOCK_HZ,
        .h_res = CONFIG_ESP_CUBE_LCD_H_RES,
        .v_res = CONFIG_ESP_CUBE_LCD_V_RES,
        .use_te = false,                                /* M3 启用 TE 同步 */
    };
    esp_err_t derr = drv_display_init(&dcfg);
    if (derr == ESP_OK) {
        ESP_ERROR_CHECK(ui_port_init());                /* ui_model 设备后端 */
        app_ui_boot_cfg_t ucfg = {
            .io_handle = drv_display_get_io(),
            .panel_handle = drv_display_get_panel(),
            .h_res = CONFIG_ESP_CUBE_LCD_H_RES,
            .v_res = CONFIG_ESP_CUBE_LCD_V_RES,
        };
        ESP_ERROR_CHECK(app_ui_start(&ucfg));
    } else {
        ESP_LOGE(TAG, "display init failed(%s), audio-only mode", esp_err_to_name(derr));
    }
#endif

    /* ---- 4. 音源(网络 / 蓝牙) ---- */
    ESP_ERROR_CHECK(app_player_init());     /* 订阅 BT 接管事件(须先于 BT 启动) */
#if CONFIG_ESP_CUBE_ENABLE_NET
    /* NET 先于 BT(2026-11-10 实机教训): WiFi 初始化需要大块连续内部 RAM
     * (10×1700B 静态rx缓冲), 放在 BT(控制器+Bluedroid ~80KB)之后启动时
     * 内存余量临界, 实机出现 malloc buffer fail → ESP_ERROR_CHECK abort
     * → 开机循环(每秒功放"噗"声)。WiFi 需求最硬故最先初始化。
     * 失败一律降级(蓝牙/本地仍可用), 绝不 abort。 */
    esp_err_t nerr = app_net_start();
    ESP_LOGI(TAG, "wifi: %s", nerr == ESP_OK ? "ok" : esp_err_to_name(nerr));
#endif
#if CONFIG_ESP_CUBE_ENABLE_BT
    esp_err_t berr = app_bt_start();
    ESP_LOGI(TAG, "A2DP sink: %s", berr == ESP_OK ? "ready" : esp_err_to_name(berr));
#endif
#if CONFIG_ESP_CUBE_ENABLE_BT && CONFIG_ESP_CUBE_ENABLE_NET
    /* 空口优先级(M1.8.4): WiFi/BT 共存调度默认均衡, 实测 A2DP 高码率流
     * 会被 WiFi 间歇挤占(远距离重传放大), 数秒断流直至手机放弃断开。
     * 显式声明 BT 优先 + WiFi 保持 PS_NONE(实测 PS_MIN_MODEM 的周期唤醒
     * 反而使干扰均匀化, 远近距离都卡)。语音 AI 上线后如需再平衡再调。 */
    esp_err_t cerr = esp_coex_preference_set(ESP_COEX_PREFER_BT);
    ESP_LOGI(TAG, "coex prefer BT: %s", cerr == ESP_OK ? "ok" : esp_err_to_name(cerr));
#endif

    /* ---- 6. 演示观察者 ---- */
    cube_event_subscribe(demo_evt_cb, NULL);

    ESP_LOGI(TAG, "startup complete, free heap=%u / psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
