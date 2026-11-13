/*
 * ai_session.h —— AI 云端会话编排(百炼组合 C: ASR→LLM→TTS)
 *
 * 架构(2026-11-11 定稿, 取代 xiaozhi 方案——Kaleido D-7 同款裁决):
 *   协议实现已由 speech_provider 组件承担(移植 Kaleido kaleido_speech,
 *   V-33 真机验证); 本组件只做【编排】:
 *     IDLE → LISTENING(录音+ASR 流) → THINKING(LLM 流式)
 *          → SPEAKING(TTS→重采样→mixer "tts" 源) → IDLE
 *   打断(barge-in): 任意状态再收唤醒 → abort → 重开 LISTENING
 *   事件: CUBE_EVT_AI_STATE / CUBE_EVT_AI_TEXT(状态枚举 ai_state_t
 *         定义在 cube_event_bus.h, 供 app_voice 所有权切换免依赖本头)
 *   触发: 自订阅 CUBE_EVT_WAKE_DETECTED(app_voice 唤醒词)或本函数直调
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动会话状态机(依赖 speech_provider/app_audio 麦克风服务;
 * VOICE=y 时另需 app_voice 唤醒) */
esp_err_t ai_session_start(void);

/* 唤醒入口(正常由事件总线驱动, 亦可供调试直调) */
void ai_session_on_wake(void);

/* 文本注入入口(跳过唤醒/录音/ASR, 直接进 LLM→TTS 轮):
 * CLI `ask` 命令/B2 语音链外的调试通道; 会在会话任务异步执行 */
esp_err_t ai_session_submit_text(const char *text);

/* 自动化验证(无真人): TTS 合成人声注入麦克 → 全链自测 */
esp_err_t ai_session_selftest(const char *text);   /* 合成 text 并走完整轮 */
esp_err_t ai_session_stwake(void);                 /* 合成唤醒词测 WakeNet */

/* 带外播报(提醒到点/主动交互): 不经 ASR/LLM, 直接出声(M2)。
 * 快线在位走 omni(同一音色), 否则 cosyvoice; 会话忙时挂起,
 * 本轮结束(IDLE)后自动补播。Kaleido "站在会话外对着会话说话"语义 */
esp_err_t ai_session_announce(const char *text);

/* 灵魂 2.0 深思通道(soul_think brain 注入): 整句收集 LLM 回复,
 * 0=成功有回复; system=深思协议 prompt */
int ai_session_deliberate(const char *system_prompt, char *reply, size_t cap);

/* TTS 音色热切(音色甄选台调/persona 配置): 断当前会话令下轮懒开
 * 生效; announce 同源一并切换 */
void ai_session_set_tts_voice(const char *voice);
const char *ai_session_tts_voice(void);

#ifdef __cplusplus
}
#endif
