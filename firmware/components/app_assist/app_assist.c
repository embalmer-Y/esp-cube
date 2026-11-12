/*
 * app_assist.c —— 语音助理本地服务实现(SNTP + NVS 存储 + 提醒调度)
 *
 * 存储布局: NVS 命名空间 "assist"
 *   - "nextid" u32     id 计数器
 *   - "iXXXXXXXX" blob assist_item_t 原样(id 十六进制 8 位, 前缀迭代)
 * RAM 镜像为运行时唯一事实源(互斥保护), NVS 仅持久化 —— 启动加载时
 * 以 blob 内字段为准(不信文件名), nextid 取 max(计数器, 最大id+1)。
 *
 * 调度: 独立低优先任务 1s tick(对齐 Kaleido soul 的节拍粒度):
 *   - WiFi 就绪 → SNTP 启动(仅一次); sync 回调只置标志, 事件发布
 *     与 TZ 设置收口到调度任务(LWIP 定时器上下文不做总线派发);
 *   - 到点提醒(±1s): 移除 → 发布 REMINDER_FIRED → announcer 播报。
 */
#include "app_assist.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"

#include "cubecore.h"

#define ASSIST_MAGIC   0x41535431     /* 'AST1' */
#define ASSIST_NS      "assist"
#define ASSIST_KEY_ID  "nextid"
#define ASSIST_KEY_PFX "i"
#define ASSIST_TASK_MS 1000

static const char *TAG = "assist";

static assist_item_t s_items[ASSIST_MAX_ITEMS];
static int s_count;
static uint32_t s_next_id = 1;
static SemaphoreHandle_t s_lock;

static assist_announcer_t s_announcer;
static volatile bool s_sntp_req;      /* WiFi 就绪 → tick 里启动 */
static volatile bool s_sntp_started;
static volatile bool s_time_synced;   /* sync 回调置位 */

/* ---- SNTP 同步回调(LWIP 定时器上下文: 只置标志) ---- */
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    if (!s_time_synced) {
        s_time_synced = true;
        ESP_LOGI(TAG, "time synced");
    }
}

static void assist_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                          void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WIFI_CONNECTED) {
        s_sntp_req = true;             /* 启动动作挪到调度 tick */
    }
}

/* ---- NVS ---- */
static void assist_key(char *key, size_t cap, uint32_t id)
{
    snprintf(key, cap, ASSIST_KEY_PFX "%08lx", (unsigned long)id);
}

