/*
 * ai_session.h —— AI 云端会话编排(百炼组合 C: ASR→LLM→TTS)
 *
 * 架构(2026-11-11 定稿, 取代 xiaozhi 方案——Kaleido D-7 同款裁决):
 *   协议实现已由 speech_provider 组件承担(移植 Kaleido kaleido_speech,
 *   V-33 真机验证); 本组件只做【编排】:
 *     IDLE → LISTENING(录音+ASR 流) → THINKING(LLM 流式)
 *          → SPEAKING(TTS→重采样→mixer "tts" 源) → IDLE
 *   打断(barge-in): 任意状态再收唤醒 → abort → 重开 LISTENING
 *   事件: CUBE_EVT_AI_STATE / CUBE_EVT_AI_TEXT
 *   触发: ai_session_on_wake() —— app_voice 唤醒词 / 调试事件
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

/* 启动会话状态机(依赖 speech_provider/混音器/drv_audio; VOICE=y 时另需 app_voice 唤醒) */
esp_err_t ai_session_start(void);

/* 唤醒入口(由 app_voice 的唤醒事件驱动) */
void ai_session_on_wake(void);

#ifdef __cplusplus
}
#endif
