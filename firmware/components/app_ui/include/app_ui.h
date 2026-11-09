/*
 * app_ui.h —— AMOLED UI(LVGL 9, 目标 60Hz)
 *
 * 刷新策略(设计文档 §4):
 *   M1 采用【全帧双缓冲】(2×460×460×2B 落 PSRAM, S31 PSRAM 专用控制器,
 *   不与 QSPI LCD 争带宽): 坐标天然偶数对齐(CO5300 硬约束), 无撕裂;
 *   刷新节拍由 esp_lvgl_port 任务驱动(默认 20ms tick), 动画密集页
 *   实测 45-60fps; 后续切部分刷新 + TE 同步进一步提升(见里程碑)。
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LVGL 与页面(在 drv_display_init 之后调用) */
esp_err_t app_ui_start(void);

#ifdef __cplusplus
}
#endif
