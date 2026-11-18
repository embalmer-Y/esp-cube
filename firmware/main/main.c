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
#include "act_gate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"    /* xTaskCreateWithCaps(PSRAM 栈, docs/22 §3B) */
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "cubecore.h"
#include "board_config.h"
#include "board_pins.h"
#include "nvs_flash.h"
#include "drv_audio.h"
#include "drv_display.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "app_net.h"
#include "app_ui.h"
#include "face_perf.h"
#if CONFIG_ESP_CUBE_ENABLE_BT
#include "esp_coexist.h"
#include "app_bt.h"
#endif

#if CONFIG_ESP_CUBE_ENABLE_VOICE
#include "app_voice.h"
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
#include "ai_session.h"
#include "pair_server.h"
#include "soul.h"
#endif
#include "app_assist.h"
#include "app_storage.h"

/* ui_model.h 设备后端(本目录 ui_port_esp.c, L5 实现 docs/06 §2.1) */
extern esp_err_t ui_port_init(void);
extern esp_err_t ui_port_touch_start(void);   /* M4 触摸 indev */

/* 串口命令行(本目录 cli.c) */
#include "cli.h"
/* 内存预算审计(docs/22; 启动完成时对账一次, 平时 CLI `mem`) */
#include "mem_budget.h"
/* AI 技能注册(本目录 ai_skills_setup.c, L5 依赖注入) */
extern void register_ai_skills(void);

static const char *TAG = "main";

/* M2: 提醒到点带外播报桥(app_assist 调度任务 → ai_session_announce,
 * 异步入队不阻塞调度器; 队列满即弃 —— 提醒洪峰时宁缺勿堵) */
static void assist_announce_bridge(const char *text)
{
#if CONFIG_ESP_CUBE_ENABLE_AI
    esp_err_t err = ai_session_announce(text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "announce dropped: %s", esp_err_to_name(err));
    }
#else
    (void)text;
#endif
}

/* WiFi 就绪 → 启动播放列表 —— 音响时代开机行为, 仅 SOUL 未启用目标
 * 保留(S31 回退演示路径)。SOUL=y 时开机播台 = 灵魂 L0 反射(docs/14
 * §6: 电台是 AI 的能力, 决策权归灵魂), 装配层不再插手。
 * (一次性任务: app_player_start 内含停止等待, 不可在事件循环任务里
 * 同步调用 —— 会阻塞所有事件分发) */
#if !CONFIG_ESP_CUBE_ENABLE_SOUL
static void player_start_task(void *arg)
{
    (void)arg;
    app_player_start(NULL);         /* 无参 = 播放列表轮播 */
    vTaskDeleteWithCaps(NULL);      /* 栈在 PSRAM(WithCaps 创建须配对删除) */
}
#endif

/* ---- 演示事件观察者(仅策略与日志; 跨模块功能由各模块自订阅,
 *      如唤醒→AI 会话由 ai_session 自订阅 CUBE_EVT_WAKE_DETECTED;
 *      灵魂音乐行动由 app_player 自订阅 CUBE_EVT_SOUL_ACT) ---- */
static void demo_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WIFI_CONNECTED) {
        ESP_LOGI(TAG, "EVT: WiFi connected");
#if !CONFIG_ESP_CUBE_ENABLE_SOUL
        /* 瞬态任务栈走 PSRAM(docs/22 §3B): 一次性启动器, 与 player 同类
         * 纯网络负载(PSRAM 栈 2026-11-10 起板上验证), 不占内部预算 */
        BaseType_t r = xTaskCreateWithCaps(player_start_task, "pstart", 4096,
                                           NULL, 8, NULL, MALLOC_CAP_SPIRAM);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "player start task create failed");
        }
#endif
    }
}

static void app_start_delay_hint(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " EspCube cube-speaker prototype");
    ESP_LOGI(TAG, " MCU : %s (%dMB Flash + %dMB PSRAM)", BOARD_NAME,
             BOARD_FLASH_SIZE_MB, BOARD_PSRAM_SIZE_MB);
    ESP_LOGI(TAG, " Audio: %s + %s (board builtin)", BOARD_AUDIO_DAC, BOARD_AUDIO_AMP);
