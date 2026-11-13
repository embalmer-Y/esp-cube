/*
 * soul_behavior.h —— L2 Utility 评分器 + L3 行为注册表(docs/14 §5)
 *
 * score(b) = w(b) × ∏ curve(bb 因子); 仲裁 = 阈值 ∧ 冷却 ∧ 退避
 * → 候选集按 score 加权随机 WTA(xorshift 确定性 RNG, 宿主可测)。
 * 执行器为注入回调(发布 SOUL_ACT / 直调能力) —— 本件纯 C 零平台
 * 依赖, 只读黑板(soul_bb), 不含事件发布(分层: 行动出口在 soul.c)。
 *
 * 定位纪律(docs/13 §3): 本评分器是"因子→行为"的唯一汇聚; ki 深思
 * 路径(L4)不经此 —— 命令式(会话技能)与自主式(本件)共享同一行动
 * 词汇表(CUBE_EVT_SOUL_ACT)与执行器, 但决策各自独立。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "soul_bb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOUL_BHV_MAX 8          /* 静态注册表容量 */

/* 响应曲线: consideration = f(factor), 输出钳位 [0,1] */
typedef enum {
    SOUL_C_RISE = 0,            /* clamp01(x·k): 因子越大越想做   */
    SOUL_C_FALL,                /* clamp01(1−x·k): 因子越大越不做 */
    SOUL_C_BAND,                /* 1−|1−2·clamp01(x·k)|: 中间最优 */
} soul_curve_t;

typedef struct {
    soul_bb_key_t key;          /* 因子键                       */
    soul_curve_t  curve;
    float         k;            /* 增益(1=直通)                 */
} soul_cons_t;

typedef struct {
    const char       *name;
    const soul_cons_t *cons;    /* 考虑项(乘法合成, 空表=恒 1)  */
    uint8_t           cons_n;
    float             threshold;/* 候选阈值                     */
    float             w;        /* 基础偏好(BigFive 点积预留=1) */
    uint16_t          cooldown_s;   /* 执行后冷却              */
    void            (*exec)(void); /* 执行器(注入; NULL=只评不执) */
} soul_bhv_t;

/* 注册(幂等: 同名重注册=更新); 返回 id, 表满/-1 */
int soul_behavior_register(const soul_bhv_t *b);

/* 外部抑制(退避): 用户手动停台→radio_start 退避 2h 这类"直控学习" */
void soul_behavior_defer(const char *name, uint32_t sec);

/* 查询: 是否处于退避期(开机反射等处判断"用户是否已表达拒绝") */
bool soul_behavior_deferred(const char *name);

/* 评分(soul tick 1Hz 调): 评分→仲裁→胜者执行(冷却/退避记账) */
void soul_behavior_tick(uint32_t now_s);

/* 纯评估(不执行/不记账; 测试与 bh 观测用); id 越界返回 -1 */
float soul_behavior_score(int id);

/* 最近一次执行的单调秒(id 越界返回 0); 测试断言用 */
uint32_t soul_behavior_last_fire(int id);

/* 表 dump(名/分/阈值/冷却余/退避余; CLI bh) */
void soul_behavior_dump(char *buf, size_t cap);

/* 测试钩子: RNG 种子 + 全表复位 */
void soul_behavior_set_seed(uint32_t seed);
void soul_behavior_reset(void);

#ifdef __cplusplus
}
#endif
