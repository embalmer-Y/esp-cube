/*
 * face_perf.c —— 表情逐帧计时探针(L5; CONFIG_ESP_CUBE_FACE_PERF; 阶段 A 只观测)
 *
 * freeze-and-face-blackscreen-fix 任务 31.4(B4)。接口与窗口语义见 face_perf.h。
 *
 * 采集点(LVGL 9.6, 全部在 LVGL 任务内, 时间戳 esp_timer_get_time()):
 *   D   lv_efc 帧定时器回调耗时。遍历 lv_timer_get_next() 找 user_data == 表情对象
 *       的定时器, 经私有头 lv_timer_private.h 取原回调, lv_timer_set_cb() 换成计时
 *       包装(原样调用原回调)。lv_efc 的 next_frame_cb 在未到帧间隔时直接返回, 所以
 *       只把"当前帧号发生变化且 < 总帧数"的调用记为一次解码(D 样本)。D 覆盖解码、
 *       键控准备(alpha 生成)与发布之和。
 *   R/F 显示事件: REFR_START/READY、RENDER_START/READY、FLUSH_START/FINISH、
 *       FLUSH_WAIT_START/FINISH(9.6 均存在, 名称与任务文本一致)。
 *       9.6 的时序是 REFR_START → 布局 → direct 双缓冲同步拷贝(其中含一次
 *       FLUSH_WAIT) → RENDER_START → 绘制 → [FLUSH_WAIT → FLUSH_START →
 *       flush_cb(DSI 在此等 vsync 信号量) → FLUSH_FINISH] → RENDER_READY →
 *       REFR_READY, 即刷屏嵌套在 RENDER_START/READY 之内。为让 T = D + R + F
 *       不重复计数: F = 本周期全部 FLUSH 与 FLUSH_WAIT 区间之和(含 vsync 等待);
 *       R = 周期(REFR_START→REFR_READY)扣除 F(布局 + 同步拷贝 + 绘制);
 *       只统计出现过 RENDER_START→RENDER_READY 的周期。
 *       配对: 每次 D 之后的第一次刷新与该帧配对; 两次 D 之间没有刷新时,
 *       前一个 D 计入 unpaired。
 *   I_refr 相邻两次 REFR_START 的间隔(开机窗 splash 期间)。
 *   I_loop 测量窗内 20ms 探针 lv_timer 的实际触发间隔(不 invalidate)。
 *
 * 开机窗时序:
 *   attach(main, UI 启动后) ── splash 屏 SCREEN_UNLOAD_START(master_timer_cb
 *   置 splash_active=false 后立即调 lv_screen_load_anim; 未挂上时由 REFR_START
 *   轮询 app_ui_splash_active() 兜底) = splash 结束 ── 第一次与表情圆外接
 *   方框相交的 FLUSH_FINISH = face first refresh ── 主屏 SCREEN_LOADED 之后
 *   满 600ms(FP_CHECK_AFTER_US)的第一次"最后一块" FLUSH_FINISH: 此时 buf_act
 *   尚未交换, 即刚送屏的整帧(direct 双缓冲, 整帧有效) → face screen check
 *   (ui_face_lit_screen(), ui_face_fmt.h, 812 点亮度 ≥128 的点数)。
 *   日志统一由低优先级 worker 任务(PSRAM 栈)在开机窗结束后输出, 不在 LVGL
 *   任务里打印, 免得串口写阻塞污染 first refresh 的测量; vTaskGetRunTimeStats
 *   快照在 splash 结束时由 worker 立即采集(PSRAM 缓冲), 晚些输出。
 *
 * 内存: 静态区只有 2 个指针(s_fp、s_efc_orig_cb); 上下文、样本环、报告
 * 汇总、cputop 缓冲、worker 栈/TCB 全部在 PSRAM 堆。显示/对象事件描述符
 * 由 LVGL 堆分配(8+5 个小块), 窗口结束即移除。
 */
#include "face_perf.h"

#if CONFIG_ESP_CUBE_FACE_PERF

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "lv_efc.h"
/* LVGL 私有头: 读 lv_efc 帧定时器原回调(9.6 无公开 getter; lv_efc.c 同样包含) */
#include "src/misc/lv_timer_private.h"
#include "app_ui.h"
#include "ui_model.h"
#include "face_perf_geom.h"
#include "ui_face_fmt.h"
#include "face_perf_stats.h"

_Static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6,
               "face_perf 依赖 LVGL 9.6 的 lv_timer_t 布局与显示事件时序, "
               "升级 LVGL 后复核本文件再改断言");

/* ui_widget.c 表情接口(类型不外泄; 调用须持 LVGL 锁) */
extern lv_obj_t *ui_face_anim_obj(void);
extern bool ui_face_builtin_lookup(const char *name, const uint8_t **start,
                                   const uint8_t **end);
extern bool ui_face_load_builtin(const char *name);
extern void ui_face_reload_mood(void);

static const char TAG[] = "faceperf";

