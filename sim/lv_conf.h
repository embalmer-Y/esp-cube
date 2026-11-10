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

/* 内存: 用系统堆(canvas/歌词离屏 layer 的大缓冲走 malloc;
 * 9.6 已改名 LV_USE_STDLIB_MALLOC, 旧名 LV_MEM_CUSTOM 被忽略
 * → 内置 64KB 池, 歌词 layer 86KB 分配失败) */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
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

/* 注意: 不开 LV_DRAW_TRANSFORM_USE_MATRIX(9.6.0 的旋转 layer 变换
 * 绘制 argb8888_row_checked 崩, 且 rotation+opa 动画组合走离屏路径
 * 会分配失败)。歌词飞行动画只用位移, 不与 opa 动画叠加(见 ui_pages.c)。

/* 刷新节拍(模拟器跑满, 由 SDL 主循环控制) */
#define LV_DEF_REFR_PERIOD  16

/* 日志 */
#define LV_USE_LOG          1
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF       1
