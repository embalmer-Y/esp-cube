/*
 * ui_pages.h —— 页面构建与导航(平台无关; 由 app_ui.c / 模拟器 main 调用)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 页面序号(与 tileview 列号一致) */
enum {
    UI_PAGE_VOICE = 0,
    UI_PAGE_PLAY = 1,
    UI_PAGE_SETTINGS = 2,
};

/* 构建全部界面(splash → 极简氛围场景), 须在 LVGL 上下文调用 */
void ui_pages_create(int w, int h);

#ifdef __cplusplus
}
#endif
