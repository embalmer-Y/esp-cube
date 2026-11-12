/*
 * ui_model.h —— UI 与应用的边界层(平台无关, 固件/模拟器同源)
 *
 * 架构(2026-11-11 UI 模拟器方案):
 *   ┌────────────┐  只依赖 lvgl + ui_model   ┌──────────────────────┐
 *   │ ui_pages.c │ ───读快照/调控制──►        │ ui_model.h(本文件)    │
 *   └────────────┘                           └──────────┬───────────┘
 *   固件: ui_port_esp.c 实现后端(事件总线→快照, 控制→app_* 模块)        │
 *   模拟器: sim/src/mock_app.c 实现后端(合成音频/播放器/BT/语音脚本)    │
 *   —— 链接期绑定: 同一份 ui_pages.c 两边编译, UI 代码零分叉。
 *
 * 线程契约:
 *   - 快照字段由后端【单写者】更新; 字符串写入时后端须与 UI 任务互斥
 *     (设备侧 ui_port_esp 用 lvgl_port_lock 包住; 模拟器单线程免锁);
 *   - int/bool 单字段天然原子, 可在任意上下文更新;
 *   - UI 在 LVGL 定时器(30-60fps)里轮询快照, 不做事件回调 —— 页面代码
 *     无锁、无跨任务问题(这是相对 M1 事件直推方案的简化)。
 *
 * 内存契约: 共享 UI 代码只用 malloc/free —— 设备侧 SPIRAM_MALLOC_ALWAYS-
 * INTERNAL=4096, >4KB 分配自动落 PSRAM(canvas 缓冲即走此路径)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 音源 ---- */
typedef enum {
    UI_SRC_IDLE = 0,     /* 待机 */
    UI_SRC_RADIO,        /* 网络电台 */
    UI_SRC_BT,           /* 蓝牙 A2DP */
} ui_source_t;

/* ---- 播放器状态(与 app_player_state_t 值对齐, 但不引固件头) ---- */
typedef enum {
    UI_PLAYER_IDLE = 0,
    UI_PLAYER_CONNECTING,
    UI_PLAYER_PLAYING,
    UI_PLAYER_ERROR,
} ui_player_state_t;

/* ---- AI 会话状态(与 ai_state_t 值对齐, 不引固件头) ---- */
typedef enum {
    UI_AI_IDLE = 0,
    UI_AI_WAKE,
    UI_AI_LISTENING,
    UI_AI_THINKING,
    UI_AI_SPEAKING,
} ui_ai_state_t;

#define UI_STR_LEN     64
#define UI_SUB_LEN     96
#define UI_MAX_STATIONS 8

/* ---- 应用状态快照(后端持有并可变, UI 只读) ---- */
typedef struct {
    /* 音源与播放 */
    ui_source_t source;
    ui_player_state_t player;
    char title[UI_STR_LEN];        /* 曲目 / ICY StreamTitle / BT 标题 */
    char artist[UI_STR_LEN];
    char album[UI_STR_LEN];
    int volume;                    /* 0-100 */
    int brightness;                /* 0-100(面板寄存器) */
    int buffer_pct;                /* 网络缓冲占用%, <0 = 无数据 */

    /* 电台列表(station_names 指向后端静态存储, 指针终身有效) */
    int station_idx;               /* 当前选中 */
    int station_num;
    const char *station_names[UI_MAX_STATIONS];

    /* 连接状态 */
    bool wifi_up;
    char wifi_ssid[33];
    char wifi_ip[16];
    bool bt_connected;

    /* 歌词(BT 音乐; 电台无歌词; 纯音乐 has_lyrics=false → 频谱动画) */
    bool has_lyrics;
    char lyric_cur[UI_SUB_LEN];    /* 当前行 */
    char lyric_next[UI_SUB_LEN];   /* 下一行(可为空) */

    /* 语音 AI(状态驱动极简提示: 聆听中/思考中…; 文本暂存字幕位) */
    ui_ai_state_t ai_state;
    char ai_text[UI_SUB_LEN];      /* 最近一次 ASR/AI 文本 */

    /* 助理服务(M2/M3): 时间/琐事/提醒/灵魂情绪(后端单写者) */
    bool time_synced;              /* SNTP 已校时 */
    uint8_t hour, min;             /* 本地时间(1Hz 刷新) */
    uint8_t mon, day, wday;        /* 月/日/周(日粒度刷新) */
    uint8_t memo_cnt, remind_cnt;  /* 在册琐事/待触发提醒数 */
    char next_remind[UI_STR_LEN];  /* 最近一条提醒文本(空 = 无) */
    int32_t next_remind_in_min;    /* 距其分钟数; <0 = 无 */
    char mood_tag[16];             /* happy/relaxed/angry/sad/
                                    * surprised/confused/sleepy; 空 = 无 */

    /* 命令(后端写, UI 轮询消费后缓存去重; 0=无 1=开状态栏 2=开AI状态
     * 3=收起 —— P4C5 无触摸期的 CLI/语音入口, sim/真触摸同样可用) */
    int panel_req;

    /* 杂项 */
    char fw_info[32];              /* 版本/构建信息 */
} ui_model_t;

/* 取快照(返回后端持有的可变内存; 读方在 UI 任务上下文) */
const ui_model_t *ui_model(void);

/* ---- 控制接口(UI 事件回调/LVGL 上下文中调用; 后端实现) ---- */
void ui_ctl_set_volume(int percent);      /* 同时驱动混音器/AVRCP 回传 */
void ui_ctl_set_brightness(int percent);  /* 面板 0x51 寄存器 */
void ui_ctl_toggle_play(void);            /* 电台: 播放/停止; BT: 透传暂停(M3) */
void ui_ctl_switch_station(int dir);      /* ±1 环切并立即换台 */
void ui_ctl_select_station(int idx);      /* 设置页点选 */

/* 拉最新输出音频(立体声交织, 16bit)供频谱 FFT; 返回实际帧数(0=无数据) */
int ui_audio_take_pcm(int16_t *stereo, int frames);

#ifdef __cplusplus
}
#endif
