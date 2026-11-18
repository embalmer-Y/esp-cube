/*
 * face_perf_geom.h —— 表情显示区域的几何常量(唯一来源)
 *
 * 使用者: firmware/main/face_perf.c 的开机窗首刷判定(area_hits_face: 刷新区域是否与
 * 表情圆外接方框相交, 决定 `face first refresh` 的时刻)。
 * 屏幕自检的采样点与亮度判定不在本文件: 见 firmware/components/app_ui/ui/ui_face_fmt.h
 * (ui_face_lit_screen, 812 点, 圆心 230、半径 230, 亮度 ≥128; D-FACE-SCREEN, 2026-11-17)。
 *
 * 取值来源(firmware/components/app_ui/ui/ui_widget.c ui_widget_create()):
 *   - 屏幕 460×460 圆屏;
 *   - face_anim 为 460×460、LV_ALIGN_CENTER, 素材图像在控件内居中;
 *   - face_bg(情绪底色圆, 加载失败时的回退)为 430×430 LV_RADIUS_CIRCLE 居中。
 *   FACE_GEOM_R 取 215(底色圆半径), 只用于首刷区域判定; 避开圆屏边缘的 15px 环。
 *
 * 保留的未使用宏(GRID、SAMPLE、BG)是旧采样口径的遗留, 新代码不引用。
 */
#pragma once

#define FACE_GEOM_SCR_W      460
#define FACE_GEOM_SCR_H      460
#define FACE_GEOM_CX         230
#define FACE_GEOM_CY         230
#define FACE_GEOM_R          215
#define FACE_GEOM_GRID       32
/* 主屏背景色 = C_BG(app_ui/ui/ui_internal.h, L4 内部头, L5 不直接包含) */
#define FACE_GEOM_BG_RGB888  0x0A0A12
/* C_BG 经 lv_color_hex() → RGB565: ((R&0xF8)<<8)|((G&0xFC)<<3)|(B>>3) */
#define FACE_GEOM_BG_RGB565  0x0842
#define FACE_GEOM_SAMPLE(c, i) \
    ((c) - FACE_GEOM_R + ((2 * (i) + 1) * FACE_GEOM_R) / FACE_GEOM_GRID)
