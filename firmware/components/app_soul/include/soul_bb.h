/*
 * soul_bb.h —— L1 因子黑板(统一因子汇聚点, docs/14 §4)
 *
 * 全库唯一因子读写面: 情绪(ke)/动机(kd 缺口+预留)/情境/机器状态
 * (预留)全部落键值, 评分器(soul_behavior)/深思(soul_think)/CLI/
 * prompt 渲染同源读取 —— 三融合点之一(docs/13 §3)。
 *
 * 纪律:
 *   - 固定键枚举+名字表, 零堆零动态注册(MCU 口径; 加因子=加枚举位,
 *     骨架零改动);
 *   - 单写者 = soul tick(soul_bb_set/soul_bb_tick 仅其调用);
 *   - 读侧无锁: 对齐 32 位 float 写原子(Xtensa/RISC-V);
 *   - 调试覆写槽(bbset): 带 TTL, portMUX 保护, 测试期强因子唯一通道
 *     (tick 不覆写未过期槽 —— 黄金值能活过下一拍)。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* ---- 情绪组(ke 直供) ---- */
    SOUL_BB_VALENCE = 0,    /* [-1,1] */
    SOUL_BB_AROUSAL,        /* [0,1]  活力            */
    SOUL_BB_URGENCY,        /* [0,1]  表达冲动        */
    SOUL_BB_BOND,           /* [0,1]  社交联结积分    */
    SOUL_BB_ETHICS,         /* 0/1    安抚模式闩锁    */
    /* ---- 动机组(kd 误差=缺口; 预留维 Phase B 填动力学) ---- */
    SOUL_BB_M_SOCIAL,       /* 社交水库缺口 1-level   */
    SOUL_BB_M_STIM,         /* 刺激调节缺口           */
    SOUL_BB_M_EXPR,         /* 表达冲动队列(预留 0)   */
    SOUL_BB_M_CURIOS,       /* 好奇信息缺口(预留 0)   */
    SOUL_BB_M_COMP,         /* 胜任心流(预留 0)       */
    /* ---- 情境组(感知直供) ---- */
    SOUL_BB_IDLE,           /* 无互动时长/7200s 饱和  */
    SOUL_BB_QUIET,          /* 勿扰时段 0/1           */
    SOUL_BB_AI_ACTIVE,      /* 会话进行中 0/1         */
    SOUL_BB_PLAYING,        /* 音频播放/连接中 0/1    */
    SOUL_BB_PRESENCE,       /* 在场置信(预留 0)       */
    SOUL_BB_NET,            /* 网络可用 0/1           */
    SOUL_BB_SALIENCE,       /* 事件显著性(充值+衰减)  */
    SOUL_BB_MEM_HOOK,       /* 记忆钩子(预留 0)       */
    /* ---- 机器状态组(Phase C 接线) ---- */
    SOUL_BB_BATT,           /* 电量 0-1(默认 1)       */
    SOUL_BB_TEMP,           /* 热载荷 0-1(默认 0)     */
    SOUL_BB_CHARGING,       /* 充电中 0/1             */
    SOUL_BB_TOUCH,          /* 即时触摸(衰减, 预留 0) */
    SOUL_BB_CNT
} soul_bb_key_t;

/* 键名(短名, CLI/日志用; 与枚举同序) */
const char *soul_bb_name(soul_bb_key_t key);

/* 因子读(覆写槽优先); 越界键返回 0 */
float soul_bb_get(soul_bb_key_t key);

/* 因子写(仅 soul tick 单写者; 覆写槽生效期间落到影子, 不破坏台调值) */
void soul_bb_set(soul_bb_key_t key, float v);

/* 显著性充值(事件感知路径, 取 max 不叠加 —— 防事件洪峰顶满) */
void soul_bb_salience(float v);

/* 每拍衰减与过期(soul tick 调, sec 单调秒):
 * SALIENCE/TOUCH τ≈1min 指数衰减; 覆写槽到期回收 */
void soul_bb_tick(uint32_t now_s);

/* 调试覆写(CLI bbset): val 生效 ttl_s 秒(0=立即清除该槽) */
void soul_bb_override(soul_bb_key_t key, float val, uint32_t ttl_s);

/* 全量 dump(一行键值, CLI/日志; 返回写入字节数) */
size_t soul_bb_dump(char *buf, size_t cap);

/* 测试钩子: 清全部状态(含覆写) */
void soul_bb_reset(void);

#ifdef __cplusplus
}
#endif
