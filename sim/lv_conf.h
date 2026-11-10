/*
 * lv_conf.h —— EspCube PC 模拟器 LVGL 配置
 *
 * 与设备侧(firmware/sdkconfig, LVGL 组件 Kconfig)保持关键一致:
 *   - RGB565 / 16bit(canvas 直接写像素的字节序两端一致)
 *   - LV_FONT_FMT_TXT_LARGE=1(自生成 CJK 字体位图 2MB, 超普通 20bit 索引)
 *   - 深色默认主题
 * 差异(模拟器专用): 内存走 libc、OS=NONE(裸主循环)。
 */
#pragma once

/* 颜色(9.6 用 LV_COLOR_FORMAT_DEFAULT; RGB565 与设备一致) */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* 内存: 用系统堆(canvas 大缓冲由 malloc 提供) */
#define LV_MEM_CUSTOM       1
#define LV_MEMCPY_MEMSET_STD 1

/* OS: 无(主循环手动泵 lv_timer_handler + lv_tick_inc) */
#define LV_USE_OS           0

/* 字体 */
#define LV_FONT_FMT_TXT_LARGE           1
#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_MONTSERRAT_16           1
#define LV_FONT_MONTSERRAT_28           1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 主题: 深色 */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_PRIMARY_COLOR 0x40C8F8
#define LV_THEME_DEFAULT_SECONDARY_COLOR 0x8F7BFF

/* 布局 */
#define LV_USE_FLEX         1
#define LV_USE_GRID         1

/* 刷新节拍(模拟器跑满, 由 SDL 主循环控制) */
#define LV_DEF_REFR_PERIOD  16

/* 日志 */
#define LV_USE_LOG          1
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF       1
