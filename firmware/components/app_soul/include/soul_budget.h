/*
 * soul_budget.h — 深思行动预算分账 + recency 惩罚(S3, docs/24 §4.5/§6)
 *
 * 行为来源大原则(R6, docs/24 §0.1): 本地零行动生成——本件不生成行动,
 * 只对行动出口做**约束类硬门**执法(docs/17 §4.3: 音频≤3/h·同类冷却
 * 30min, 对 LLM 深思行动同样执法)与分账日帽(§4.5: speak≤4/music≤6/
 * face≤6, 替代终身制单一 fired_today)。
 * E3 习惯化(docs/23 §2.5)迁移为深思行动级 recency(§6): 旧 L2 表的
 * hab/权重调制退役, 概念活在本件 sb_recency(prompt 配额可见+S6 多
 * 提议排序惩罚面)。
 * 令牌桶触发预算(§4.2)=S5 批, 不在本件。
 * 纯 C 零平台依赖(宿主可测); 确定性: 零 RNG/零分配; 日翻转由调用方
 * now_s 推进(单调秒, 跨 86400 即新账日)。
 */
#ifndef SOUL_BUDGET_H
#define SOUL_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 行动类别(深思词表 speak/face/music 的分账面) */
enum sb_cat {
    SB_SPEAK = 0,       /* 主动说话(TTS 音频)            */
    SB_MUSIC,           /* 放歌/切台(音频)               */
    SB_FACE,            /* 换表情(非音频)                */
    SB_CNT
};

struct sb_cfg {
    uint8_t  daily_cap[SB_CNT];  /* 分账日帽(§4.5 初值 4/6/6)      */
    uint8_t  audio_per_hour;     /* 主动音频时窗帽(speak+music 合计) */
    uint32_t cat_cooldown_s;     /* 同类冷却(§4.3 初值 30min)      */
    uint32_t recency_tau_s;      /* 新鲜度恢复 τ(E3 迁移, 30min)   */
};

/* 复位(测试/出厂); 并恢复默认参数 */
void sb_reset(void);

/* 覆写参数(NULL=恢复默认); 日中生效(已记账不清) */
void sb_configure(const struct sb_cfg *cfg);

/* 硬门查询(不记账): 日帽内 ∧ 音频时窗内(仅音频类) ∧ 过同类冷却 */
bool sb_allow(int cat, uint32_t now_s);

/* 执行后记账(日翻转自动结算; 音频类入时窗环) */
void sb_charge(int cat, uint32_t now_s);

/* E3 迁移: 类别新鲜度 [0,1](1=久未执行, τ 内线性恢复; 从未执行=1)。
 * 消费面: prompt v2 配额/新鲜度可见 + S6 多提议排序惩罚项。 */
float sb_recency(int cat, uint32_t now_s);

/* 配额可见快照(prompt v2/CLI): 仅分账日帽段
 * "今日说话 1/4 切歌 0/6 表情 2/6"(时窗/冷却动态不在此——prompt 侧
 * 以 sb_recency 新鲜度传达, 执法在 sb_allow); cat 越界忽略 */
void sb_snapshot(char *buf, size_t cap);

/* 遥测: 该类今日已执行次数(cat 越界返回 255) */
uint8_t sb_fired_today(int cat);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_BUDGET_H */