static void nvs_write_item(const assist_item_t *it)
{
    nvs_handle_t h;
    if (nvs_open(ASSIST_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    assist_key(key, sizeof(key), it->id);
    nvs_set_blob(h, key, it, sizeof(*it));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_erase_item(uint32_t id)
{
    nvs_handle_t h;
    if (nvs_open(ASSIST_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    assist_key(key, sizeof(key), id);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

/* 启动加载: 前缀迭代 + nextid 对齐 */
static void load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(ASSIST_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t nid = 0;
        if (nvs_get_u32(h, ASSIST_KEY_ID, &nid) == ESP_OK) {
            s_next_id = nid;
        }
        nvs_close(h);
    }
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(ASSIST_NS, ASSIST_KEY_PFX,
                                   NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (s_count < ASSIST_MAX_ITEMS) {
            nvs_handle_t rh;
            if (nvs_open(ASSIST_NS, NVS_READONLY, &rh) == ESP_OK) {
                assist_item_t item;
                size_t len = sizeof(item);
                if (nvs_get_blob(rh, info.key, &item, &len) == ESP_OK &&
                    len == sizeof(item) && item.magic == ASSIST_MAGIC &&
                    item.text[0] != '\0') {
                    s_items[s_count++] = item;
                    if (item.id >= s_next_id) {
                        s_next_id = item.id + 1;
                    }
                }
                nvs_close(rh);
            }
        } else {
            ESP_LOGW(TAG, "storage overflow on load, drop %s", info.key);
        }
        err = nvs_entry_next(&it);
    }
    /* created 升序(淘汰/列表口径一致) */
    for (int i = 1; i < s_count; i++) {
        assist_item_t k = s_items[i];
        int j = i - 1;
        while (j >= 0 && s_items[j].created > k.created) {
            s_items[j + 1] = s_items[j];
            j--;
        }
        s_items[j + 1] = k;
    }
    ESP_LOGI(TAG, "loaded %d item(s)", s_count);
}

/* ---- 淘汰最旧(容量满时) ---- */
static void evict_oldest_locked(void)
{
    if (s_count < ASSIST_MAX_ITEMS) return;
    uint32_t victim = s_items[0].id;
    memmove(&s_items[0], &s_items[1],
            sizeof(s_items[0]) * (s_count - 1));
    s_count--;
    nvs_erase_item(victim);
    ESP_LOGW(TAG, "full, evicted #%u", (unsigned)victim);
}

/* ---- 调度任务: SNTP 启动 + 时间事件 + 到点派发 ---- */
static void assist_task(void *arg)
{
    (void)arg;
    bool synced_evt_sent = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ASSIST_TASK_MS));

        if (s_sntp_req && !s_sntp_started) {
            s_sntp_req = false;
            s_sntp_started = true;
            /* LWIP_SNTP_MAX_SERVERS=1(项目默认): 单服务器, 阿里云 NTP
             * 国内可靠; 失败由 sync_cb 缺席自然暴露(时间未同步态) */
            esp_sntp_config_t sc =
                ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            sc.wait_for_sync = false;
            sc.sync_cb = sntp_sync_cb;
            esp_netif_sntp_init(&sc);   /* .start=true 自启 */
            ESP_LOGI(TAG, "sntp started");
        }
        if (s_time_synced && !synced_evt_sent) {
            synced_evt_sent = true;
            cube_event_publish(CUBE_EVT_TIME_SYNCED, NULL, 0);
        }

        int64_t now = (int64_t)time(NULL);
        if (now < 1000000000LL) continue;    /* 时钟未同步: 不派发 */

        /* 到点扫描(每 tick ≤4 条, 超量留到下一拍 —— 洪峰防御) */
        assist_item_t fired[4];
        int nf = 0;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50))) {
            for (int i = 0; i < s_count && nf < 4; ) {
                if (s_items[i].due > 0 && s_items[i].due <= now) {
                    fired[nf++] = s_items[i];
                    memmove(&s_items[i], &s_items[i + 1],
                            sizeof(s_items[0]) * (s_count - i - 1));
                    s_count--;
                    nvs_erase_item(fired[nf - 1].id);
                } else {
                    i++;
                }
            }
            xSemaphoreGive(s_lock);
        }
        for (int i = 0; i < nf; i++) {
            cube_reminder_t r = { 0 };
            r.id = fired[i].id;
            r.due = fired[i].due;
            strlcpy(r.text, fired[i].text, sizeof(r.text));
            ESP_LOGI(TAG, "reminder fired #%u: %s",
                     (unsigned)r.id, r.text);
            cube_event_publish(CUBE_EVT_REMINDER_FIRED, &r, sizeof(r));
            cube_event_publish(CUBE_EVT_ASSIST_CHANGED, NULL, 0);
            if (s_announcer) {
                char say[160];
                snprintf(say, sizeof(say), "时间到了，提醒你：%s", r.text);
                s_announcer(say);
            }
        }
    }
}

