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
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "cube_act.h"
#include "act_gate.h"
#include "app_bt.h"
#include "app_assist.h"
#include "app_storage.h"
#include <stdlib.h>
#include "app_ui.h"
#include "drv_display.h"
#include "drv_audio.h"
#include "drv_touch.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"    /* xTaskCreateWithCaps(faceload PSRAM 栈, docs/22 §3B) */
/* lv_async 经 lvgl.h(LV_USE_OS=0 时 misc/lv_async 仍编入; 见 lvgl.h 汇总头) */
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
#include <stdio.h>
#include <sys/stat.h>
#include "esp_efc_codec.h"
#include "esp_heap_caps.h"
#include "lv_efc.h"
#include "ui_face_fmt.h"
#endif

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

/* ---- 助理快照刷新(事件上下文: 静态缓冲拷贝, 短平快) ---- */
static void assist_refresh(void)
{
    static assist_item_t items[ASSIST_MAX_ITEMS];
    int n = app_assist_list(items, ASSIST_MAX_ITEMS);
    int memos = 0, reminds = 0;
    int64_t soonest = 0;
    char soonest_txt[UI_STR_LEN] = "";
    int64_t now = app_assist_now();
    for (int i = 0; i < n; i++) {
        if (items[i].due > 0) {
            reminds++;
            if (soonest == 0 || items[i].due < soonest) {
                soonest = items[i].due;
                strlcpy(soonest_txt, items[i].text, sizeof(soonest_txt));
            }
        } else {
            memos++;
        }
    }
    s_model.memo_cnt = (uint8_t)memos;
    s_model.remind_cnt = (uint8_t)reminds;
    model_set_str(s_model.next_remind, soonest_txt,
                  sizeof(s_model.next_remind));
    s_model.next_remind_in_min = soonest > 0
        ? (int32_t)((soonest - now + 59) / 60) : -1;
}

/* ---- 1s 时钟拍(int 字段天然原子; esp_timer 任务上下文) ---- */
/* ---- 灵魂 2.0 face 执行器状态(底色记忆 + 请求记账 + 瞬态回拉) ---- */
static char s_face_bg[24] = "smile_static";  /* 当前底色段名(默认=内置兜底段; 审计修复: neutral 非有效段名) */
static volatile char s_face_req[24];          /* 待加载段(tick 消费) */
static volatile bool s_face_req_pending;
static volatile int64_t s_face_revert_ms;     /* >0 = 瞬态回拉时刻 */

/* tick 提交(异步纪律: timer/总线回调都不做阻塞 SD 读;
 * ui_port_face_req = strdup+faceload 任务, 16KB 栈) */
static void face_req_consume(void)
{
    extern esp_err_t ui_port_face_req(const char *rel);
    if (s_face_req_pending) {
        s_face_req_pending = false;
        ui_port_face_req((const char *)s_face_req);
    }
    if (s_face_revert_ms > 0 &&
        esp_timer_get_time() / 1000 >= s_face_revert_ms) {
        s_face_revert_ms = 0;
        ui_port_face_req(s_face_bg);           /* 回底色 */
    }
}

static void port_tick_cb(void *arg)
{
    face_req_consume();                       /* 灵魂 face 请求(异步提交) */
    s_model.sd_ok = app_storage_ok();         /* T18: SD 状态(1Hz 轮询) */
    time_t t = time(NULL);
    if (t > 1000000000LL) {
        struct tm tmv;
        localtime_r(&t, &tmv);
        s_model.time_synced = true;
        s_model.hour = (uint8_t)tmv.tm_hour;
        s_model.min = (uint8_t)tmv.tm_min;
        s_model.mon = (uint8_t)(tmv.tm_mon + 1);
        s_model.day = (uint8_t)tmv.tm_mday;
        s_model.wday = (uint8_t)tmv.tm_wday;
        if (s_model.next_remind_in_min > 0) {
            s_model.next_remind_in_min--;       /* 粗倒计时(事件来时校准) */
        }
    }
}

