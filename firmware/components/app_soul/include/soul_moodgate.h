/*
 * soul_moodgate.h — LLM mood 回写三断法(D4, S6; docs/24 §5.4)
 *
 * 对话 LLM 的 <mood d="±0.x"/> 标签是"回写通道"——防自增强闭环
 * (Sentipolis/Loona 反面: LLM 自评喂回自身→漂移):
 *   断一·隔轮采纳: ≤1/2 轮生效(其余轮丢弃, 不积累);
 *   断二·×0.5 衰减: 生效值乘 0.5 再施加(永远小于 LLM 主张);
 *   断三·方向校验: 与引擎当拍快层重评价方向失配 → 再 ×0.5;
 * 自增强指纹: 最近 50 次生效回写的 AR(1)>0.8(强自相关=回写在喂自
 * 己)→ 回写通道暂停 24h(ke 重评价直通, 机械面不受影响)。
 * 纯 C 零平台依赖(宿主可测); 确定性; 时基由调用方供给。
 */
#ifndef SOUL_MOODGATE_H
#define SOUL_MOODGATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 裁决一次回写(每轮对话末调用)。
 * llm_delta=LLM 标签原始值; engine_trend=引擎快层当拍方向
 * (重评价通道近 N 轮的符号; 0=中性, 同号=0/异号=非 0);
 * round=对话轮序(单调递增); applied=实际应施加值(已过三断)。
 * 返回 true=本轮允许施加(applied 有效); false=隔轮丢弃。 */
bool soul_moodgate_allow(uint32_t round, int engine_trend,
                         float llm_delta, float *applied);

/* 生效回写记录(施加成功后调; AR(1) 自增强指纹统计) */
void soul_moodgate_record(float applied);

/* 回写通道是否被自增强指纹暂停(暂停期间 allow 恒 false) */
bool soul_moodgate_paused(uint32_t now_s);

/* 复位(测试) */
void soul_moodgate_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_MOODGATE_H */
