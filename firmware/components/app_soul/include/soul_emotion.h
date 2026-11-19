/*
 * kaleido_emotion.h — 九维感受态情感引擎（Galatea L05 affect 的 C 提取）
 *
 * 设计依据：docs/research/18-Galatea情感模型提取评估.md §2/§4 +
 * 28 号深挖（W0-2 对源偏差修正 D1-D7 落地）。
 *  - 确定性动力学：一阶低通（τ_int=4/τ_dec=16 拍）、迟滞门、饱和、
 *    一次性重评价——零 RNG、零墙钟（tick 由调用方供给，1Hz 拍）
 *  - 安全范式整体继承：LLM 禁直写情感数值（D-13）；重评价唯一例外通道
 *    = 有界 delta + 回执（28 号 D4：改一次性精确施加，applied 可审计）
 *  - Kaleido 特有：social_bond 维、ethics_latched 伦理闩锁（极端持续
 *    负面 → 只允许安抚模板；ke_ethics_clear 解锁=owner 批准语义）
 *
 * 拍语义：1 拍 = 1Hz 调用一次 ke_step()；τ 以拍计。
 */
#ifndef KALEIDO_EMOTION_H
#define KALEIDO_EMOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 感受态（九维；18 号 §2.1 的 Kaleido 剪裁版）---- */

struct ke_state {
	float valence;         /* [-1,1] 显示效价 = 0.65×快层+0.35×mood(docs/24 §2.1) */
	float mood;            /* [-1,1] 慢层心境(τ≈3h, 三水库满足度导出; docs/24 §2.1) */
	float arousal;         /* [0,1]                            */
	float urgency;         /* [0,1]                            */
	float persistence;     /* [0,1] 慢谱（|valence| 滤波; S4 随 trait 重审） */
	float controllability; /* [0,1]                            */
	float novelty;         /* [0,1] 感觉预测误差供给           */
	float uncertainty;     /* [0,1] 信念不确定性供给            */
	float social_bond;     /* [0,1] 互动强度积分（Kaleido 特有） */
	bool ethics_latched;   /* 极端持续负面闩锁（只允许安抚模板） */
	float ethics_load;     /* 闩锁负荷遥测 [0,∞)（docs/23 E2b 遗留收口） */
};

/* ---- 每拍输入 ---- */

/* 执行回执三态（G1：无事件拍保持 controllability；一轮结算拍喂一次） */
enum ke_receipt {
	KE_RECEIPT_NONE = 0,
	KE_RECEIPT_SUCCESS,
	KE_RECEIPT_FAIL,
};

struct ke_inputs {
	bool have_drive;         /* 驱力误差输入是否有效（无效拍
				    arousal/urgency 保持——D5 源缺席语义） */
	float drive_error_sum;   /* E = Σw·e（**sum 非 mean**，28 号 D1：
				    kd error_sum 直供；raw valence 域随
				    权重与驱力数，发布 clip [−1,1]）   */
	float drive_error_max;   /* max e_d（arousal/urgency 源）      */
	float drive_sat[3];      /* 三驱力水位 [0,1]（social/stim/rest;
				    mood 慢层源——AIBO 满足度口径,
				    docs/24 §2.1; have_drive 时有效）  */
	enum ke_receipt receipt; /* 执行回执（→controllability）       */
	bool have_residual;      /* novelty 输入有效性位（D7：无效拍保持）*/
	float sensory_residual_mean; /* 感觉预测误差 → novelty         */
	bool have_uncertainty;   /* uncertainty 输入有效性位（D7）      */
	float belief_uncertainty_mean; /* 1−p_max → uncertainty        */
	bool social_event;       /* 本拍发生真实互动（→social_bond）   */
	bool soothe_event;       /* 温和互动安抚(voice/touch/正面轮;
				    闩锁期减载, docs/24 §2.2)         */
};

/* ---- 输出（消费方：express/动画、speech/TTS hint、LLM prompt 注入）---- */

#define KE_ANIM_TAG_MAX 16