/* ---- 事件回调(发布者任务上下文) ---- */
static void face_req_consume(void);    /* 灵魂 face 状态(后部定义) */
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
        ESP_LOGI(TAG, "ui: wifi up (T18 overlay off)");
        break;
    case CUBE_EVT_WIFI_DISCONNECTED:
        s_model.wifi_up = false;
        ESP_LOGW(TAG, "ui: wifi down (T18 overlay on)");
        break;
    case CUBE_EVT_AI_STATE:
        if (data != NULL) s_model.ai_state = *(const int *)data;
        break;
    case CUBE_EVT_AI_TEXT:
        if (data != NULL) model_set_str(s_model.ai_text, (const char *)data,
                                        sizeof(s_model.ai_text));
        break;
    case CUBE_EVT_TIME_SYNCED:
        s_model.time_synced = true;
        assist_refresh();
        break;
    case CUBE_EVT_ASSIST_CHANGED:
    case CUBE_EVT_REMINDER_FIRED:
        assist_refresh();                   /* 计数/下条提醒快照重拉 */
        break;
    case CUBE_EVT_SOUL_ACT: {                  /* 灵魂 2.0 face 执行器 */
        /* 总线回调不得阻塞(SD 读+EFC init 重): 只记账, tick 异步执行 */
        const cube_soul_act_t *act = (const cube_soul_act_t *)data;
        if (act == NULL || len < sizeof(*act)) break;
        if (act->kind == 0) {                   /* 底色: 切换并记忆 */
            strlcpy(s_face_bg, act->content, sizeof(s_face_bg));
            strlcpy((char *)s_face_req, act->content, sizeof(s_face_req));
            s_face_req_pending = true;
        } else if (act->kind == 1) {            /* 瞬态: 播完回底色 */
            strlcpy((char *)s_face_req, act->content, sizeof(s_face_req));
            s_face_req_pending = true;
            s_face_revert_ms = esp_timer_get_time() / 1000 + 8000;
        }
        break;
    }
    case CUBE_EVT_SOUL_MOOD: {
        const cube_soul_mood_t *mood =
            (const cube_soul_mood_t *)data;
        if (mood != NULL && len >= sizeof(*mood)) {
            model_set_str(s_model.mood_tag, mood->anim_tag,
                          sizeof(s_model.mood_tag));
        }
        break;
    }
    default:
        break;
    }
}

static void face_sd_evt_cb(cube_evt_id_t id, const void *data, size_t len, void *ctx);

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
    cube_event_subscribe(face_sd_evt_cb, NULL);   /* SD 就绪换脸(仅 SOUL=n 演示) */
    /* 1s 时钟拍(时间/倒计时; M4 助理主页) */
    esp_timer_handle_t tick;
    const esp_timer_create_args_t ta = {
        .name = "uiport_tick", .callback = port_tick_cb,
    };
    if (esp_timer_create(&ta, &tick) == ESP_OK) {
        esp_timer_start_periodic(tick, 1000000);
    }
    ESP_LOGI(TAG, "ui_port ready (stations=%d vol=%d)",
             s_model.station_num, s_model.volume);
    return ESP_OK;
}

/* ================= 触摸(LVGL pointer indev; M4 手势真机通道) ================= */
static esp_lcd_touch_handle_t s_tp;
static int16_t s_last_x, s_last_y;
static bool s_tp_down;

static void tp_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_tp == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_read_data(s_tp);           /* INT 未接中断: 轮询读 */
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    bool pressed = esp_lcd_touch_get_data(s_tp, pt, &n, 1) == ESP_OK && n > 0;
    if (pressed) {
        s_last_x = pt[0].x;
        s_last_y = pt[0].y;
        /* 按下边沿发触摸事件(B-6a 喂点: soul sm_event TOUCH 桥;
         * 30fps 轮询去抖=按住只发一次) */
        if (!s_tp_down)
            cube_event_publish(CUBE_EVT_TOUCH, NULL, 0);
        s_tp_down = true;
    } else if (s_tp_down) {
        s_tp_down = false;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t ui_port_touch_start(void)
{
    esp_err_t err = drv_touch_start(drv_audio_i2c_bus());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch degraded: %s", esp_err_to_name(err));
        return err;                          /* UI 无触摸仍完整(面板 CLI 通道) */
    }
    s_tp = drv_touch_handle();
    if (!lvgl_port_lock(100)) return ESP_ERR_TIMEOUT;
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, tp_read_cb);
    lv_indev_set_display(in, lv_display_get_default());
    lv_indev_set_mode(in, LV_INDEV_MODE_TIMER);
    lv_timer_set_period(lv_indev_get_read_timer(in), 16);  /* 60Hz 跟手 */
    lvgl_port_unlock();
    ESP_LOGI(TAG, "touch indev up");
    return ESP_OK;
}

/* 面板命令(CLI/语音, 无触摸期入口; 单写者纪律仍在本文件) */
void ui_port_panel_req(int which)
{
    s_model.panel_req = which;
}