/* ---- 对外 ---- */
esp_err_t app_assist_init(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;

    /* TZ 先行: 未同步时 mktime/strftime 语义也已确定(UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    load_all();

    ESP_RETURN_ON_ERROR(cube_event_subscribe(assist_evt_cb, NULL),
                        TAG, "evt subscribe");

    if (xTaskCreate(assist_task, "aremind", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_assist_set_announcer(assist_announcer_t fn)
{
    s_announcer = fn;
}

bool app_assist_time_synced(void)
{
    return s_time_synced;
}

int64_t app_assist_now(void)
{
    return (int64_t)time(NULL);
}

bool app_assist_parse_localtime(const char *s, int64_t *out_unix)
{
    if (s == NULL || out_unix == NULL) return false;
    /* 规范化副本: 'T' 分隔符 → 空格(ISO 习惯), 去首尾空白。
     * 板上实测(2026-11-12): sscanf 六 %d 命中 5 个时 %n 不写入,
     * 未初始化 n 让合法的 "2026-11-13 10:00" 被误判无效 —— 改为
     * 精确比对返回个数, n 未写(==0)一律走 fallback */
    char buf[40];
    strlcpy(buf, s, sizeof(buf));
    for (char *p = buf; *p; p++) {
        if (*p == 'T' || *p == '\t') *p = ' ';
    }
    char *b = buf;
    while (*b == ' ') b++;
    char *e = b + strlen(b);
    while (e > b && e[-1] == ' ') *--e = '\0';

    int y, mo, d, h, mi, sec = 0, n = 0;
    int k = sscanf(b, "%d-%d-%d %d:%d:%d%n",
                   &y, &mo, &d, &h, &mi, &sec, &n);
    if (k != 6 || n == 0 || n != (int)strlen(b)) {
        sec = 0; n = 0;
        k = sscanf(b, "%d-%d-%d %d:%d%n", &y, &mo, &d, &h, &mi, &n);
        if (k != 5 || n == 0 || n != (int)strlen(b)) return false;
    }
    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        sec < 0 || sec > 59) {
        return false;
    }
    struct tm tmv = {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = h, .tm_min = mi, .tm_sec = sec,
    };
    time_t t = mktime(&tmv);                    /* TZ=CST-8 本地语义 */
    if (t == (time_t)-1) return false;
    *out_unix = (int64_t)t;
    return true;
}

void app_assist_fmt_speak(int64_t unix, char *out, size_t cap)
{
    time_t t = (time_t)unix;
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, cap, "%d月%d日 %d点%02d分",
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

void app_assist_fmt_compact(int64_t unix, char *out, size_t cap)
{
    time_t t = (time_t)unix;
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, cap, "%02d-%02d %02d:%02d",
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

esp_err_t app_assist_add(const char *text, int64_t due, uint32_t *id_out)
{
    if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (due > 0 && !app_assist_time_synced()) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    evict_oldest_locked();
    assist_item_t *it = &s_items[s_count];
    memset(it, 0, sizeof(*it));
    it->magic = ASSIST_MAGIC;
    it->id = s_next_id++;
    it->due = due;
    it->created = (int64_t)time(NULL);
    strlcpy(it->text, text, sizeof(it->text));
    s_count++;
    nvs_write_item(it);
    /* 计数器独立提交(淘汰路径不回滚, nextid 单调即可) */
    {
        nvs_handle_t h;
        if (nvs_open(ASSIST_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, ASSIST_KEY_ID, s_next_id);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    if (id_out) *id_out = it->id;
    xSemaphoreGive(s_lock);
    cube_event_publish(CUBE_EVT_ASSIST_CHANGED, NULL, 0);
    return ret;
}

esp_err_t app_assist_remove(uint32_t id)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id == id) {
            memmove(&s_items[i], &s_items[i + 1],
                    sizeof(s_items[0]) * (s_count - i - 1));
            s_count--;
            nvs_erase_item(id);
            ret = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (ret == ESP_OK) {
        cube_event_publish(CUBE_EVT_ASSIST_CHANGED, NULL, 0);
    }
    return ret;
}

esp_err_t app_assist_clear(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < s_count; i++) {
        nvs_erase_item(s_items[i].id);
    }
    s_count = 0;
    xSemaphoreGive(s_lock);
    cube_event_publish(CUBE_EVT_ASSIST_CHANGED, NULL, 0);
    return ESP_OK;
}

int app_assist_list(assist_item_t *out, int cap)
{
    if (out == NULL || cap <= 0) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
    int n = s_count < cap ? s_count : cap;
    memcpy(out, s_items, sizeof(s_items[0]) * n);
    xSemaphoreGive(s_lock);
    return n;
}