#define FP_PROBE_MS        20u          /* I_loop 探针周期 */
#define FP_FRAME_CAP       1024u        /* 逐帧记录 1024 × 16B */
#define FP_FRAME_NCOL      4u           /* D R F T */
#define FP_IREFR_CAP       1024u        /* splash I_refr(约 75 个/2.4s) */
#define FP_BOOT_TIMEOUT_US (30LL * 1000 * 1000)
#define FP_CHECK_AFTER_US  (600LL * 1000)    /* SCREEN_LOADED 后至少此时长再做屏幕自检 */
#define FP_LOCK_MS         500u
#define FP_WORKER_STACK    4096
#define FP_WORKER_PRIO     2            /* 低于 LVGL(4) */
#define FP_WORKER_CORE     0            /* LVGL 在 core1 */
#define FP_NB_SPLASH       (1u << 0)
#define FP_NB_DONE         (1u << 1)

/* 微秒 → "x.y" 毫秒两个参数(四舍五入到 0.1ms) */
#define FP_MS(us) (uint32_t)(((uint64_t)(us) + 50u) / 1000u), \
                  (uint32_t)((((uint64_t)(us) + 50u) / 100u) % 10u)
#define FP_MSF "%" PRIu32 ".%" PRIu32 "ms"

enum { COL_D = 0, COL_R, COL_F, COL_T };
enum { SUM_D = 0, SUM_R, SUM_F, SUM_T, SUM_ILOOP, SUM_N };
typedef enum { FP_MODE_BOOT = 1, FP_MODE_WIN } fp_mode_t;
typedef enum { BP_SPLASH = 0, BP_WAIT_FIRST, BP_WAIT_CHECK, BP_DONE } fp_phase_t;
typedef enum { WS_RUN = 0, WS_DONE } fp_wstate_t;
typedef enum {
    CHK_NONE = 0, CHK_OK, CHK_NO_BUF, CHK_BAD_CF, CHK_BAD_MODE, CHK_BAD_SIZE,
} fp_chk_t;

typedef struct {
    uint8_t mode;                 /* fp_mode_t */
    bool attached;                /* 钩子在位(仅在 LVGL 锁内改) */
    bool aborted;                 /* 开机窗挂接失败: worker 只清理 */
    bool d_hooked;                /* lv_efc 定时器包装已装上(否则 d 输出 -1) */
    lv_display_t *disp;
    lv_obj_t *anim;               /* face_anim; 删除时置 NULL */
    lv_timer_t *efc_timer;        /* NULL = D 不可测 */
    lv_obj_t *splash_scr;         /* 开机窗订阅的屏幕; 删除时置 NULL */
    lv_obj_t *main_scr;

    /* ---- 逐帧配对(测量窗) ---- */
    bool d_pending, paired, rendering, rendered;
    uint32_t d_us, pair_d, f_acc;
    int64_t cyc_t0, f_t0, w_t0;
    uint32_t d_cnt, d_unpaired, norender;

    /* ---- 开机窗 ---- */
    bool flush_last, flush_face, main_loaded;
    uint8_t phase;                /* fp_phase_t */
    uint8_t chk;                  /* fp_chk_t */
    int64_t t_attach, t_last_refr, t_splash_end, t_first;
    int64_t t_loaded, t_check;    /* SCREEN_LOADED 时刻; 屏幕自检时刻 */
    uint32_t splash_d;
    uint32_t chk_pts, chk_hit, chk_cf;
    int32_t chk_frame;
    TaskHandle_t worker;

    /* ---- 测量窗 ---- */
    uint8_t wstate;               /* fp_wstate_t */
    bool reporting, summarized;
    bool paused0, paused1;        /* 开窗/收窗时 lv_efc 定时器是否暂停(面板) */
    lv_timer_t *probe;
    int64_t probe_last, t_start, t_end, t_stop;
    char clip[16];
    uint32_t delay_ms;
    int ai_state;
    face_perf_done_cb on_done;
    void *done_arg;

    face_perf_ring_t frames, iloop, irefr;
    uint32_t *frames_buf, *iloop_buf, *irefr_buf;
    face_perf_summary_t sum[SUM_N];
} fp_ctx_t;

static fp_ctx_t *s_fp;               /* 当前上下文(PSRAM); 只在 LVGL 锁内改 */
static lv_timer_cb_t s_efc_orig_cb;  /* lv_efc 定时器原回调 */

/* ================= 小工具 ================= */
static inline uint32_t us_u32(int64_t d)
{
    if (d <= 0) return 0u;
    return d >= (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

static void *ps_calloc(size_t n, size_t sz)
{
    return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void ctx_free(fp_ctx_t *c)
{
    if (c == NULL) return;
    heap_caps_free(c->frames_buf);
    heap_caps_free(c->iloop_buf);
    heap_caps_free(c->irefr_buf);
    heap_caps_free(c);
}

/* 与表情圆外接方框相交(face_perf_geom.h 口径) */
static bool area_hits_face(const lv_area_t *a)
{
    return a != NULL &&
           a->x2 >= FACE_GEOM_CX - FACE_GEOM_R &&
           a->x1 <= FACE_GEOM_CX + FACE_GEOM_R &&
           a->y2 >= FACE_GEOM_CY - FACE_GEOM_R &&
           a->y1 <= FACE_GEOM_CY + FACE_GEOM_R;
}

/* 报告标签: 只保留 [a-z0-9_-], 最长 15 字符; 空 = "current" */
static void clip_copy(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; src != NULL && *src != 0 && n + 1 < cap; src++) {
        char ch = *src;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-') {
            dst[n++] = ch;
        }
    }
    dst[n] = 0;
    if (n == 0) snprintf(dst, cap, "current");
}

/* 设计口径(B6 类别规则): smile_static、shocked 简单(T_P95 ≤33ms); laugh、investigate 复杂(≤66ms) */
static const char *clip_class(const char *clip, uint32_t *budget_ms)
{
    if (strcmp(clip, "smile_static") == 0 || strcmp(clip, "shocked") == 0) {
        *budget_ms = 33;
        return "simple";
    }
    if (strcmp(clip, "current") == 0) {
        *budget_ms = 0;
        return "unknown";
    }
    *budget_ms = 66;
    return "complex";
}

static lv_timer_t *find_efc_timer(lv_obj_t *anim)
{
    if (anim == NULL) return NULL;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t != NULL;
         t = lv_timer_get_next(t)) {
        if (lv_timer_get_user_data(t) == anim) return t;
    }
    return NULL;
}