/* ---- 表情素材热载(SD face/<名>.efc; design B3, streamlined §三: 5 个拒绝码) ----
 * 任务上下文执行, 只有 set_src_data 与挂起/帧速调用持 LVGL 锁:
 *   1. 等启动页结束(50ms 轮询, 最长 5s);
 *   2. 读文件进 PSRAM: 先 stat 判大小(> 262144 → TOO_LARGE), 读后再核一次;
 *   3. 预检(解码器, 不持锁): BAD_FILE、NOT_460、BAD_FORMAT、MOSTLY_KEYED → 保留当前表情;
 *   4. 持锁: set_src_data → set_frame_delay(50) → set_loop_enabled → ui_face_set_running,
 *      再核对发布的 cf 与尺寸; 核对失败或解码器拒绝 → 恢复内置 smile_static 并告警;
 *   5. 释放文件缓冲(lv_efc 在 set_src_data 内复制了 blob)。 */
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
#define FACE_HOT_MAX_BYTES       262144u   /* TOO_LARGE 上限 */
#define FACE_HOT_W               460u
#define FACE_HOT_H               460u
#define FACE_HOT_DELAY_MS        50u       /* SD 热载帧间隔 */
#define FACE_HOT_SPLASH_WAIT_MS  5000u
#define FACE_HOT_POLL_MS         50u
#define FACE_HOT_KEYED_PERMILLE  990u      /* MOSTLY_KEYED: 第 0 帧键占比 ≥ 990‰ */
#define FACE_HOT_LOCK_MS         500u

extern lv_obj_t *ui_face_anim_obj(void);                  /* ui_widget.c */
extern void ui_face_set_running(lv_obj_t *obj);           /* ui_widget.c */
extern void ui_face_log_loaded(lv_obj_t *obj, const char *label);  /* ui_widget.c */
extern bool ui_face_load_builtin(const char *name);       /* ui_widget.c */

typedef enum {
    FACE_HOT_OK = 0,
    FACE_HOT_TOO_LARGE,
    FACE_HOT_BAD_FILE,
    FACE_HOT_NOT_460,
    FACE_HOT_BAD_FORMAT,
    FACE_HOT_MOSTLY_KEYED,
} face_hot_reason_t;

static const char *face_hot_reason_str(face_hot_reason_t r)
{
    switch (r) {
    case FACE_HOT_TOO_LARGE:    return "TOO_LARGE";
    case FACE_HOT_BAD_FILE:     return "BAD_FILE";
    case FACE_HOT_NOT_460:      return "NOT_460";
    case FACE_HOT_BAD_FORMAT:   return "BAD_FORMAT";
    case FACE_HOT_MOSTLY_KEYED: return "MOSTLY_KEYED";
    default:                    return "OK";
    }
}

/* 等启动页结束; 超时后仍加载(运行态由 ui_face_set_running 保持暂停, 页面结束时恢复) */
static void face_hot_wait_splash(void)
{
    const int64_t t_end = esp_timer_get_time() + (int64_t)FACE_HOT_SPLASH_WAIT_MS * 1000;
    while (app_ui_splash_active()) {
        if (esp_timer_get_time() >= t_end) {
            ESP_LOGW(TAG, "face hot-load: splash still active after %u ms, loading anyway",
                     (unsigned)FACE_HOT_SPLASH_WAIT_MS);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(FACE_HOT_POLL_MS));
    }
}

/* 读 face/<名>.efc 进 PSRAM 缓冲(名称已校验为 [a-z0-9_-]{1,15}) */
static face_hot_reason_t face_hot_read(const char *name, uint8_t **out, size_t *out_len)
{
    char rel[32];
    snprintf(rel, sizeof(rel), "face/%s.efc", name);
    char path[96];
    app_storage_path(path, sizeof(path), rel);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return FACE_HOT_BAD_FILE;
    }
    if ((uint64_t)st.st_size > FACE_HOT_MAX_BYTES) return FACE_HOT_TOO_LARGE;
    size_t len = 0;
    char *buf = app_storage_read(rel, &len);     /* 堆(SPIRAM 优先), 调用方 free */
    if (buf == NULL || len == 0) {
        free(buf);
        return FACE_HOT_BAD_FILE;
    }
    if (len > FACE_HOT_MAX_BYTES) {
        free(buf);
        return FACE_HOT_TOO_LARGE;
    }
    *out = (uint8_t *)buf;
    *out_len = len;
    return FACE_HOT_OK;
}

