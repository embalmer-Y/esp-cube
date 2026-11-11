/*
 * app_ui.h —— L4 表现层入口(LVGL 9)
 *
 * 依赖规则(docs/06 §2.1): 本组件只依赖 ui_model.h 契约与 LVGL,
 * 显示句柄由装配层(main)经启动参数注入 —— 组件不 include 任何
 * L1-L3 固件头, 固件/模拟器同一份 UI 代码(sim/src/mock_app.c 为
 * 模拟器后端, main/ui_port_esp.c 为设备后端)。
 *
 * 刷新策略(设计文档 §4): 全帧双缓冲(2×460×460×2B 落 PSRAM, S31
 * PSRAM 专用控制器, 不与 QSPI LCD 争带宽): 坐标天然偶对齐(CO5300
 * 硬约束), 无撕裂; 刷新节拍由 esp_lvgl_port 任务驱动, 动画密集页
 * 实测 45-60fps(40MHz QSPI); 部分刷新+TE 是后续提升项(见里程碑)。
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 显示接口类型(由装配层按板注入, L4 不感知板卡) */
typedef enum {
    UI_DISP_IF_QSPI = 0,    /* S31: QSPI 面板, 全帧双缓冲 + swap_bytes */
    UI_DISP_IF_DSI,         /* P4C5: MIPI-DSI DPI, direct_mode + 防撕裂 */
} ui_disp_iface_t;

/* 启动配置(句柄以 void* 传递: 免得 app_ui.h 引入 esp_lcd 头) */
typedef struct {
    void *io_handle;        /* esp_lcd_panel_io_handle_t(QSPI/DBI IO) */
    void *panel_handle;     /* esp_lcd_panel_handle_t(面板) */
    int h_res;
    int v_res;
    ui_disp_iface_t iface;  /* 显示接口(决定 esp_lvgl_port 绑定方式) */
} app_ui_boot_cfg_t;

/* 初始化 LVGL 运行环境 + 绑定显示 + 构建页面(在 drv_display_init 成功后) */
esp_err_t app_ui_start(const app_ui_boot_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
