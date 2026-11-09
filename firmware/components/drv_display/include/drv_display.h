/*
 * drv_display.h —— CO5300 QSPI AMOLED 显示驱动封装
 *
 * 职责: SPI 总线 + 面板对象创建、背光(AMOLED 为寄存器亮度)、
 *       对 app_ui 暴露 esp_lcd 句柄(lvgl_port 直接复用)。
 * 面板: 鱼鹰光电 2寸 460x460 QSPI AMOLED(AM200Q460460FLS1, IC=CO5300)
 */
#pragma once

#include <stdint.h>
#include <esp_err.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pixel_clock_hz;      /* QSPI 时钟(建议首次点亮用 40MHz, 稳定后 80MHz) */
    int h_res;
    int v_res;
    bool use_te;             /* 是否启用 TE 撕裂同步 */
} drv_display_cfg_t;

/* 初始化(幂等): 创建 SPI IO + 面板, 执行厂商初始化序列, 复位并点亮 */
esp_err_t drv_display_init(const drv_display_cfg_t *cfg);

/* 获取句柄(供 esp_lvgl_port / 直接绘屏使用) */
esp_lcd_panel_io_handle_t drv_display_get_io(void);
esp_lcd_panel_handle_t drv_display_get_panel(void);

/* 亮度(0-100, AMOLED 无背光, 写 CO5300 亮度寄存器) */
esp_err_t drv_display_set_brightness(int percent);

/* 睡眠控制(省电/待机) */
esp_err_t drv_display_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