/* 预检(不持锁, 解码器只读): EFC 头、尺寸、第 0 帧颜色键与 alpha0(≥990‰ 拒绝)。
 * 解码用的 634,800 字节 PSRAM 暂存在返回前释放。 */
static face_hot_reason_t face_hot_precheck(const uint8_t *blob, size_t len)
{
    face_hot_reason_t why = FACE_HOT_OK;
    esp_efc_dec_handle_t dec = NULL;
    esp_efc_dec_info_t info;
    uint8_t *stage = NULL;
    if (len < 8 || memcmp(blob, "EFC1", 4) != 0 ||
        esp_efc_dec_open(blob, len, &dec) != ESP_EFC_OK ||
        esp_efc_dec_get_info(dec, &info) != ESP_EFC_OK) {
        why = FACE_HOT_BAD_FILE;
    } else if (info.width != (int)FACE_HOT_W || info.height != (int)FACE_HOT_H) {
        why = FACE_HOT_NOT_460;
    } else {
        /* 颜色平面 w·h·2 + alpha 平面 w·h, 与 lv_efc 发布布局一致 */
        const size_t stage_bytes = (size_t)FACE_HOT_W * FACE_HOT_H * 3u;
        stage = heap_caps_malloc(stage_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stage == NULL ||
            esp_efc_dec_frame_with_alpha(dec, 0, stage) != ESP_EFC_OK ||
            esp_efc_dec_get_info(dec, &info) != ESP_EFC_OK) {
            why = FACE_HOT_BAD_FILE;
        } else if (info.color_key_index < 0) {
            /* 无颜色键: lv_efc 只发布 RGB565, 不是 RGB565A8 */
            why = FACE_HOT_BAD_FORMAT;
        } else {
            const int pm = ui_face_alpha0_permille(stage, stage_bytes, FACE_HOT_W,
                                                   FACE_HOT_H, FACE_HOT_W * 2u);
            if (pm < 0) {
                why = FACE_HOT_BAD_FILE;
            } else if ((uint32_t)pm >= FACE_HOT_KEYED_PERMILLE) {
                why = FACE_HOT_MOSTLY_KEYED;
            }
        }
    }
    heap_caps_free(stage);
    if (dec != NULL) esp_efc_dec_close(dec);
    return why;
}

/* ---- §52 v5: EFC 解码收拢到 taskLVGL ----
 * esp_efc 解码(esp_efc_dec_frame / lv_efc_set_src_data 同步首帧)含 PIE
 * 指令; PIE 上下文保存区 carve 在执行任务自己的栈内, 而 PIE 存储写不了
 * LP SRAM(RTCRAM) → 解码只允许发生在 taskLVGL(DMA caps 栈)。任何其他
 * 任务(本文件的 ui_port 任务/CLI)触发的加载/预检都经本 helper 转投
 * LVGL 任务执行, 调用方阻塞等结果(锁语义不变: cb 运行时 esp_lvgl_port
 * 已持 LVGL 锁)。超时返回 false(上下文堆分配, 由 cb 兜底释放, 不悬垂)。 */
typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} lvexec_t;

static void lvexec_cb(void *p)
{
    lvexec_t *e = p;
    e->fn(e->arg);
    xSemaphoreGive(e->done);
}

