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

#include "soul_trait.h"
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "soul_ignition.h"    /* ki_think_ctx/ki_intent(W3 思维协议) */

/* 启动 1Hz 灵魂任务(幂等); 依赖 NVS 已初始化(记忆层) */
esp_err_t soul_start(void);

/* 灵魂 2.0 深思轮(W3): brain=深思 LLM 通道注入(整句收集), speak=出声
 * 通道注入(announce); 行动 face/music 经 CUBE_EVT_SOUL_ACT 广播 */
void soul_think_set_brain(int (*fn)(const char *sys, char *reply, size_t cap));
void soul_think_set_speak(esp_err_t (*fn)(const char *text));
esp_err_t soul_think_start(void);
void soul_think_gate_ctx(struct ki_think_ctx *tc);   /* tick 供上下文 */
bool soul_think_fire(uint8_t reason_mask, bool forced, float score);
void soul_think_note_voice(void);   /* 用户输入侧记(结算窗证据) */
bool soul_think_busy(void);

/* ---- 事件钩子(任意任务; 内部 pend, tick 消费) ---- */
void soul_on_round(bool ok, const char *user_text, const char *reply_first);
/* 用户显式停止归因(T13: 入口调用, 替代旧时间窗猜测——docs/15 P0-3):
 * radio_start 退避 + 日志; 锁存由调用方 cact_user_stop 负责 */
void soul_note_user_stop(void);
void soul_on_barge(void);
void soul_on_voice(void);            /* 内置 ≥5s 限频(总线回调可直调) */

/* ---- trait 慢变量注入(S2.5b persona 持久化; act_gate 调用) ----
 * 建角色(genesis)/角色加载后由 persona owner 推入; 内部 trait_set
 * (band 钳位+调制面重算)。越界维拒绝整包(false)。 */
void soul_trait_load(const float tr[TRAIT_DIM]);   /* TRAIT_DIM=5 */

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
    bool  audio_user_stopped;        /* 用户停台抑制窗内(S3: 深思 music
                                      * 行动硬门, docs/17 §5.5) */
    int   idle_sec;                  /* 距上次非空闲拍秒数 */
} soul_state_t;
bool soul_state_get(soul_state_t *out);

/* 内在状态 prompt 段(内部静态缓冲, 调用方立刻拼走; session 任务上下文) */
const char *soul_prompt_suffix(void);

/* 记忆块 prompt 段(soul_mem; 空库返回空态文案防幻觉) */
const char *soul_mem_prompt_block(void);

/* 记录一条记忆(轮末由 soul/ai_session 调; 批次 44 收进头文件——
 * 此前 cli.c/soul.c 各自手写 extern, 签名漂移隐患) */
void soul_mem_record(const char *user, const char *reply,
                     float valence, float arousal);
/* 启动装载(SD→内存, 迟挂载由 app_storage 事件补触发) */
void soul_mem_boot(void);

/* persona 作用域绑定(T21; docs/17 §7.2): key=persona_id hex;
 * consent_mem=false 时长期记忆不写(已有可读)。切换角色/授权变更时
 * 由装配层调用。 */
void soul_mem_scope_bind(const char *persona_key, bool consent_mem);
void soul_mem_query_rebuild(const char *query);   /* 按当前 query 检索精排(T21 续) */

/* 删除(T23; docs/17 §7.2/§8.2): 台账先行防复活 */
typedef enum {
    SOUL_MEM_DEL_OK = 0,       /* 持久接受(内存+台账+重写完成) */
    SOUL_MEM_DEL_PENDING,      /* RAM 已隐藏, 台账未落盘(未持久接受) */
    SOUL_MEM_DEL_NOT_FOUND,
    SOUL_MEM_DEL_REJECTED,     /* 未绑定/停用/锁超时 */
} soul_mem_del_res_t;
soul_mem_del_res_t soul_mem_delete(uint32_t seq);

#ifdef __cplusplus
}
#endif
