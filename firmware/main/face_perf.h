/*
 * face_perf.h —— 表情逐帧计时探针(L5, CONFIG_ESP_CUBE_FACE_PERF; 阶段 A 只观测)
 *
 * freeze-and-face-blackscreen-fix 任务 31.4(B4)。实现见 face_perf.c, 统计口径见
 * face_perf_stats.h(P50/P95/max、超阈 = 严格大于), 几何口径见 face_perf_geom.h,
 * 屏幕自检的采样与亮度判定见 ui_face_fmt.h(ui_face_lit_screen)。
 *
 * 两类观测窗口, 同一时刻只有一个:
 *   - 开机窗(face_perf_boot_attach, main 在 app_ui_start() 后调用): 从 UI 启动到
 *     splash 结束统计 I_refr, 输出 `splash: n=… p95=…ms max=…ms gt100=… d=…` 与一次
 *     vTaskGetRunTimeStats 快照; 随后输出 `face first refresh +Xms` 与
 *     `face screen check: lit=xx.x% pts=812 frame=N`, 然后摘除全部钩子、释放缓冲。
 *   - 测量窗(face_perf_start; CLI `faceperf start` 经 face_perf_start_clip): 逐帧
 *     D/R/F/T 与 20ms 探针定时器的 I_loop; 到时自动结束, face_perf_report 出报告。
 *
 * 只观测: 钩子只记时间戳, lv_efc 帧定时器包装原样调用原回调, 探针定时器不
 * invalidate; 样本缓冲走 PSRAM 堆, 开窗时分配, 报告后(或下次开窗时)释放。
 * 不打逐帧日志; 日志只含数值、片段名与固定标签(NFR-03)。
 *
 * 线程: 接口在普通任务调用(CLI/main), 内部按需取 lvgl_port_lock(可重入);
 * 不要在 LVGL 任务内调用 face_perf_start/report —— report 的排序在调用者
 * 任务执行, 放进 LVGL 任务会造成一次界面停顿。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* printf 风格输出函数(CLI 传 printf) */
typedef int (*face_perf_print_fn)(const char *fmt, ...);

/* 测量窗结束回调: 在 LVGL 上下文、持 LVGL 锁时调用(LVGL 任务内的探针
 * 定时器到时, 或 face_perf_stop 的调用者持锁时)。供 CLI 恢复测量前的片段;
 * 不得阻塞。 */
typedef void (*face_perf_done_cb)(void *arg);

#define FACE_PERF_WIN_MAX_S 300u   /* 测量窗最长秒数 */

#if CONFIG_ESP_CUBE_FACE_PERF

/* main 在 app_ui_start() 成功后立即调用一次(任务上下文)。
 * splash 已结束或资源不足时只告警, 不影响启动。 */
void face_perf_boot_attach(void);

/*
 * 开测量窗: seconds ∈ [1, FACE_PERF_WIN_MAX_S]; clip 仅作报告标签
 * (NULL = "current"; 片段切换由调用者在调用前完成); done 可为 NULL。
 * 返回: ESP_OK; ESP_ERR_INVALID_ARG; ESP_ERR_INVALID_STATE(开机窗或上一
 * 测量窗未结束/报告进行中); ESP_ERR_NO_MEM; ESP_ERR_NOT_FOUND(无显示或
 * 找不到 lv_efc 帧定时器); ESP_ERR_TIMEOUT(LVGL 锁 500ms 内未取到)。
 */
esp_err_t face_perf_start(uint32_t seconds, const char *clip,
                          face_perf_done_cb done, void *arg);

/*
 * CLI `faceperf start <秒> [片段]`(任务 31.4): clip 为 NULL/空时等同
 * face_perf_start(seconds, NULL, NULL, NULL), 不切片段; 否则在 LVGL 锁内把表情
 * 切到该内置片段(smile_static、laugh、investigate、shocked), 输出一行 face clip=<名>,
 * 再开测量窗。测完(到时或 face_perf_stop)按 mood 映射恢复片段, 运行态按 splash 与
 * 面板标志重算(即恢复开窗前的运行态)。开窗失败时立即恢复。
 * 返回值同 face_perf_start, 另: clip 不在内置表 → ESP_ERR_INVALID_ARG;
 * splash 未结束 → ESP_ERR_INVALID_STATE; 表情未加载 → ESP_ERR_NOT_FOUND;
 * 片段加载失败 → ESP_FAIL。
 */
esp_err_t face_perf_start_clip(uint32_t seconds, const char *clip);

/* 提前结束测量窗(会调用 done)。无运行中的测量窗返回 ESP_ERR_INVALID_STATE */
esp_err_t face_perf_stop(void);

/* 开机窗或测量窗进行中 */
bool face_perf_busy(void);

/*
 * 输出最近一次测量窗报告: 每段(D、R、F、T、I_loop)的 n、P50、P95、max 与超阈计数
 * (33/66/100ms)。首次报告时计算汇总并释放样本缓冲, 之后可重复输出同一汇总。
 * 测量中输出进度并返回 ESP_ERR_INVALID_STATE; 无数据返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t face_perf_report(face_perf_print_fn pr);

#else  /* !CONFIG_ESP_CUBE_FACE_PERF */

static inline void face_perf_boot_attach(void) {}
static inline esp_err_t face_perf_start(uint32_t seconds, const char *clip,
                                        face_perf_done_cb done, void *arg)
{
    (void)seconds; (void)clip; (void)done; (void)arg;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t face_perf_start_clip(uint32_t seconds, const char *clip)
{
    (void)seconds; (void)clip;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t face_perf_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool face_perf_busy(void) { return false; }
static inline esp_err_t face_perf_report(face_perf_print_fn pr)
{
    (void)pr;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif
