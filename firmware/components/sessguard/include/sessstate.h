/* sessstate.h —— 会话生命周期状态机(T28 宿主可测语义; docs/18 T28)
 *
 * 语义(T28 验收逐条):
 *   - 连续多轮: SPEAKING 收口 → LISTENING(无需重新唤醒); 轮数上限 → IDLE;
 *   - VAD 定稿: LISTENING → THINKING(语音定稿进 LLM);
 *   - 超时: 会话级静默超时 → IDLE(停止会话采集, 恢复唤醒监听);
 *   - 双占麦防护: 采集权单 owner —— LISTENING 期间的二次采集请求拒绝;
 *   - 取消清理: 任意状态 cancel → 返回清理掩码(停采集/停 TTS/失效门),
 *     跨角数据由 sessguard 代次门兜底(调用方联动)。
 *
 * 与 ai_state(既有展示状态)解耦: 本机是行为机, 展示层另行映射。
 * 纯逻辑无 I/O(宿主可测)。
 */
#ifndef SESSSTATE_H
#define SESSSTATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SS_IDLE = 0,           /* 唤醒监听(会话未开; 采集权=唤醒引擎) */
    SS_WAKE,               /* 唤醒命中(过渡) */
    SS_LISTENING,          /* 会话采集中(采集权=ASR) */
    SS_THINKING,           /* VAD 定稿 → LLM */
    SS_SPEAKING,           /* 回答播放中(采集权=无) */
} ss_state_t;

/* 取消清理掩码 */
#define SS_CLEAN_CAPTURE 0x1u   /* 停止会话采集 */
#define SS_CLEAN_TTS     0x2u   /* 停止/丢弃 TTS(无旧声) */
#define SS_CLEAN_GUARD   0x4u   /* 会话守卫全失效(跨角/旧请求) */

typedef struct sessstate sessstate_t;

int ss_init(sessstate_t **out, uint32_t turn_max, uint64_t idle_timeout_ms);
void ss_free(sessstate_t *ss);

ss_state_t ss_state(const sessstate_t *ss);
uint32_t ss_turns(const sessstate_t *ss);       /* 本会话已完成轮数 */

/* 事件(非法转移返回 false 且状态不变 —— 状态机防护语义) */
bool ss_on_wake(sessstate_t *ss);
bool ss_on_speech_start(sessstate_t *ss);       /* WAKE→LISTENING(开采集) */
bool ss_on_speech_end(sessstate_t *ss);         /* LISTENING→THINKING(VAD 定稿) */
bool ss_on_reply_done(sessstate_t *ss);         /* THINKING/SPEAKING→LISTENING
                                                 * (连续多轮; 轮满→IDLE) */
/* 会话级静默超时(任意状态; IDLE 无会话恒 false): 到点→IDLE+清理掩码 */
uint32_t ss_on_timeout(sessstate_t *ss, uint64_t now_ms);
/* 会话静默起算点(回复收口/进 LISTENING 时刷新; SS_IDLE 时无意义) */
void ss_touch(sessstate_t *ss, uint64_t now_ms);

/* 采集权防护: 仅 LISTENING 可开采集; 已开时二次请求拒绝(双占麦) */
bool ss_capture_request(sessstate_t *ss);
bool ss_capture_active(const sessstate_t *ss);

/* 任意状态取消: 返回清理掩码(0=无会话), 状态→IDLE(恢复唤醒) */
uint32_t ss_cancel(sessstate_t *ss);

#ifdef __cplusplus
}
#endif
#endif /* SESSSTATE_H */
