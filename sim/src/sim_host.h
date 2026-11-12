/*
 * sim_host.h —— 模拟器内部接口(main.c 提供 host 能力, mock_app.c 提供后端)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 合成输入回放(main.c 实现; mock/demo 调用) */
void sim_swipe_playback(int dy);
void sim_tap_playback(void);

#ifdef __cplusplus
extern "C" {
#endif

/* main.c 实现: 截图当前 LVGL 帧到 sim/shots/<name>.bmp */
void sim_host_screenshot(const char *name);

/* 键盘动作(由 main.c 的 SDL 按键映射产生) */
typedef enum {
    SIM_ACT_TOGGLE_PLAY,
    SIM_ACT_NEXT_STATION,       /* 电台换台 / BT 切曲(下一) */
    SIM_ACT_PREV_STATION,
    SIM_ACT_BT_TOGGLE,          /* 蓝牙连接/断开 */
    SIM_ACT_BT_META,            /* BT 切歌(元数据刷新) */
    SIM_ACT_ICY_TITLE,          /* 电台 StreamTitle 刷新 */
    SIM_ACT_VOL_UP,
    SIM_ACT_VOL_DOWN,
    SIM_ACT_AI_ASK,            /* 演示一轮 AI 会话(聆听→思考→回答) */
    SIM_ACT_SWIPE_DOWN,        /* 合成下滑(开状态栏; M4 面板演示) */
    SIM_ACT_SWIPE_UP,          /* 合成上滑(开 AI 状态) */
    SIM_ACT_TAP,               /* 合成单击(收起面板) */
} sim_action_t;

/* mock_app.c 实现 */
void mock_app_init(bool demo);
void mock_app_action(sim_action_t a);       /* 键盘动作入口 */
void mock_app_periodic(uint32_t now_ms);    /* 后台推进(标题轮换/歌词/demo) */
int  mock_app_demo_done(void);              /* demo 模式: 1=脚本播完可退出 */

#ifdef __cplusplus
}
#endif
