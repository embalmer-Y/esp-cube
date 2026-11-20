/*
 * soul_trait.h — 性格 trait 层(S2.5a, docs/24 §3+§11.3/§11.6 已确认)
 *
 * 项目目的(docs/16 最高原则): 有血有肉的个人伙伴, 反应模式由性格决定——
 * "什么时候进入什么状态"是性格(自由), "进入后系统做什么"是固定安全行为。
 * 5 维(好奇/温情/活力/主见/韧性)全程不称大五(docs/17 §7.4 边界)。
 *
 * S2.5a 范围: 静态底座+全调制面(ke 调谐/kd 参数覆写), trait_set 即重算。
 * S2.5b 范围(docs/24 §3.3/§3.5): 双通道更新——
 *   通道一·统计 EMA(Whole Trait): 日界结算, x[d]=残差密度(减去 trait
 *     预测基线, 防 ALMA 式自激励正反馈);
 *   通道二·反思跃迁(GA): LLM 洞察的 trait_delta 全量本地后校验;
 *   互斥(同周期取 |Δ| 大者)+熔断(7 日净移>0.75×band 冻结该维 7 天)。
 * genesis 面试=宿主工具(tools/genesis.py), 经 trait_set 进入, 不在本层。
 * 纯 C 零平台依赖(宿主可测, 与 kd/ke/soul_face 同纪律)。
 */
#ifndef SOUL_TRAIT_H
#define SOUL_TRAIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRAIT_DIM 5

enum {
    TR_CURIOSITY = 0,   /* 好奇: novelty 增益/素材新颖偏好        */
    TR_WARMTH,          /* 温情: 依恋(bond/社交代谢/易哄好)        */
    TR_ENERGY,          /* 活力: arousal 基线/自娱自足            */
    TR_ASSERT,          /* 主见: urgency 容忍                     */
    TR_RESILIENCE,      /* 韧性: 负面消退/自愈/抗闩锁             */
};

/* 底座与带宽(docs/24 §3.3: band ±0.15, 用户只可在建角色时定, 之外不可干预) */
#define TRAIT_BAND 0.15f

/*
 * trait 调制系数全集(由 5 维导出, 引擎只读)。
 * 语义/公式出处: docs/24 §3.2(ke 白名单)+§11.3(ethics/kd 性格化)。
 */
typedef struct {
    /* --- ke: 情绪/表达调制 --- */
    float arousal_base;     /* [0.00,0.20] = 0.10+0.20*(E-0.5)      */
    float valence_off;      /* [-0.05,0.05] = 0.10*(W-0.5), 显示侧  */
    float tau_dec_mod;      /* [0.8,1.2]×  慢衰减(韧性)             */
    float bond_up_mod;      /* [0.75,1.25]× bond 上行(温情)         */
    float urg_on;           /* [0.08,0.12]  urgency 门(主见)        */
    float novelty_gain_mod; /* [0.7,1.3]×   novelty 增益(好奇)      */
    /* --- ethics: 触发面性格化(§11.3; 闩锁后输出行为不调制) --- */
    float frag;             /* 脆弱度 [0,1] = 0.6*(1-R)+0.4*W       */
    float eth_gate;         /* 深度门 [0.08,0.55]: frag=1 纯冷落可闩 */
    float eth_dose_mod;     /* [0.6,1.5]×  事件剂量(脆弱者伤更深)   */
    float eth_load_t;       /* [1.6,3.2]   负荷阈                   */
    float eth_heal_tau_mod; /* [0.5,1.67]× 自愈 τ(脆弱者难自愈)     */
    float eth_soothe_mod;   /* [0.75,1.25]× 安抚效率(温情易哄好)    */
    /* --- kd: 节奏性格化 --- */
    float social_met_mod;   /* [0.5,1.5]×  社交代谢(温情=黏人)      */
    float stim_self_recover;/* [0,8e-5]/s  自娱回充(活力=自足; §11.3 初值实施回调) */
} trait_tune_t;