/* ================= D: lv_efc 定时器包装 ================= */
static void d_wrap_cb(lv_timer_t *t)
{
    lv_timer_cb_t orig = s_efc_orig_cb;
    fp_ctx_t *c = s_fp;
    if (orig == NULL) return;
    if (c == NULL || !c->attached || t != c->efc_timer || c->anim == NULL) {
        orig(t);
        return;
    }
    lv_obj_t *anim = c->anim;
    const int32_t f0 = lv_efc_get_current_frame(anim);
    const int64_t t0 = esp_timer_get_time();
    orig(t);
    const int64_t t1 = esp_timer_get_time();
    /* orig 内对象被删时 DELETE 回调已把 c->anim 置 NULL */
    if (c->anim == NULL) return;
    const int32_t f1 = lv_efc_get_current_frame(anim);
    const int32_t total = lv_efc_get_total_frames(anim);
    if (f1 == f0 || f1 < 0 || f1 >= total) return;   /* 未到帧间隔/播完 */
    c->d_cnt++;
    if (c->mode == FP_MODE_WIN) {
        if (c->d_pending) c->d_unpaired++;
        c->d_pending = true;
        c->d_us = us_u32(t1 - t0);
    }
}

/* ================= 摘除钩子(LVGL 上下文或持锁) ================= */
static void disp_evt_cb(lv_event_t *e);
static void obj_evt_cb(lv_event_t *e);

static void detach(fp_ctx_t *c)
{
    if (!c->attached) return;
    c->attached = false;
    if (c->efc_timer != NULL && c->efc_timer->timer_cb == d_wrap_cb &&
        s_efc_orig_cb != NULL) {
        lv_timer_set_cb(c->efc_timer, s_efc_orig_cb);
    }
    /* 9.6 事件表遍历中删除是安全的(标记删除, 遍历结束后清理) */
    if (c->disp != NULL) {
        lv_display_remove_event_cb_with_user_data(c->disp, disp_evt_cb, c);
    }
    if (c->anim != NULL) {
        lv_obj_remove_event_cb_with_user_data(c->anim, obj_evt_cb, c);
    }
    if (c->splash_scr != NULL) {
        lv_obj_remove_event_cb_with_user_data(c->splash_scr, obj_evt_cb, c);
        c->splash_scr = NULL;
    }
    if (c->main_scr != NULL) {
        lv_obj_remove_event_cb_with_user_data(c->main_scr, obj_evt_cb, c);
        c->main_scr = NULL;
    }
    if (c->probe != NULL) {
        lv_timer_delete(c->probe);   /* 可在探针自身回调里删除 */
        c->probe = NULL;
    }
}

/* ================= 开机窗 ================= */
static void boot_splash_end(fp_ctx_t *c, int64_t now)
{
    if (c->phase != BP_SPLASH) return;
    c->t_splash_end = now;
    c->splash_d = c->d_cnt;
    c->phase = BP_WAIT_FIRST;
    xTaskNotify(c->worker, FP_NB_SPLASH, eSetBits);
}

static void boot_refr_start(fp_ctx_t *c, int64_t now)
{
    if (c->phase != BP_SPLASH) return;
    if (c->t_last_refr != 0) {
        const uint32_t v = us_u32(now - c->t_last_refr);
        face_perf_ring_push(&c->irefr, &v);
    }
    c->t_last_refr = now;
    /* 兜底: splash 屏事件未挂上时按状态查询收窗 */
    if (!app_ui_splash_active()) boot_splash_end(c, now);
}

