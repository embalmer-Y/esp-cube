/*
 * drv_display.h —— CO5300 AMOLED 显示驱动封装(双板同 API)
 *
 * 职责: 总线 + 面板对象创建、背光(AMOLED 为寄存器亮度)、
 *       对 app_ui 暴露 esp_lcd 句柄(lvgl_port 直接复用)。
 * 面板: 鱼鹰光电 2寸 460x460 AMOLED(IC=CO5300)
 *   - EspCube S31 板: QSPI 版 AM200Q460460FLS1(后端 drv_display_qspi.c)
 *   - 鱼鹰 P4C5 板:   MIPI-DSI 版 AM200MT460460FLS2(后端 drv_display_dsi.c)
 * cfg.pixel_clock_hz 语义随板: S31=QSPI 时钟 / P4C5=DPI 像素钟。
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
    int pixel_clock_hz;      /* S31=QSPI 时钟(首亮 10MHz, 稳定后 40MHz);
                                P4C5=DPI 像素钟(板级常量, 此值兜底校验) */
    int h_res;
    int v_res;
    bool use_te;             /* 是否启用 TE 撕裂同步(S31 QSPI, M3) */
} drv_display_cfg_t;

/* 初始化(幂等): 创建总线 IO + 面板, 执行厂商初始化序列, 复位并点亮 */
esp_err_t drv_display_init(const drv_display_cfg_t *cfg);

/* 单线(3-wire 9-bit)点亮自检: GPIO 位压彩条, 不经 GPSPI/QSPI(docs/05 排查);
 * 点亮=D0/CLK/CS/RST 全通。仅 S31 后端实现,
 * CONFIG_ESP_CUBE_LCD_SINGLE_LANE_TEST(限 S31 目标)启用 */
esp_err_t drv_display_single_lane_test(int h_res, int v_res);

/* 获取句柄(供 esp_lvgl_port / 直接绘屏使用) */
esp_lcd_panel_io_handle_t drv_display_get_io(void);
esp_lcd_panel_handle_t drv_display_get_panel(void);

/* 亮度(0-100, AMOLED 无背光, 写 CO5300 亮度寄存器) */
esp_err_t drv_display_set_brightness(int percent);

/* 显示关闭/恢复(不进入深度睡眠) */
esp_err_t drv_display_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
