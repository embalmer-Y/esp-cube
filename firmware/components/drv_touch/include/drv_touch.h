/*
 * drv_touch.h —— CST820 触摸驱动接入(P4C5 屏模组 in-cell 触摸)
 *
 * I2C0 与 ES8311 codec 共总线(drv_audio 创建并导出句柄, 7 位地址 0x15);
 * RST=GPIO6 / INT=GPIO21(board_pins_p4c5.h)。LVGL indev 由 L5
 * (ui_port_esp) 创建 —— 本层只提供触摸设备与采样读取。
 */
#pragma once

#include <esp_err.h>
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在已存在的 I2C 总线上创建 CST820(幂等; 失败降级 = 返回错误, UI 无触摸) */
esp_err_t drv_touch_start(i2c_master_bus_handle_t bus);

/* 触摸句径(未启动返回 NULL; 只读) */
esp_lcd_touch_handle_t drv_touch_handle(void);

#ifdef __cplusplus
}
#endif