/* 屏幕自检: 显示帧缓冲上表情圆的 812 个采样点中 lit(亮度 ≥128)的点数(ui_face_fmt.h) */
static void screen_check(fp_ctx_t *c)
{
    c->chk_frame = c->anim != NULL ? lv_efc_get_current_frame(c->anim) : -1;
    /* direct 双缓冲才保证 buf_act 是完整一帧且未被 swap_bytes 改写 */
    if (lv_display_get_render_mode(c->disp) != LV_DISPLAY_RENDER_MODE_DIRECT) {
        c->chk = CHK_BAD_MODE;
        return;
    }
    const lv_draw_buf_t *b = lv_display_get_buf_active(c->disp);
    if (b == NULL || b->data == NULL) {
        c->chk = CHK_NO_BUF;
        return;
    }
    if (b->header.cf != LV_COLOR_FORMAT_RGB565) {
        c->chk_cf = b->header.cf;
        c->chk = CHK_BAD_CF;
        return;
    }
    if (b->header.w < FACE_GEOM_SCR_W || b->header.h < FACE_GEOM_SCR_H) {
        c->chk = CHK_BAD_SIZE;
        return;
    }
    const uint32_t w = b->header.w, h = b->header.h;
    const uint32_t stride = b->header.stride != 0 ? b->header.stride : w * 2u;
    uint32_t lit = 0;
    const int pts = ui_face_lit_screen(b->data, stride, w, h, &lit);
    c->chk_pts = pts > 0 ? (uint32_t)pts : 0u;
    c->chk_hit = lit;                           /* 亮度 ≥128 的采样点数 */
    c->chk = c->chk_pts > 0 ? CHK_OK : CHK_BAD_SIZE;
}

static void boot_flush_finish(fp_ctx_t *c, int64_t now)
{
    if (c->phase == BP_WAIT_FIRST && c->flush_face) {
        c->t_first = now;
        c->phase = BP_WAIT_CHECK;
    }
    /* main_loaded、t_loaded 在动画定时器里置位(不在刷新过程中)。自检取
     * SCREEN_LOADED 之后满 FP_CHECK_AFTER_US 的第一次最后一块刷新 */
    if (c->phase == BP_WAIT_CHECK && c->main_loaded && c->flush_last &&
        now - c->t_loaded >= FP_CHECK_AFTER_US) {
        c->t_check = now;
        screen_check(c);
        c->phase = BP_DONE;
        detach(c);
        xTaskNotify(c->worker, FP_NB_DONE, eSetBits);
    }
}

/* ================= 测量窗 ================= */
static void win_frame_done(fp_ctx_t *c, int64_t now)
{
    c->paired = false;
    if (!c->rendered) {
        c->norender++;
        return;
    }
    const uint32_t cyc = us_u32(now - c->cyc_t0);
    const uint32_t f = c->f_acc < cyc ? c->f_acc : cyc;
    const uint64_t tt = (uint64_t)c->pair_d + cyc;
    const uint32_t rec[FP_FRAME_NCOL] = {
        [COL_D] = c->pair_d,
        [COL_R] = cyc - f,
        [COL_F] = f,
        [COL_T] = tt > UINT32_MAX ? UINT32_MAX : (uint32_t)tt,
    };
    face_perf_ring_push(&c->frames, rec);
}

static void win_finish(fp_ctx_t *c, int64_t now)
{
    if (c->wstate != WS_RUN) return;
    c->t_stop = now;
    c->paused1 = c->efc_timer != NULL ? lv_timer_get_paused(c->efc_timer)
                                      : true;
    detach(c);
    c->wstate = WS_DONE;
    if (c->on_done != NULL) c->on_done(c->done_arg);
}

static void probe_cb(lv_timer_t *t)
{
    fp_ctx_t *c = lv_timer_get_user_data(t);
    if (c == NULL || c != s_fp || !c->attached) return;
    const int64_t now = esp_timer_get_time();
    if (c->probe_last != 0) {
        const uint32_t v = us_u32(now - c->probe_last);
        face_perf_ring_push(&c->iloop, &v);
    }
    c->probe_last = now;
    if (now >= c->t_end) win_finish(c, now);
}

/* ================= 事件回调(LVGL 任务) ================= */
static void disp_evt_cb(lv_event_t *e)
{
    fp_ctx_t *c = lv_event_get_user_data(e);
    if (c == NULL || c != s_fp || !c->attached) return;
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        if (c->mode == FP_MODE_BOOT) {
            boot_refr_start(c, now);
            break;
        }
        c->paired = c->d_pending;
        if (c->paired) {
            c->d_pending = false;
            c->pair_d = c->d_us;
            c->cyc_t0 = now;
            c->f_acc = 0;
            c->rendering = false;
            c->rendered = false;
        }
        break;
    case LV_EVENT_RENDER_START:
        if (c->paired) c->rendering = true;
        break;
    case LV_EVENT_RENDER_READY:
        if (c->paired && c->rendering) c->rendered = true;
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        c->w_t0 = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (c->paired && c->w_t0 != 0) c->f_acc += us_u32(now - c->w_t0);
        c->w_t0 = 0;
        break;
    case LV_EVENT_FLUSH_START:
        c->f_t0 = now;
        c->flush_last = lv_display_flush_is_last(c->disp);
        c->flush_face = area_hits_face(lv_event_get_param(e));
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (c->paired && c->f_t0 != 0) c->f_acc += us_u32(now - c->f_t0);
        c->f_t0 = 0;
        if (c->mode == FP_MODE_BOOT) boot_flush_finish(c, now);
        break;
    case LV_EVENT_REFR_READY:
        if (c->paired) win_frame_done(c, now);
        break;
    default:
        break;
    }
}

