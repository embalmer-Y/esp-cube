/*
 * app_prov.c —— SoftAP 首配窗口实现(P3a; docs/17 §9.1 / docs/18 v2 P3a)
 *
 * 协议: espressif/network_provisioning 1.4.0(protocomm SoftAP + Security2
 *   = SRP-6a 3072-bit + AES-GCM, 与 pair 组件同族同群)。SoftAP 本体为
 *   open(service_key=NULL), 安全都压在 SRP 层 —— 与官方 ESP Provisioning
 *   app 的标准姿势一致。
 * PIN: 每次开窗随机 6 位(esp_srp_gen_salt_verifier 头注原生支持"设备
 *   屏显 PIN"模式), 低熵顾虑由 SRP 在线证明 + 限时窗口缓解(v2 二维码
 *   + 高熵秘密升级路线见 docs/17 §9.2 注记)。
 * 凭据持久化: 成功路径 manager 经 esp_wifi_set_config(FLASH) 落 C5 NVS
 *   (esp_wifi_remote 透传, P4 侧 set_storage 被静默吸收); 开机由
 *   app_net 优先沿用既有配置(get_config 非空即不注入 Kconfig 凭据)。
 * 任务: 瞬态 PSRAM WithCaps(cube_tasks.h; 窗口期无 P4 侧 flash 写路径,
 *   NVS 写发生在 C5)。
 */
#include "app_prov.h"
#include "app_net.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_srp.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

#include "cube_tasks.h"

static const char *TAG = "prov";

/* ESP Provisioning app Security2 的默认用户名(客户端侧约定) */
#define PROV_USERNAME   "wifiprov"
#define PROV_PIN_LEN    6

/* 事件组: 窗口任务与 manager 回调/CLI 的会合点 */
#define EV_CRED_OK    BIT0    /* NETWORK_PROV_WIFI_CRED_SUCCESS */
#define EV_CRED_FAIL  BIT1    /* NETWORK_PROV_WIFI_CRED_FAIL(窗口保留, app 可重试) */
#define EV_END        BIT2    /* NETWORK_PROV_END(服务已停) */
#define EV_STOP_REQ   BIT3    /* CLI prov stop */

typedef struct {
    char ssid[33];
    char pin[PROV_PIN_LEN + 1];
    char *salt;             /* esp_srp 堆产物, 窗口期须存活(示例注记) */
    char *verifier;
    int verifier_len;
    int timeout_ms;
    bool mgr_inited;
} prov_ctx_t;

static volatile bool s_active;          /* 窗口进行中(供 CLI status) */
static EventGroupHandle_t s_ev;         /* 文件级: CLI stop 可达 */

/* ---- manager 应用回调(manager 上下文: 只置位+日志, 不做重活) ---- */
static void prov_evt_cb(void *ud, network_prov_cb_event_t event, void *event_data)
{
    (void)ud;
    if (s_ev == NULL) {
        return;
    }
    switch (event) {
    case NETWORK_PROV_WIFI_CRED_RECV: {
        /* event_data = wifi_sta_config_t*(仅 SSID 可打, 密码不入日志) */
        const wifi_sta_config_t *sta = (const wifi_sta_config_t *)event_data;
        char ssid[33];
        memcpy(ssid, sta->ssid, sizeof(ssid) - 1);
        ssid[32] = 0;
        ESP_LOGI(TAG, "creds received for \"%s\"", ssid);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL:
        ESP_LOGW(TAG, "connect failed (reason in app), 可在手机端重试");
        xEventGroupSetBits(s_ev, EV_CRED_FAIL);
        break;
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "STA connected with provisioned creds");
        xEventGroupSetBits(s_ev, EV_CRED_OK);
        break;
    case NETWORK_PROV_END:
        xEventGroupSetBits(s_ev, EV_END);
        break;
    default:
        break;
    }
}