bool ui_face_exec_in_lvgl(void (*fn)(void *), void *arg, uint32_t timeout_ms)
{
    lvexec_t *e = malloc(sizeof(*e));
    if (e == NULL) return false;
    e->fn = fn;
    e->arg = arg;
    e->done = xSemaphoreCreateBinary();
    if (e->done == NULL) { free(e); return false; }
    if (!lvgl_port_lock(FACE_HOT_LOCK_MS)) {
        ESP_LOGW(TAG, "face reject name=- reason=LOCK_TIMEOUT keep=current");
        vSemaphoreDelete(e->done);
        free(e);
        return false;
    }
    lv_async_call(lvexec_cb, e);
    lvgl_port_unlock();
    if (xSemaphoreTake(e->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* LVGL 任务卡死等罕见路径: 堆上下文留给 cb 兜底释放, 记泄漏 */
        ESP_LOGE(TAG, "exec_in_lvgl timeout (ctx leaked)");
        return false;
    }
    vSemaphoreDelete(e->done);
    free(e);
    return true;
}

/* 热载应用段(在 taskLVGL 上下文, LVGL 锁已被 esp_lvgl_port 持有):
 * 预检(含解码=PIE)+加载+发布核对+失败回退, 全部留在 LVGL 任务栈 */
typedef struct {
    const char *name;
    uint8_t *blob;
    size_t len;
    esp_err_t err;
} hot_apply_t;

static void hot_apply_cb(void *p)
{
    hot_apply_t *c = p;
    face_hot_reason_t why = face_hot_precheck(c->blob, c->len);
    if (why != FACE_HOT_OK) {
        ESP_LOGW(TAG, "face reject name=%s reason=%s keep=current", c->name,
                 face_hot_reason_str(why));
        c->err = ESP_FAIL;
        return;
    }
    lv_obj_t *obj = ui_face_anim_obj();
    if (obj == NULL) {
        c->err = ESP_ERR_INVALID_STATE;
        return;
    }
    char label[24];
    snprintf(label, sizeof(label), "sd:%s", c->name);
    lv_efc_set_src_data(obj, c->blob, c->len);
    lv_efc_set_frame_delay(obj, FACE_HOT_DELAY_MS);
    lv_efc_set_loop_enabled(obj, true);
    ui_face_set_running(obj);
    /* 加载后核对发布的第 0 帧; 旧片段已被 set_src 释放, 失败只能回退内置片段 */
    const lv_image_dsc_t *dsc = lv_efc_is_loaded(obj)
        ? (const lv_image_dsc_t *)lv_image_get_src(obj) : NULL;
    face_hot_reason_t post = FACE_HOT_OK;
    if (dsc == NULL) {
        post = FACE_HOT_BAD_FILE;
    } else if (dsc->header.cf != LV_COLOR_FORMAT_RGB565A8) {
        post = FACE_HOT_BAD_FORMAT;
    } else if (dsc->header.w != FACE_HOT_W || dsc->header.h != FACE_HOT_H) {
        post = FACE_HOT_NOT_460;
    }
    if (post == FACE_HOT_OK) {
        ui_face_log_loaded(obj, label);      /* face clip=sd:<名> … */
    } else {
        ESP_LOGW(TAG, "face reject name=%s reason=%s keep=smile_static", c->name,
                 face_hot_reason_str(post));
        ui_face_load_builtin("smile_static");
        c->err = ESP_FAIL;
    }
}

static esp_err_t face_hot_load(const char *name)
{
    if (!ui_face_name_ok(name)) {
        ESP_LOGW(TAG, "face reject name=invalid reason=BAD_FILE keep=current");
        return ESP_ERR_INVALID_ARG;
    }
    face_hot_wait_splash();
    uint8_t *blob = NULL;
    size_t len = 0;
    face_hot_reason_t why = face_hot_read(name, &blob, &len);   /* 纯文件读, 无 PIE */
    if (why != FACE_HOT_OK) {
        ESP_LOGW(TAG, "face reject name=%s reason=%s keep=current", name,
                 face_hot_reason_str(why));
        free(blob);
        return ESP_FAIL;
    }
    hot_apply_t c = { name, blob, len, ESP_OK };
    bool ran = ui_face_exec_in_lvgl(hot_apply_cb, &c, FACE_HOT_LOCK_MS + 2000);
    free(blob);             /* lv_efc_set_src_data 复制 blob, 读缓冲始终由本函数释放 */
    if (!ran) return ESP_ERR_TIMEOUT;
    return c.err;
}

/* CLI/SD 热载入口(任务上下文): name 为 face/ 下不含后缀的名称 */
esp_err_t ui_port_face_load(const char *rel)
{
    return face_hot_load(rel);
}

/* 台调 mood(CLI uimood, 任务 31.4): 经单写者接口改快照 mood_tag, 只接受固定词表。
 * ui_face_apply() 在下一个 30fps 同步周期按 mood 映射切片段。 */
esp_err_t ui_port_mood_req(const char *tag)
{
    const char *v = ui_face_mood_vocab(tag);
    if (strcmp(v, "none") == 0 || strcmp(v, "other") == 0) return ESP_ERR_INVALID_ARG;
    model_set_str(s_model.mood_tag, v, sizeof(s_model.mood_tag));
    return ESP_OK;
}

static void face_load_task(void *arg);   /* 前向(定义在其后) */

/* CLI face 命令入口: 异步化(CLI 任务栈浅, EFC 解析深链同步调会溢出) */
esp_err_t ui_port_face_req(const char *rel)
{
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
    char *r = strdup(rel);
    if (r == NULL) return ESP_ERR_NO_MEM;
    /* 16KB 瞬态栈走 PSRAM(docs/22 §3B): 本任务纯文件读+async 交接,
     * 无 PIE/无 flash 写(PSRAM 栈禁 flash-cache 关断窗口, pairsrv 批次
     * 20 教训), 是内部 SRAM 最大瞬态挤占源之一 */
    if (xTaskCreateWithCaps(face_load_task, "faceload", 16384, r, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(r);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    (void)rel;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void face_load_task(void *arg)
{
    char *rel = (char *)arg;
    ui_port_face_load(rel);
    free(rel);
    vTaskDeleteWithCaps(NULL);    /* 栈在 PSRAM(WithCaps 须配对删除) */
}
#endif

/* SD 就绪 → 演示换脸(SOUL=n 演示模式: 热载 face/leisure.efc, 表内名,
 * 卡上无 face/ 素材则 keep-current)。A-2 收窄: 旧 face/idle.efc 开机覆盖
 * 槽为 23 段表外名(素材无处存在, 每启动必 BAD_FILE 噪声)且与 B-8 表情
 * 双权威冲突——SOUL=y 时表情唯一权威=soul ACT 路径, 本演示退役。 */
static void face_sd_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                           void *ctx)
{
    (void)data; (void)len; (void)ctx;
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM && !CONFIG_ESP_CUBE_ENABLE_SOUL
    if (id != CUBE_EVT_SD_READY) return;
    char *rel = strdup("leisure");
    if (rel != NULL) {
        if (xTaskCreateWithCaps(face_load_task, "faceload", 16384, rel, 5,
                                NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            free(rel);
        }
    }
#endif
}

/* ---- ui_model.h 契约实现 ---- */
const ui_model_t *ui_model(void)
{
    return &s_model;
}

/* 豁免声明(T13): 音量/亮度为连续属性调节, 非离散动作请求 —— 不产生
 * 动作回执语义, 保留 R0 物理直控直调(docs/17 §15.4 UIViewAdapter);
 * 离散动作(播放/停止/切台)已迁统一门, 见下方 ui_ctl_* */
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
    uint32_t id = 0;
    if (s_model.player == UI_PLAYER_PLAYING ||
        s_model.player == UI_PLAYER_CONNECTING) {
        /* 触摸停止 = 用户停止: 锁存 + 经统一门(docs/17 §5.5) */
        act_gate_user_stop_audio("touch_stop");
        act_gate_submit("music.stop", NULL, 0, CACT_ORIGIN_PHYSICAL_USER, &id);
        act_gate_pump();
    } else {
        act_gate_submit("radio.play", NULL, 0, CACT_ORIGIN_PHYSICAL_USER, &id);
        act_gate_pump();
        if (act_gate_started(id)) act_gate_resume_audio();
    }
}

void ui_ctl_switch_station(int dir)
{
    if (s_model.source == UI_SRC_BT) return; /* BT 接管期不切台 */
    int n = s_model.station_num > 0 ? s_model.station_num : 1;
    int idx = s_model.station_idx;
    idx = ((idx + dir) % n + n) % n;
    uint8_t param[16];
    memcpy(param, "station", 8);
    param[8] = (uint8_t)('0' + idx);
    param[9] = 0;
    uint32_t id = 0;
    if (act_gate_submit("radio.play", param, 10,
                        CACT_ORIGIN_PHYSICAL_USER, &id) == CACT_RES_OK) {
        act_gate_pump();
        if (act_gate_started(id)) act_gate_resume_audio();
    }
}

void ui_ctl_select_station(int idx)
{
    if (idx < 0 || idx >= s_model.station_num) return;
    uint8_t param[16];
    memcpy(param, "station", 8);
    param[8] = (uint8_t)('0' + idx);
    param[9] = 0;
    uint32_t id = 0;
    if (act_gate_submit("radio.play", param, 10,
                        CACT_ORIGIN_PHYSICAL_USER, &id) == CACT_RES_OK) {
        act_gate_pump();
        if (act_gate_started(id)) act_gate_resume_audio();
    }
}

int ui_audio_take_pcm(int16_t *stereo, int frames)
{
    return audio_mixer_take_spectrum(stereo, frames) ? frames : 0;
}

#if !CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
/* 无表情动画: 表情热载桩 —— 灵魂 face 行动链编译面完整 */
esp_err_t ui_port_face_req(const char *rel)
{
    (void)rel;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