static void obj_evt_cb(lv_event_t *e)
{
    fp_ctx_t *c = lv_event_get_user_data(e);
    if (c == NULL || c != s_fp) return;
    lv_obj_t *obj = lv_event_get_current_target(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        /* 对象删除后其事件表随之释放; 置 NULL 免 detach 触碰野指针 */
        if (obj == c->anim) {
            c->anim = NULL;
            c->efc_timer = NULL;          /* lv_efc 析构会删定时器 */
        }
        if (obj == c->splash_scr) c->splash_scr = NULL;
        if (obj == c->main_scr) c->main_scr = NULL;
        return;
    }
    if (!c->attached || c->mode != FP_MODE_BOOT) return;
    if (code == LV_EVENT_SCREEN_UNLOAD_START && obj == c->splash_scr) {
        boot_splash_end(c, esp_timer_get_time());
    } else if (code == LV_EVENT_SCREEN_LOADED && obj == c->main_scr) {
        c->main_loaded = true;
        c->t_loaded = esp_timer_get_time();
    }
}

/* ================= 挂接(持 LVGL 锁) ================= */
static esp_err_t attach_locked(fp_ctx_t *c)
{
    static const lv_event_code_t codes[] = {
        LV_EVENT_REFR_START, LV_EVENT_REFR_READY,
        LV_EVENT_RENDER_START, LV_EVENT_RENDER_READY,
        LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH,
        LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH,
    };
    c->disp = lv_display_get_default();
    if (c->disp == NULL) return ESP_ERR_NOT_FOUND;
    c->anim = ui_face_anim_obj();
    c->efc_timer = find_efc_timer(c->anim);
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        if (lv_display_add_event_cb(c->disp, disp_evt_cb, codes[i], c) == NULL) {
            lv_display_remove_event_cb_with_user_data(c->disp, disp_evt_cb, c);
            return ESP_ERR_NO_MEM;
        }
    }
    if (c->anim != NULL &&
        lv_obj_add_event_cb(c->anim, obj_evt_cb, LV_EVENT_DELETE, c) == NULL) {
        lv_display_remove_event_cb_with_user_data(c->disp, disp_evt_cb, c);
        return ESP_ERR_NO_MEM;
    }
    if (c->efc_timer != NULL) {
        if (c->efc_timer->timer_cb != d_wrap_cb) {
            s_efc_orig_cb = c->efc_timer->timer_cb;
        }
        lv_timer_set_cb(c->efc_timer, d_wrap_cb);
        c->d_hooked = true;
    }
    c->attached = true;
    return ESP_OK;
}

/* 屏幕对象订阅(开机窗); 任一失败则该屏不订阅 */
static lv_obj_t *scr_subscribe(lv_obj_t *scr, lv_event_code_t code, void *c)
{
    if (scr == NULL) return NULL;
    if (lv_obj_add_event_cb(scr, obj_evt_cb, code, c) == NULL) return NULL;
    if (lv_obj_add_event_cb(scr, obj_evt_cb, LV_EVENT_DELETE, c) == NULL) {
        lv_obj_remove_event_cb_with_user_data(scr, obj_evt_cb, c);
        return NULL;
    }
    return scr;
}

/* ================= 开机窗 worker(PSRAM 栈) ================= */
static char *cpu_snapshot(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
    /* 每行 ≤ 名字 16 + "\t" + 10 位 + "\t\t" + "100%\r\n" < 48B; +8 余量 */
    const size_t cap = ((size_t)uxTaskGetNumberOfTasks() + 8u) * 48u;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf != NULL) {
        buf[0] = 0;
        vTaskGetRunTimeStats(buf);
    }
    return buf;
#else
    return NULL;
#endif
}

static void boot_print(fp_ctx_t *c, char *cpu)
{
    if (c->phase >= BP_WAIT_FIRST) {
        static const uint32_t thr[1] = { 100000u };
        const uint32_t n = c->irefr.len;
        uint32_t *scratch = ps_calloc(n > 0 ? n : 1u, sizeof(uint32_t));
        face_perf_summary_t s;
        if (scratch != NULL &&
            face_perf_ring_summarize(&c->irefr, 0, scratch, n, thr, 1, &s) == 0) {
            /* d=-1: lv_efc 定时器未装上计时包装, splash 期间解码次数未知 */
            ESP_LOGI(TAG, "splash: n=%" PRIu32 " p95=" FP_MSF " max=" FP_MSF
                     " gt100=%" PRIu32 " d=%" PRId32,
                     s.n, FP_MS(s.p95), FP_MS(s.max), s.over[0],
                     c->d_hooked ? (int32_t)c->splash_d : (int32_t)-1);
        } else {
            ESP_LOGW(TAG, "splash: summary alloc failed n=%" PRIu32, n);
        }
        heap_caps_free(scratch);
    } else {
        ESP_LOGW(TAG, "splash: timeout n=%" PRIu32 " d=%" PRId32,
                 c->irefr.len,
                 c->d_hooked ? (int32_t)c->d_cnt : (int32_t)-1);
    }
    if (cpu != NULL && cpu[0] != 0) {
        ESP_LOGI(TAG, "splash cputop (since boot: task abs pct):");
        char *save = NULL;
        for (char *ln = strtok_r(cpu, "\r\n", &save); ln != NULL;
             ln = strtok_r(NULL, "\r\n", &save)) {
            ESP_LOGI(TAG, "  %s", ln);
        }
    }
    if (c->phase >= BP_WAIT_CHECK) {
        ESP_LOGI(TAG, "face first refresh +" FP_MSF,
                 FP_MS(us_u32(c->t_first - c->t_splash_end)));
    } else if (c->phase == BP_WAIT_FIRST) {
        ESP_LOGW(TAG, "face first refresh: timeout");
    }
    if (c->phase == BP_DONE && c->chk == CHK_OK) {
        const uint32_t pct10 = c->chk_pts > 0
            ? (c->chk_hit * 1000u + c->chk_pts / 2u) / c->chk_pts : 0u;
        ESP_LOGI(TAG, "face screen check: lit=%" PRIu32 ".%" PRIu32
                 "%% pts=%" PRIu32 " frame=%" PRId32,
                 pct10 / 10u, pct10 % 10u, c->chk_pts, c->chk_frame);
    } else if (c->phase == BP_DONE) {
        ESP_LOGW(TAG, "face screen check: n/a reason=%u cf=%" PRIu32,
                 (unsigned)c->chk, c->chk_cf);
    } else if (c->phase >= BP_WAIT_FIRST) {
        ESP_LOGW(TAG, "face screen check: timeout loaded=%d",
                 (int)c->main_loaded);
    }
    if (c->phase == BP_DONE) {
        ESP_LOGI(TAG, "face screen check delay: +" FP_MSF " after SCREEN_LOADED",
                 FP_MS(us_u32(c->t_check - c->t_loaded)));
    }
}