/* ---- AP 名: Cube-<MAC 尾 3 字节>(与官方例程 PROV_xxx 同构) ---- */
static void make_ssid(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        snprintf(out, cap, "Cube-000000");
        return;
    }
    snprintf(out, cap, "Cube-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

/* ---- 回落 STA: 与 app_net_start 同判(Kconfig 凭据仅无配置时注入) ----
 * ok=true: 成功路径 STA 已连(CRED_SUCCESS 即已连上新 AP), 只切回 STA 模式
 *   不 stop/start(免闪断; CP 侧仅目标≠STA 时才断连)。
 * ok=false: 射频状态未知(窗口期/APSTA), stop→STA→起网走自动重连。 */
static void restore_sta(bool ok)
{
    if (ok) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        app_net_prov_guard(false);
        return;
    }
    (void)esp_wifi_stop();          /* 从 APSTA 归一(未启动时返回错, 无害) */
    (void)esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t cur = {0};
    (void)esp_wifi_get_config(WIFI_IF_STA, &cur);
    if (cur.sta.ssid[0] == '\0' && strlen(CONFIG_ESP_CUBE_WIFI_SSID) > 0) {
        wifi_config_t cfg = {
            .sta = {
                .ssid = CONFIG_ESP_CUBE_WIFI_SSID,
                .password = CONFIG_ESP_CUBE_WIFI_PASSWORD,
            },
        };
        (void)esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    /* 先恢复重连语义再起网: STA_START 自动 connect 走既有 handler */
    app_net_prov_guard(false);
    (void)esp_wifi_start();
}

static void prov_window_task(void *arg)
{
    prov_ctx_t *c = (prov_ctx_t *)arg;
    bool ok = false;                    /* goto teardown 早退路径可达 */

    /* 1. 随机 PIN + 现算 salt/verifier(3072-bit 群大数运算) */
    int pin_val = 100000 + (int)(esp_random() % 900000);
    snprintf(c->pin, sizeof(c->pin), "%d", pin_val);
    if (esp_srp_gen_salt_verifier(PROV_USERNAME, strlen(PROV_USERNAME),
                                  c->pin, strlen(c->pin),
                                  &c->salt, 16,
                                  &c->verifier, &c->verifier_len) != ESP_OK) {
        ESP_LOGE(TAG, "srp salt/verifier gen failed");
        goto teardown;
    }

    make_ssid(c->ssid, sizeof(c->ssid));

    /* 2. 抑制 STA 自动重连(先立旗再停网, 断开事件不再排重连定时器) */
    app_net_prov_guard(true);
    (void)esp_wifi_stop();          /* manager 需从停态起网(官方例程口径) */

    /* 3. manager 起服务: scheme_softap 内部切 APSTA + 配 AP + 起 httpd */
    network_prov_mgr_config_t cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = { .event_cb = prov_evt_cb, .user_data = c },
        .network_prov_wifi_conn_cfg = { .wifi_conn_attempts = 1 },
    };
    esp_err_t err = network_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mgr init: %s", esp_err_to_name(err));
        goto teardown;
    }
    c->mgr_inited = true;

    network_prov_security2_params_t sec2 = {
        .salt = c->salt,
        .salt_len = 16,                 /* designated init 缺省=0, 必须显式 */
        .verifier = c->verifier,
        .verifier_len = (uint16_t)c->verifier_len,
    };
    err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, &sec2,
                                              c->ssid, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start provisioning: %s", esp_err_to_name(err));
        goto teardown;
    }

    printf("=== P3a 配网窗口 ===\n"
           "  SoftAP: %s (open, 安全在 SRP 层)\n"
           "  PIN:    %s\n"
           "  QR:     {\"ver\":\"v1\",\"name\":\"%s\",\"transport\":\"softap\","
           "\"pop\":\"%s\"}\n"
           "  窗口:   %ds | 手机连 AP → ESP Provisioning app(Security2, "
           "username=%s) 输 PIN → 投递 WiFi 凭据\n",
           c->ssid, c->pin, c->ssid, c->pin, c->timeout_ms / 1000,
           PROV_USERNAME);

    /* 4. 等待: 成功即收窗; 失败保留窗口(app 端可重试); 超时/手动收窗 */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(c->timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            ESP_LOGW(TAG, "window timeout");
            break;
        }
        EventBits_t b = xEventGroupWaitBits(s_ev,
                                            EV_CRED_OK | EV_CRED_FAIL | EV_END | EV_STOP_REQ,
                                            pdTRUE, pdFALSE,
                                            deadline - now);
        if (b & EV_STOP_REQ) {
            ESP_LOGI(TAG, "window closed by request");
            break;
        }
        if (b & EV_CRED_OK) {
            ok = true;
            break;
        }
        if (b & EV_CRED_FAIL) {
            ESP_LOGI(TAG, "窗口保留, 手机端修正凭据后可重试");
            continue;
        }
        if (b & EV_END) {
            ESP_LOGW(TAG, "service ended unexpectedly");
            break;
        }
    }

teardown:
    if (c->mgr_inited) {
        network_prov_mgr_stop_provisioning();
        for (int i = 0; i < 50 && !network_prov_mgr_is_sm_idle(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        network_prov_mgr_deinit();
    }
    if (c->salt) {
        free(c->salt);
    }
    if (c->verifier) {
        free(c->verifier);
    }
    printf("=== 配网窗口关闭: %s ===\n",
           ok ? "成功, 凭据已持久化(C5 NVS), 保持 STA 连接"
              : "未完成, 回落 STA(原凭据)");
    restore_sta(ok);

    if (s_ev) {
        vEventGroupDelete(s_ev);
        s_ev = NULL;
    }
    free(c);
    s_active = false;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t app_prov_start(int timeout_sec)
{
    if (!app_net_ready()) {
        ESP_LOGE(TAG, "app_net not started");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    prov_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->timeout_ms = (timeout_sec > 0 ? timeout_sec : 180) * 1000;
    s_ev = xEventGroupCreate();
    if (s_ev == NULL) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    s_active = true;
    if (xTaskCreatePinnedToCoreWithCaps(prov_window_task, CUBE_TASK_PROV_NAME,
                                        CUBE_TASK_PROV_STACK, c,
                                        CUBE_TASK_PROV_PRIO, NULL,
                                        CUBE_TASK_PROV_CORE,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        vEventGroupDelete(s_ev);
        s_ev = NULL;
        free(c);
        s_active = false;
        ESP_LOGE(TAG, "window task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_prov_stop(void)
{
    if (!s_active || s_ev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_ev, EV_STOP_REQ);
    return ESP_OK;
}

bool app_prov_active(void)
{
    return s_active;
}
