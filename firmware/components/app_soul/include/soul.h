/*
 * soul.h —— 灵魂内核编排层(M3, 移植自 Kaleido convo-stage soul.c)
 *
 * 三层结构(Kaleido 灵魂架构 v1):
 *   驱力(kd) → 感受态(ke) → 消费(prompt 注入/TTS hint/UI 表情/点火)
 * 由 1Hz tick 任务单写者驱动(kd/ke 组件纪律: 文件级 static 无锁);
 * 事件钩子任意任务可调(内部只置 pending 位, tick 内按优先级消费:
 * BARGE > ROUND_* > VOICE)。
 *
 * 消费通道:
 *   - soul_prompt_suffix(): 每轮 LLM system prompt 追加内在状态 +
 *     mood 标签协议(LLM 回复尾部 <mood d="+0.1"/>, soul_strip_mood_tag
 *     剥离并回写 —— LLM 禁直写情感数值, 重评价唯一合法通道);
 *   - soul_tts_hint(): 语速/音高调制(cosyvoice speech_rate/pitch);
 *   - CUBE_EVT_SOUL_MOOD: anim_tag/关键维快照(节流: tag 变化或 60s);
 *   - soul_on_round(): 顺带喂记忆(soul_mem, 另文件)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 1Hz 灵魂任务(幂等); 依赖 NVS 已初始化(记忆层) */
esp_err_t soul_start(void);

/* 主动交互(点火): 出声通道注入(避免组件循环依赖) + 启动 */
void soul_think_set_speak(esp_err_t (*fn)(const char *text));
esp_err_t soul_think_start(void);

/* ---- 事件钩子(任意任务; 内部 pend, tick 消费) ---- */
void soul_on_round(bool ok, const char *user_text, const char *reply_first);
void soul_on_barge(void);
void soul_on_voice(void);            /* 内置 ≥5s 限频(总线回调可直调) */

/* ---- LLM 回复的 mood 标签协议 ---- */
/* 从回复尾部剥掉 <mood d="±0.x"/> 标签; 返回 true=剥到并已受理重评价
 * (tick 内施加, 有界 ±0.2 越界拒)。text 原地截短。 */
bool soul_strip_mood_tag(char *text);

/* ---- 消费(快照拷贝, 短临界区) ---- */
typedef struct {
    float rate;                      /* TTS 语速 [0.8,1.2] */
    float pitch;                     /* TTS 音高 [-2,+2] 半音 */
} soul_tts_hint_t;
void soul_tts_hint(soul_tts_hint_t *out);

/* 全量状态快照(soul_think/UI/调试用) */
typedef struct {
    float valence, arousal, urgency, social_bond;
    float drive_level[3];            /* social/stim/rest */
    char  anim_tag[16];
    bool  ethics_latched;
    bool  ai_idle;                   /* 会话空闲 */
    int   idle_sec;                  /* 距上次非空闲拍秒数 */
} soul_state_t;
bool soul_state_get(soul_state_t *out);

/* 内在状态 prompt 段(内部静态缓冲, 调用方立刻拼走; session 任务上下文) */
const char *soul_prompt_suffix(void);

/* 记忆块 prompt 段(soul_mem; 空库返回空态文案防幻觉) */
const char *soul_mem_prompt_block(void);

#ifdef __cplusplus
}
#endif