static void boot_worker(void *arg)
{
    fp_ctx_t *c = arg;
    char *cpu = NULL;
    bool done = false;
    while (!done) {
        const int64_t left = c->t_attach + FP_BOOT_TIMEOUT_US - esp_timer_get_time();
        if (left <= 0) break;
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(left / 1000) + 1);
        if ((bits & FP_NB_SPLASH) != 0 && cpu == NULL) cpu = cpu_snapshot();
        if ((bits & FP_NB_DONE) != 0) done = true;
    }
    if (!c->aborted) {
        lvgl_port_lock(0);
        detach(c);                    /* 超时路径; 正常路径已在 LVGL 内摘除 */
        if (s_fp == c) s_fp = NULL;
        lvgl_port_unlock();
        boot_print(c, cpu);
    }
    heap_caps_free(cpu);
    ctx_free(c);
    vTaskDeleteWithCaps(NULL);
}

void face_perf_boot_attach(void)
{
    fp_ctx_t *c = ps_calloc(1, sizeof(*c));
    uint32_t *rb = ps_calloc(FP_IREFR_CAP, sizeof(uint32_t));
    if (c == NULL || rb == NULL) {
        heap_caps_free(c);
        heap_caps_free(rb);
        ESP_LOGW(TAG, "boot window off: no psram");
        return;
    }
    c->mode = FP_MODE_BOOT;
    c->irefr_buf = rb;
    face_perf_ring_init(&c->irefr, rb, FP_IREFR_CAP, 1);
    c->chk_frame = -1;
    c->t_attach = esp_timer_get_time();
    if (xTaskCreatePinnedToCoreWithCaps(boot_worker, "faceperf",
                                        FP_WORKER_STACK, c, FP_WORKER_PRIO,
                                        &c->worker, FP_WORKER_CORE,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ctx_free(c);
        ESP_LOGW(TAG, "boot window off: worker create failed");
        return;
    }

    esp_err_t err;
    bool hooked = false;
    if (!lvgl_port_lock(FP_LOCK_MS)) {
        err = ESP_ERR_TIMEOUT;
    } else {
        if (s_fp != NULL || !app_ui_splash_active()) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = attach_locked(c);
        }
        if (err == ESP_OK) {
            lv_obj_t *main_scr = c->anim != NULL ? lv_obj_get_screen(c->anim)
                                                 : NULL;
            lv_obj_t *act = lv_display_get_screen_active(c->disp);
            c->main_scr = scr_subscribe(main_scr, LV_EVENT_SCREEN_LOADED, c);
            if (act != main_scr) {
                c->splash_scr = scr_subscribe(act, LV_EVENT_SCREEN_UNLOAD_START, c);
            }
            c->phase = BP_SPLASH;
            hooked = c->efc_timer != NULL;
            s_fp = c;
        }
        lvgl_port_unlock();
    }
    /* 解锁后 c 归 worker 所有, 本函数不再访问 */
    if (err != ESP_OK) {
        TaskHandle_t w = c->worker;
        c->aborted = true;
        xTaskNotify(w, FP_NB_DONE, eSetBits);           /* worker 释放 c */
        ESP_LOGW(TAG, "boot window off: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "boot window on (lv_efc timer %s)", hooked ? "hooked" : "missing");
}

/* ================= 测量窗接口 ================= */
esp_err_t face_perf_start(uint32_t seconds, const char *clip,
                          face_perf_done_cb done, void *arg)
{
    if (seconds == 0 || seconds > FACE_PERF_WIN_MAX_S) return ESP_ERR_INVALID_ARG;
    const uint32_t icap = seconds * (1000u / FP_PROBE_MS) + 64u;
    fp_ctx_t *c = ps_calloc(1, sizeof(*c));
    uint32_t *fb = ps_calloc(FP_FRAME_CAP * FP_FRAME_NCOL, sizeof(uint32_t));
    uint32_t *ib = ps_calloc(icap, sizeof(uint32_t));
    if (c == NULL || fb == NULL || ib == NULL) {
        heap_caps_free(c);
        heap_caps_free(fb);
        heap_caps_free(ib);
        return ESP_ERR_NO_MEM;
    }
    c->mode = FP_MODE_WIN;
    c->frames_buf = fb;
    c->iloop_buf = ib;
    face_perf_ring_init(&c->frames, fb, FP_FRAME_CAP, FP_FRAME_NCOL);
    face_perf_ring_init(&c->iloop, ib, icap, 1);
    clip_copy(c->clip, sizeof(c->clip), clip);
    c->on_done = done;
    c->done_arg = arg;

    if (!lvgl_port_lock(FP_LOCK_MS)) {
        ctx_free(c);
        return ESP_ERR_TIMEOUT;
    }
    fp_ctx_t *old = s_fp;
    esp_err_t err = ESP_OK;
    if (old != NULL && (old->mode == FP_MODE_BOOT || old->wstate == WS_RUN ||
                        old->reporting)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = attach_locked(c);
        if (err == ESP_OK && c->efc_timer == NULL) {
            detach(c);
            err = ESP_ERR_NOT_FOUND;
        }
        if (err == ESP_OK) {
            c->probe = lv_timer_create(probe_cb, FP_PROBE_MS, c);
            if (c->probe == NULL) {
                detach(c);
                err = ESP_ERR_NO_MEM;
            }
        }
        if (err == ESP_OK) {
            const ui_model_t *m = ui_model();
            c->delay_ms = lv_efc_get_frame_delay(c->anim);
            c->paused0 = lv_timer_get_paused(c->efc_timer);
            c->ai_state = m != NULL ? (int)m->ai_state : -1;
            c->t_start = esp_timer_get_time();
            c->t_end = c->t_start + (int64_t)seconds * 1000000;
            c->wstate = WS_RUN;
            s_fp = c;                 /* old 已摘除(DONE), 解锁后释放 */
        }
    }
    lvgl_port_unlock();
    if (err != ESP_OK) {
        ctx_free(c);
        return err;
    }
    ctx_free(old);
    return ESP_OK;
}

/* ================= 测量片段切换(CLI faceperf start <秒> <片段>) =================
 * 切换经 ui_face_load_builtin(): lv_efc_set_src_data() 从第 0 帧开始并复位定时器,
 * 不再追加重启调用(否则第 0 帧解码两次)。show_loaded() 会恢复定时器, 因此加载后
 * 由 ui_face_set_running() 按 splash 与面板标志重算运行态, 与开窗前的运行态一致。
 * 测完 clip_restore_cb() 经 ui_face_reload_mood() 恢复 mood 映射的内置片段: 热载缓冲无法
 * 从内存重放, 若开窗前是 SD 热载片段, 恢复后为 mood 映射的内置片段。 */

/* done 回调(LVGL 上下文持锁; 或开窗失败时由下方持锁调用) */
static void clip_restore_cb(void *arg)
{
    (void)arg;
    ui_face_reload_mood();
}

esp_err_t face_perf_start_clip(uint32_t seconds, const char *clip)
{
    if (clip == NULL || clip[0] == 0) {
        return face_perf_start(seconds, NULL, NULL, NULL);
    }
    const uint8_t *start = NULL, *end = NULL;
    if (!ui_face_builtin_lookup(clip, &start, &end)) {
        return ESP_ERR_INVALID_ARG;           /* 只接受内置片段名 */
    }
    if (seconds == 0 || seconds > FACE_PERF_WIN_MAX_S) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(FP_LOCK_MS)) return ESP_ERR_TIMEOUT;
    /* 持锁直到开窗完成(锁可重入): 检查、切换、开窗之间状态不变 */
    esp_err_t err;
    const fp_ctx_t *old = s_fp;
    lv_obj_t *anim = ui_face_anim_obj();
    lv_timer_t *t = find_efc_timer(anim);
    if ((old != NULL && (old->mode == FP_MODE_BOOT || old->wstate == WS_RUN ||
                         old->reporting)) ||
        app_ui_splash_active()) {
        err = ESP_ERR_INVALID_STATE;
    } else if (anim == NULL || t == NULL || !lv_efc_is_loaded(anim)) {
        err = ESP_ERR_NOT_FOUND;      /* 无控件或已回退为底色圆: 不改 UI 状态 */
    } else {
        ui_face_load_builtin(clip);   /* 切换片段、按运行态重挂、输出一行 face clip=… */
        err = lv_efc_is_loaded(anim)
            ? face_perf_start(seconds, clip, clip_restore_cb, NULL)
            : ESP_FAIL;
        if (err != ESP_OK) clip_restore_cb(NULL);
    }
    lvgl_port_unlock();
    return err;
}

esp_err_t face_perf_stop(void)
{
    if (!lvgl_port_lock(FP_LOCK_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    fp_ctx_t *c = s_fp;
    if (c != NULL && c->mode == FP_MODE_WIN && c->wstate == WS_RUN) {
        win_finish(c, esp_timer_get_time());
        err = ESP_OK;
    }
    lvgl_port_unlock();
    return err;
}

bool face_perf_busy(void)
{
    if (!lvgl_port_lock(FP_LOCK_MS)) return true;
    const fp_ctx_t *c = s_fp;
    const bool busy = c != NULL &&
                      (c->mode == FP_MODE_BOOT || c->wstate == WS_RUN);
    lvgl_port_unlock();
    return busy;
}

/* 测量窗已结束(样本环不再被写): 计算汇总并释放样本缓冲 */
static esp_err_t win_summarize(fp_ctx_t *c)
{
    static const uint32_t thr[] = FACE_PERF_STATS_THR_US_INIT;
    uint32_t cap = c->frames.len > c->iloop.len ? c->frames.len : c->iloop.len;
    if (cap == 0) cap = 1;
    uint32_t *scratch = ps_calloc(cap, sizeof(uint32_t));
    if (scratch == NULL) return ESP_ERR_NO_MEM;
    for (uint32_t col = 0; col < FP_FRAME_NCOL; col++) {
        face_perf_ring_summarize(&c->frames, col, scratch, cap, thr,
                                 FACE_PERF_STATS_THR_US_COUNT, &c->sum[col]);
    }
    face_perf_ring_summarize(&c->iloop, 0, scratch, cap, thr,
                             FACE_PERF_STATS_THR_US_COUNT, &c->sum[SUM_ILOOP]);
    heap_caps_free(scratch);
    /* 环结构保留 len/pushed 计数供输出, 存储释放(cap=0 即惰性) */
    heap_caps_free(c->frames_buf);
    heap_caps_free(c->iloop_buf);
    c->frames_buf = c->iloop_buf = NULL;
    c->frames.buf = c->iloop.buf = NULL;
    c->frames.cap = c->iloop.cap = 0;
    c->summarized = true;
    return ESP_OK;
}

static void win_print(const fp_ctx_t *c, face_perf_print_fn pr)
{
    static const char *const names[SUM_N] = { "D", "R", "F", "T", "I_loop" };
    uint32_t budget = 0;
    const char *cls = clip_class(c->clip, &budget);
    /* FP_MS 把 1/1000 单位换成 "x.y": 传毫秒即得秒 */
    const uint32_t dur_ms = us_u32(c->t_stop - c->t_start) / 1000u;
    pr("faceperf: clip=%s class=%s ai=%d delay=%" PRIu32 "ms paused=%d/%d"
       " dur=%" PRIu32 ".%" PRIu32 "s\n",
       c->clip, cls, c->ai_state, c->delay_ms, (int)c->paused0,
       (int)c->paused1, FP_MS(dur_ms));
    if (budget > 0) {
        pr("faceperf: frames=%" PRIu32 " kept=%" PRIu32 " decodes=%" PRIu32
           " unpaired=%" PRIu32 " norender=%" PRIu32 " t_budget=%" PRIu32 "ms\n",
           c->frames.pushed, c->frames.len, c->d_cnt, c->d_unpaired,
           c->norender, budget);
    } else {
        pr("faceperf: frames=%" PRIu32 " kept=%" PRIu32 " decodes=%" PRIu32
           " unpaired=%" PRIu32 " norender=%" PRIu32 " t_budget=na\n",
           c->frames.pushed, c->frames.len, c->d_cnt, c->d_unpaired,
           c->norender);
    }
    for (int k = 0; k < SUM_N; k++) {
        const face_perf_summary_t *s = &c->sum[k];
        pr("faceperf: %-6s n=%" PRIu32 " p50=" FP_MSF " p95=" FP_MSF
           " max=" FP_MSF " gt33=%" PRIu32 " gt66=%" PRIu32 " gt100=%" PRIu32 "\n",
           names[k], s->n, FP_MS(s->p50), FP_MS(s->p95), FP_MS(s->max),
           s->over[0], s->over[1], s->over[2]);
    }
}

esp_err_t face_perf_report(face_perf_print_fn pr)
{
    if (pr == NULL) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(FP_LOCK_MS)) return ESP_ERR_TIMEOUT;
    fp_ctx_t *c = s_fp;
    if (c == NULL) {
        lvgl_port_unlock();
        pr("faceperf: no data\n");
        return ESP_ERR_NOT_FOUND;
    }
    if (c->mode == FP_MODE_BOOT) {
        lvgl_port_unlock();
        pr("faceperf: boot window active\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (c->wstate == WS_RUN) {
        const uint32_t el = us_u32(esp_timer_get_time() - c->t_start) / 1000000u;
        const uint32_t tot = us_u32(c->t_end - c->t_start) / 1000000u;
        const uint32_t nf = c->frames.pushed, ni = c->iloop.pushed;
        lvgl_port_unlock();
        pr("faceperf: running %" PRIu32 "/%" PRIu32 "s frames=%" PRIu32
           " iloop=%" PRIu32 "\n", el, tot, nf, ni);
        return ESP_ERR_INVALID_STATE;
    }
    if (c->reporting) {
        lvgl_port_unlock();
        pr("faceperf: report busy\n");
        return ESP_ERR_INVALID_STATE;
    }
    c->reporting = true;              /* start() 不会释放 c */
    lvgl_port_unlock();

    esp_err_t err = c->summarized ? ESP_OK : win_summarize(c);
    if (err == ESP_OK) {
        win_print(c, pr);
    } else {
        pr("faceperf: summary alloc failed\n");
    }
    lvgl_port_lock(0);
    c->reporting = false;
    lvgl_port_unlock();
    return err;
}

#endif /* CONFIG_ESP_CUBE_FACE_PERF */
