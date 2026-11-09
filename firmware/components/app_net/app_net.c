/*
 * app_net.c —— WiFi STA 实现
 *
 * 重连策略(工程规范修订): 事件处理器【不得阻塞】——它运行在默认事件循环
 * 任务上, 阻塞会卡住所有 WIFI/IP 事件分发(实测旧实现 vTaskDelay 最长 32s)。
 * 断线后改为 esp_timer 单次定时回调里发起 esp_wifi_connect, 指数退避。
 */
#include "app_net.h"

#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "cubecore.h"

static const char *TAG = "net";

/* 与蓝牙共存: 稍缓的重连节奏, 避免初始化风暴挤占 2.4G */
#define WIFI_RETRY_MS_BASE  2000
#define WIFI_RETRY_MAX_SHIFT 4      /* 最大退避 = 基准 × 2^4 = 32s */

static esp_timer_handle_t s_reconnect_timer;
static volatile int s_retry_count = 0;

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
            ESP_LOGW(TAG, "disconnected, retry in %dms(#%d)", delay_ms, s_retry_count);
            cube_event_publish(CUBE_EVT_WIFI_DISCONNECTED, NULL, 0);
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
        cube_event_publish(CUBE_EVT_WIFI_CONNECTED, NULL, 0);
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
    /* 流媒体场景关闭 modem-sleep: 省电模式会让 TCP 变成间歇突发, 造成播放缓冲频繁耗尽 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set ps");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "wifi sta started, connecting to \"%s\"", CONFIG_ESP_CUBE_WIFI_SSID);
    return ESP_OK;
}
