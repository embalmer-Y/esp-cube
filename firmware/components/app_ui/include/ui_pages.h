/*
 * ui_pages.h —— 页面构建入口(内部接口, 仅供 app_ui.c 调用)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 在 LVGL 锁保护下构建默认页面(w/h 为面板分辨率) */
void ui_pages_create(int w, int h);

#ifdef __cplusplus
}
#endif