struct ke_output {
	struct ke_state state;
	char anim_tag[KE_ANIM_TAG_MAX];  /* valence×arousal 平面查表     */
	struct {
		float rate;                  /* TTS 语速调制 [0.8,1.2]      */
		float pitch;                 /* 音高调制 [-2,+2] 半音        */
	} tts_hint;
	bool ignition_candidate;         /* urgency 过激活门（>0.1；
					    Kaleido 附加迟滞，非源语义） */
	uint8_t hyst_saturated : 1;      /* 饱和命中（本拍）             */
};

/*
 * 重评价请求（LLM → 情感的唯一合法通道；有界+回执）。
 * 28 号 D4：一次性精确施加——下一拍把钳位后的 delta 直接加进积分器
 * （禁历史改写；TTL 斜坡已废，实际施加量=请求值）。回执经
 * ke_reappraisal_receipt 查询、ke_reappraisal_applied 审计。
 */
struct ke_reappraisal {
	float delta;    /* ∈ [-0.2,+0.2] 有界（越界整单拒绝）  */
	uint64_t receipt_id; /* 回执编号（调用方生成，>0）     */
};

/* 重评价三件套（评审 C3 契约）：与 ke_step **同任务**调用（组件
 * 文件级 static 无锁——soul tick 单写者持有；异任务请求须经调用方
 * 侧的 pending 漏斗，如 soul_on_mood）。返回 0 成功；-1 参数错
 * （req 空/receipt_id 0）；-2 delta 越界。 */
int ke_reappraisal_request(const struct ke_reappraisal *req);
bool ke_reappraisal_receipt(uint64_t receipt_id); /* 是否已生效 */
float ke_reappraisal_applied(void);   /* 最近一次实际施加量（审计用） */

/* 引擎：ke_step 每拍调用（tick 预留未消费；台账键=调用方 receipt_id，
 * 评审 C7）。ke_state_snapshot 返回内部指针——仅当拍借用，ke_reset
 * 后失效；跨任务取状态用拷贝（soul_snapshot 模式）。 */
void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out);
const struct ke_state *ke_state_snapshot(void);
void ke_reset(const struct ke_state *init); /* NULL=全零 */
/* 伦理闩锁解锁——显式重置(owner/出厂路径)。常规解锁走动力学:
 * 时间自愈+安抚加速+施密特(docs/24 §2.2), 无需调用本函数 */
void ke_ethics_clear(void);

/* S2.5 trait 调制面(docs/24 §3.2+§11.3; 运行期可调, 默认=中性常数=锚)。
 * trait 层(soul_trait)设置; 引擎各处读 s_tune 而非冻结宏。 */
struct ke_tune {
	float arousal_base;     /* [0,0.20]  基线(活力)                    */
	float valence_off;      /* [-0.05,0.05] 显示偏移(温情)             */
	float tau_dec_mod;      /* [0.8,1.2]×  慢衰减调制(韧性)            */
	float bond_up_mod;      /* [0.75,1.25]× bond 上行(温情)            */
	float urg_on;           /* [0.08,0.12]  urgency 门(主见)           */
	float novelty_gain_mod; /* [0.7,1.3]×   novelty 增益(好奇)         */
	float eth_gate;         /* [0.10,0.55] 深度门(脆弱度)              */
	float eth_dose_mod;     /* [0.6,1.5]×  事件剂量(脆弱度)            */
	float eth_load_t;       /* [1.6,3.2]   负荷阈(脆弱度)              */
	float eth_heal_tau_mod; /* [0.5,1.67]× 自愈 τ(脆弱度)              */
	float eth_soothe_mod;   /* [0.75,1.25]× 安抚效率(温情)             */
};
void ke_tune_set(const struct ke_tune *t);   /* NULL=恢复中性默认 */

/* 动力学原语（测试与 ignition 复用；18 号 §2.2 六算子）
 * ke_hysteresis：band 为**半带宽**（源 dynamics.py:34-40 语义，
 * band=0.1 ⇒ ON ≥0.55 / OFF ≤0.45，边界含）——28 号 D6 修正 */
float ke_lowpass(float s, float x, float tau);
bool ke_hysteresis(bool on, float x, float band);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_EMOTION_H */
