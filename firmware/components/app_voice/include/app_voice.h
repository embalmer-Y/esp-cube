/*
 * app_voice.h —— 语音前端(esp-sr)
 *
 * 管线(设计文档 §5.1):
 *   板载麦(48k) → 降采样16k ┐
 *   混音器输出 → AEC参考 16k ┴→ esp-sr AFE(SR模式, "MMR") → WakeNet10 → MultiNet
 * 事件: CUBE_EVT_WAKE_DETECTED / CUBE_EVT_VOICE_CMD(音量+/暂停/下一首等本地命令)
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本地命令词 ID(MultiNet mn7_cn, 与命令表顺序对应) */
typedef enum {
    VOICE_CMD_NONE = 0,
    VOICE_CMD_VOLUME_UP,
    VOICE_CMD_VOLUME_DOWN,
    VOICE_CMD_PAUSE,
    VOICE_CMD_PLAY,
    VOICE_CMD_NEXT_TRACK,
    VOICE_CMD_PREV_TRACK,
} app_voice_cmd_t;

/* 启动语音前端任务(依赖 drv_audio 已初始化; 混音器已注册参考抽头) */
esp_err_t app_voice_start(void);

#ifdef __cplusplus
}
#endif
