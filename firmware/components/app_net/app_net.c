/*
 * app_net.c —— WiFi STA 实现
 *
 * 重连策略(工程规范修订): 事件处理器【不得阻塞】——它运行在默认事件循环
 * 任务上, 阻塞会卡住所有 WIFI/IP 事件分发(实测旧实现 vTaskDelay 最长 32s)。
 * 断线后改为 esp_timer 单次定时回调里发起 esp_wifi_connect, 指数退避。
 */
#include "app_net.h"

#include <stdbool.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"

static const char *TAG = "net";

/* 与蓝牙共存: 稍缓的重连节奏, 避免初始化风暴挤占 2.4G */
#define WIFI_RETRY_MS_BASE  2000
#define WIFI_RETRY_MAX_SHIFT 4      /* 最大退避 = 基准 × 2^4 = 32s */

static esp_timer_handle_t s_reconnect_timer;
static volatile int s_retry_count = 0;
static volatile bool s_suspended;  /* BT 模式: WiFi 已停, 重连抑制 */
static volatile bool s_got_ip;     /* STA 已获 IP(供播放器恢复时等待) */
static volatile bool s_inited;

/* esp_timer 回调上下文(定时器任务): 只做一次非阻塞 API 调用 */
static void wifi_reconnect_cb(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();             /* 事件驱动首次连接 */
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
                int shift = s_retry_count > WIFI_RETRY_MAX_SHIFT ? WIFI_RETRY_MAX_SHIFT
                                                             : s_retry_count;
            int delay_ms = WIFI_RETRY_MS_BASE << shift;
            s_retry_count++;
            s_got_ip = false;
            ESP_LOGW(TAG, "disconnected, retry in %dms(#%d)%s", delay_ms, s_retry_count,
                     s_suspended ? " [suspended]" : "");
            cube_event_publish(CUBE_EVT_WIFI_DISCONNECTED, NULL, 0);
            if (s_suspended) {
                break;              /* BT 模式主动停网: 不安排重连 */
            }
            /* 指数退避后重连(定时器回调, 不阻塞事件循环) */
            (void)esp_timer_stop(s_reconnect_timer);   /* 未激活时返回INVALID_STATE, 无害 */
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        s_got_ip = true;
        cube_event_publish(CUBE_EVT_WIFI_CONNECTED, NULL, 0);
    }
}

/* ================= BT 模式射频让渡(M1.8.5) =================
 * 实测(用户听感+pcm flow 数据): WiFi/BT 共存调度下 A2DP 存在
 * ①"劣化积累态"(首次卡顿后持续数秒一卡, 重连才恢复) ②3 米卡顿显著增多。
 * 对策: BT 模式停 WiFi, 蓝牙独占射频; 断开蓝牙后重启 WiFi 并恢复电台。
 * 代价: BT 播放期间无网络功能 —— M2 语音 AI 上线时需重新权衡
 * (届时可做成运行时开关: "音质优先(关WiFi)" vs "AI 优先(共存)")。
 */
esp_err_t app_net_suspend(void)
{
    if (!s_inited || s_suspended) {
        return ESP_OK;
    }
    s_suspended = true;
    (void)esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_wifi_stop();           /* 阻塞 ~百毫秒, 事件由上方抑制 */
    ESP_LOGI(TAG, "wifi suspended for BT (err=%s)", esp_err_to_name(err));
    return err;
}

esp_err_t app_net_resume(void)
{
    if (!s_inited || !s_suspended) {
        return ESP_OK;
    }
    s_suspended = false;
    s_retry_count = 0;
    esp_err_t err = esp_wifi_start();          /* STA_START 事件触发自动连接 */
    ESP_LOGI(TAG, "wifi resumed (err=%s)", esp_err_to_name(err));
    return err;
}

bool app_net_is_up(void)
{
    return s_got_ip;
}

esp_err_t app_net_wait_up(int timeout_ms)
{
    int waited = 0;
    while (!s_got_ip && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_got_ip ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* 事件总线订阅: BT 接管 → 停网; BT 释放 → 复网(播放器再等 IP 后恢复电台) */
static void net_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)ctx;
    if (id != CUBE_EVT_BT_AUDIO_ACTIVE || data == NULL || len != sizeof(bool)) {
        return;                             /* 事件总线全量派发: 必须按 ID 过滤 */
    }
    if (*(const bool *)data) {
        app_net_suspend();
    } else {
        app_net_resume();
    }
}

esp_err_t app_net_start(void)
{
    /* NVS 是 WiFi 依赖(失败仅当首次开机擦除区坏, 非致命则擦除重试) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    }

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_reconnect_cb,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    /* 与 BT 共存: 使用默认静态/动态共存缓冲 */
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "reg wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG, "reg ip evt");

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_ESP_CUBE_WIFI_SSID,
            .password = CONFIG_ESP_CUBE_WIFI_PASSWORD,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");
    /* PS_NONE(M1.8.4 实测结论): PS_MIN_MODEM 的周期性唤醒/DTIM 突发使 BT
     * A2DP 远近距离均匀卡顿(数据流 5s 窗口从 868KB 掉半直至断流 14s)。
     * 恢复 PS_NONE, BT 空口优先改由 main 里 esp_coex_preference_set 解决。 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set ps");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "wifi sta started, connecting to \"%s\"", CONFIG_ESP_CUBE_WIFI_SSID);
    s_inited = true;
    ESP_ERROR_CHECK(cube_event_subscribe(net_evt_cb, NULL));  /* BT 模式自动让渡射频 */
    return ESP_OK;
}