#if CONFIG_ESP_CUBE_ENABLE_BT
    ESP_LOGI(TAG, " BT  : A2DP sink \"EspCube\" | NET: %s", CONFIG_ESP_CUBE_WIFI_SSID);
#else
    ESP_LOGI(TAG, " BT  : not supported on this target | NET: %s", CONFIG_ESP_CUBE_WIFI_SSID);
#endif
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

    /* ---- 1.1 内部堆观测点(docs/19 §52 修复 B 复盘):
     * 曾尝试 boot 期预留 28KB 给 taskLVGL 栈, 但预留窗口内 esp_hosted 的
     * SDIO TX 聚合缓冲(DMA 对齐 15872B)分配失败 → assert 复位循环(板上
     * 实证)。改为记录关键点最大连续内部块, 依据数据选择收紧点。 */
    ESP_LOGI(TAG, "mem ckpt app_main: largest internal=%u",
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    /* ---- 1.5 助理服务(M2): SNTP 时间底座 + 琐事/提醒(NVS) ---- */
    ESP_ERROR_CHECK(app_assist_init());
    app_assist_set_announcer(assist_announce_bridge);

    /* ---- 1.6 SD 卡存储(M3+ 记忆系统强制底座; 无卡降级不阻塞) ---- */
    if (app_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "storage degraded: mem->NVS, assets->flash");
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

    /* ---- 3. 语音 AI(里程碑2): 初始化次序按目标分派 ——
     * S31: voice/AI 先于显示/网络 —— esp-sr 模型解密需要大块连续内部
     * RAM, 放最后会被 WiFi/BT/LVGL 挤死(2026-11-12 实测内部仅剩 6.5KB;
     * Kaleido V-04 同序)。
     * P4: 语音在 WiFi 之后(V-45 铁律: hosted SDIO × 显示 × wakenet 共存
     * 期次序敏感), 见下方 net 启动后。
     * 可选功能一律降级不 abort。 ---- */
#if CONFIG_ESP_CUBE_ENABLE_VOICE && !CONFIG_IDF_TARGET_ESP32P4
    esp_err_t verr = app_voice_start();
    if (verr != ESP_OK) {
        ESP_LOGE(TAG, "voice degraded: %s", esp_err_to_name(verr));
    }
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
    act_gate_init();                        /* 统一动作门(docs/18 T13; 先于技能/灵魂) */
    register_ai_skills();                   /* L5 技能注入(先于会话启动) */
    /* T17: LLM 密钥经配对管理通道(credstore)优先, Kconfig 兜底 */
    ai_session_set_api_key_provider(pair_server_llm_key);
    ai_session_set_profile_provider(pair_server_profile_settings);
    esp_err_t aerr = ai_session_start();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "ai session degraded: %s", esp_err_to_name(aerr));
    }
    /* M3 灵魂内核: 1Hz 情绪动力学 + 记忆(SOUL=n 时为空实现) */
    esp_err_t serr = soul_start();
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "soul degraded: %s", esp_err_to_name(serr));
    }
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
    /* 灵魂 2.0 深思轮: brain=深思 LLM 通道, speak=带外播报;
     * face/music 行动经 CUBE_EVT_SOUL_ACT 广播(music 桥在此订阅) */
    soul_think_set_brain(ai_session_deliberate);
    soul_think_set_speak(ai_session_announce);
    soul_think_start();
#endif
#endif

    /* ---- 3.5 网络(整块自"音源"前移, P4 关键) ----
     * P4: net 必须先于显示/触摸 —— esp_hosted 初始化要 15872B 连续
     * 内部 DMA 缓冲, 显示(LVGL/EFC)+触摸先吃会碎片化 → hosted
     * "cannot allocate" assert 重启循环(hosted 必须先挑内存,
     * 2026-11-12 板上两次定位)。 */
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

#if CONFIG_ESP_CUBE_ENABLE_UI
    /* ---- 4. 显示(独立失败域) ---- */
#if CONFIG_ESP_CUBE_LCD_SINGLE_LANE_TEST
    drv_display_single_lane_test(CONFIG_ESP_CUBE_LCD_H_RES,
                                 CONFIG_ESP_CUBE_LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(5000));     /* 观察窗: 人工判定单线是否点亮 */