/* 复位到底座 0.5 全中性(等效 S2 行为)。 */
void trait_reset(void);

/* 设 trait(逐维钳位到底座±带宽); 成功后重算调制系数并**立即**应用到
 * ke(ke_tune_set)与 kd(trait_tune_apply_kd 参数覆写)。返回 false=参数越界。 */
bool trait_set(const float tr[TRAIT_DIM]);

void trait_get(float tr[TRAIT_DIM]);
const trait_tune_t *trait_tune(void);     /* 当前调制系数(只读)      */

/* 把 kd 调制(social_met_mod/stim_self_recover)写入 kd_params 覆写
 * (基于内置冻结表 × 调制; trait_set 已自动调用, 独立调用供重挂场景)。 */
void trait_tune_apply_kd(void);

/* ---- S2.5b: 双通道更新(docs/24 §3.3 定案; §3.5 参数表) ---- */

/* 每拍统计观测(调用方=soul 任务/模拟器 1Hz)。x[d] 由这些观测在日界
 * 结算为"残差密度"(观测密度 − 生活基线常数 − trait 预测增量,
 * docs/24 §3.3 关键: 防"trait 抬基线→状态更高→又推 trait"正反馈;
 * 基线常数=30 天 life 标定, 见 soul_trait.c TR_K_*)。 */
struct trait_obs {
    float arousal;      /* ke arousal [0,1]                     */
    float salience;     /* 黑板显著性 [0,1](新异事件流密度代理: ke
                         * novelty 无喂入源=结构性死通道, v1 以
                         * salience 代理; novelty_gain_mod 仍为其调制) */
    float urgency;      /* ke urgency [0,1]                     */
    float social_bond;  /* ke social_bond [0,1]                 */
    float valence;      /* ke 显示效价 [-1,1]                    */
    bool  initiative;   /* 本拍自发发起(深思行动/点火出口)       */
};

void trait_stat_observe(const struct trait_obs *o);  /* 每拍喂(零拷贝) */

/* 日界结算(每 24h 一次, 调用方定日界)。通道一 EMA 步:
 *   delta = 0.03·(x[d]−tr[d]) + 0.005·(0.5−tr[d]), 钳 ±0.05;
 * 互斥: 当日反思已施加 |Δ|≥EMA 的维, EMA 让步(取大者不求和);
 * 熔断: 结算后检 7 日净移>0.75×band → 该维冻结 7 天(自动护栏,
 * docs/24 §11.2: 非用户干预面)。无观测日不计入 7 日窗( warmup≥7)。 */
void trait_ema_settle(void);

/* 通道二·反思跃迁(docs/24 §3.3): 深思轮 LLM 洞察 → trait_delta,
 * 本地全量后校验, 逐维独立生效/拒绝:
 *   - 引用深度 ≤2(防脱根); 置信 ∈[0,1]
 *   - 逐维钳 ±0.05; 目标值钳 band
 *   - 连续同向(与上次该维方向一致)需 evidence≥2 且 conf≥0.6
 *   - 每日 ≤1 次(24h 冷却以结算日粒度执法; 占深思配额属 S3 预算分账)
 * 全拒绝返回 false 且状态不动; 生效即重算调制面。 */
struct trait_reflect {
    float   delta[TRAIT_DIM]; /* 带符号变化(原始, 越界钳到限幅) */
    float   conf;             /* [0,1] 洞察置信                  */
    uint8_t evidence_n;       /* 引用记忆条数                    */
    uint8_t ref_depth;        /* 反思引用深度(≤2)                */
};
bool trait_reflect_apply(const struct trait_reflect *r);

/* 熔断/计数遥测(透明性 B4 只读; 测试锚) */
bool trait_frozen(int dim);          /* 该维冻结中           */
uint32_t trait_day_count(void);      /* 已结算天数           */
float trait_last_ema(int dim);       /* 最近一次 EMA 步(审计)*/

#ifdef __cplusplus
}
#endif
#endif /* SOUL_TRAIT_H */
