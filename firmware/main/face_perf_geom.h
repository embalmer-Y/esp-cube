/*
 * face_perf_geom.h —— 表情屏幕内容自检的几何与采样口径(唯一来源)
 *
 * 使用者:
 *   - firmware/main/face_perf.c(任务 4.2): 主屏首次渲染后在活动帧缓冲上采样,
 *     输出 `face screen check: nonbg=xx%`;
 *   - tools/eaf_codec.py lint: 解析本文件的整数 #define, 按同一口径给出每份
 *     素材第 0 帧的板上期望值(板上值应落在期望 ±5 个百分点内)。
 *   改动本文件即同时改动两端口径; 只允许"#define 名 整数"形式(lint 用正则解析)。
 *
 * 取值来源(firmware/components/app_ui/ui/ui_widget.c ui_widget_create()):
 *   - 屏幕 460×460 圆屏(boardcfg AM200MT460460FLS2 / AM200Q460460FLS1);
 *   - face_anim 为 460×460、LV_ALIGN_CENTER, 素材图像在控件内居中;
 *   - face_bg(情绪底色圆, 加载失败时的回退)为 430×430 LV_RADIUS_CIRCLE 居中。
 *   "表情圆"取 face_bg 圆: 圆心 (230,230)、半径 215。不取面板外接圆 230,
 *   以避开圆屏边缘 15px 的抗锯齿/不可见环, 也与回退底色圆的可见范围一致。
 *
 * 采样口径:
 *   GRID×GRID 点阵均布在表情圆的外接正方形上, 第 i 列/第 j 行(0 起)的点为
 *     x = FACE_GEOM_SAMPLE(FACE_GEOM_CX, i), y = FACE_GEOM_SAMPLE(FACE_GEOM_CY, j)
 *   (整数除法, 即各格中心向下取整), 只保留 (x-CX)^2 + (y-CY)^2 <= R^2 的点。
 *   nonbg = 保留点中 RGB565 值 != FACE_GEOM_BG_RGB565 的比例。
 *   屏幕坐标 → 素材坐标: ix = x - (SCR_W - w) / 2, iy = y - (SCR_H - h) / 2
 *   (素材居中; 落在素材外的点按背景计)。
 */
#pragma once

#define FACE_GEOM_SCR_W      460
#define FACE_GEOM_SCR_H      460
#define FACE_GEOM_CX         230
#define FACE_GEOM_CY         230
#define FACE_GEOM_R          215
#define FACE_GEOM_GRID       32

/* 主屏背景色 = C_BG(app_ui/ui/ui_internal.h, L4 内部头, L5 不直接包含);
 * eaf_codec.py lint 每次运行都校验二者一致, 不一致即报错。 */
#define FACE_GEOM_BG_RGB888  0x0A0A12
/* C_BG 经 lv_color_hex() → RGB565: ((R&0xF8)<<8)|((G&0xFC)<<3)|(B>>3) */
#define FACE_GEOM_BG_RGB565  0x0842

#define FACE_GEOM_SAMPLE(c, i) \
    ((c) - FACE_GEOM_R + ((2 * (i) + 1) * FACE_GEOM_R) / FACE_GEOM_GRID)