#endif
    drv_display_cfg_t dcfg = {
#if CONFIG_ESP_CUBE_BOARD_P4C5
        /* MIPI-DSI 后端: DPI 像素钟/时序取板级常量, 此处仅传分辨率 */
        .pixel_clock_hz = BOARD_LCD_DPI_CLK_MHZ * 1000000,
#else
        .pixel_clock_hz = CONFIG_ESP_CUBE_LCD_PIXEL_CLOCK_HZ,
#endif
        .h_res = CONFIG_ESP_CUBE_LCD_H_RES,
        .v_res = CONFIG_ESP_CUBE_LCD_V_RES,
        .use_te = false,                                /* M3 启用 TE 同步 */
    };
    esp_err_t derr = drv_display_init(&dcfg);
    if (derr == ESP_OK) {
        ESP_ERROR_CHECK(ui_port_init());                /* ui_model 设备后端 */
        extern bool app_net_is_up(void);                /* net 早于 UI 启动 */
        if (app_net_is_up()) cube_event_publish(CUBE_EVT_WIFI_CONNECTED, NULL, 0);
        app_ui_boot_cfg_t ucfg = {
            .io_handle = drv_display_get_io(),
            .panel_handle = drv_display_get_panel(),
            .h_res = CONFIG_ESP_CUBE_LCD_H_RES,
            .v_res = CONFIG_ESP_CUBE_LCD_V_RES,
#if CONFIG_ESP_CUBE_BOARD_P4C5
            .iface = UI_DISP_IF_DSI,
#else
            .iface = UI_DISP_IF_QSPI,
#endif
        };
        ESP_LOGI(TAG, "mem ckpt pre_ui: largest internal=%u dma=%u",
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        /* UI 为独立失败域(docs/06 §6): 失败不 abort 也不重试(lvgl_port
         * 失败→deinit→re-init 路径会损坏 TLSF 堆, v3 板上实证
         * block_locate_free/remove_free_block 断言)。降级无屏运行。 */
        esp_err_t uerr = app_ui_start(&ucfg);
        if (uerr != ESP_OK) {
            ESP_LOGE(TAG, "UI degraded (headless): %s", esp_err_to_name(uerr));
        }
#if CONFIG_ESP_CUBE_FACE_PERF
        /* 表情计时探针(只观测): splash 摘要/首帧/屏幕自检, 须紧跟 UI 启动 */
        face_perf_boot_attach();
#endif
        /* 亮度: DSI init 只开 5%(防点亮瞬态拉塌 3.3V, drv_display_dsi.c
         * 文件头教训), 计划的"UI 手势调高"从未实装 → 5% 成永久值,
         * 亮环境下屏幕形同全黑(2026-11-12 用户实测)。UI 稳定后提到
         * 80%(QSPI 后端同值长期验证; 深色 UI 像素电流低, 无供电风险) */
        drv_display_set_brightness(80);
        /* M4 手势真机通道: CST820 触摸(与 ES8311 共 I2C0; 失败降级 CLI) */
        ui_port_touch_start();
    } else {
        ESP_LOGE(TAG, "display init failed(%s), audio-only mode", esp_err_to_name(derr));
    }
#endif

#if CONFIG_ESP_CUBE_ENABLE_VOICE && CONFIG_IDF_TARGET_ESP32P4
    /* P4: WakeNet 在 WiFi 之后初始化(Kaleido V-45 铁律: wakenet 必须
     * 后于 wifi; P4 内部 RAM 充裕, 无 S31 的模型解密挤压问题) */
    esp_err_t verr = app_voice_start();
    if (verr != ESP_OK) {
        ESP_LOGE(TAG, "voice degraded: %s", esp_err_to_name(verr));
    }
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

    /* ---- 6. 演示观察者 + 串口 CLI ---- */
    cube_event_subscribe(demo_evt_cb, NULL);
/* §52 v4: CLI 为调试通道, 创建失败降级不 abort(板上实测: LVGL 抢占
         * 内部块后 CLI 栈分配失败曾致 abort 循环) */
        if (cli_start() != ESP_OK) {
            ESP_LOGE(TAG, "cli degraded: no console");
        }

    ESP_LOGI(TAG, "startup complete, free heap=%u / psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    mem_budget_audit("boot");   /* docs/22 §3E: 稳态水位对账(FAIL 只告警) */
}
