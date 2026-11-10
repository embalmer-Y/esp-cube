/*
 * ui_pages.h —— 氛围 UI 构建入口(平台无关; 由 app_ui.c / 模拟器 main 调用)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 构建全部界面(splash → 极简氛围场景), 须在 LVGL 上下文调用 */
void ui_pages_create(int w, int h);

#ifdef __cplusplus
}
#endif
