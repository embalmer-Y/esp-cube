/*
 * ai_session.h —— AI 会话客户端(大模型语音控制)
 *
 * 架构(设计文档 §5.2):
 *   状态机(IDLE→WAKE→LISTENING→THINKING→SPEAKING) 与【协议适配器】解耦:
 *     - xiaozhi_client: WebSocket + Opus(16k/mono/60ms) → 自建 xiaozhi-esp32-server(GLM)
 *     - glm_realtime_client(预留): WSS 直连 GLM-Realtime(pcm16 入/pcm24 出)
 *   事件: CUBE_EVT_AI_STATE / CUBE_EVT_AI_TEXT
 *   音频: TTS 解码 → 重采样48k → 混音器 AUDIO_SRC_TTS 通道(ducking 由状态机驱动)
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_IDLE = 0,
    AI_WAKE,
    AI_LISTENING,
    AI_THINKING,
    AI_SPEAKING,
} ai_state_t;

/* 协议适配器接口(M2 实现; 新后端实现此结构即可切换) */
typedef struct {
    const char *name;
    esp_err_t (*connect)(void);
    esp_err_t (*disconnect)(void);
    /* 上行音频(16k mono int16);返回推入字节数 */
    int (*send_audio)(const int16_t *pcm, int frames);
    /* 控制文本(如 listen start/stop) */
    esp_err_t (*send_ctrl)(const char *json);
    /* 打断(中止当前 TTS) */
    esp_err_t (*abort)(void);
} ai_backend_t;

/* 启动会话状态机(依赖 app_voice 与混音器) */
esp_err_t ai_session_start(void);

/* 唤醒入口(由 app_voice 的唤醒事件驱动) */
void ai_session_on_wake(void);

#ifdef __cplusplus
}
#endif
