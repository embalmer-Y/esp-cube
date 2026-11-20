/*
 * test_soul_trait.c — trait 层黄金回归(S2.5a 静态锚+S2.5b 双通道, docs/24)
 *
 * 锚原则(核心契约): 性格全 0.5 ⇒ 调制系数 ≡ S2 中性常数——
 * ke/kd 全部黄金锚不因 trait 层引入而漂移。
 * S2.5b 锚: 无观测不动; x=0.5 中性密度不动; 单步≤0.05; band 钳位;
 * 互斥取大者; 反思五门(深度/冷却/同向证据/限幅/band); 熔断可入可出。
 */
#include <math.h>
#include <string.h>

#include "soul_drive.h"
#include "soul_emotion.h"
#include "soul_trait.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 锚: 中性 = S2 常数全集 */
static void golden_anchor_neutral(void)
{
	const trait_tune_t *t;

	trait_reset();
	t = trait_tune();
	TEST_ASSERT_TRUE(feq(t->arousal_base, 0.10f));
	TEST_ASSERT_TRUE(feq(t->valence_off, 0.0f));
	TEST_ASSERT_TRUE(feq(t->tau_dec_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->bond_up_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->urg_on, 0.10f));
	TEST_ASSERT_TRUE(feq(t->novelty_gain_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->eth_gate, 0.35f));
	TEST_ASSERT_TRUE(feq(t->eth_dose_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->eth_load_t, 2.5f));
	TEST_ASSERT_TRUE(feq(t->eth_heal_tau_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->eth_soothe_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->social_met_mod, 1.0f));
	TEST_ASSERT_TRUE(feq(t->stim_self_recover, 0.0f));
}

/* 脆弱/自足两极(soul_accept C4 同款)的调制端点 */
static void golden_extremes(void)
{
	const float fragile[TRAIT_DIM] = { 0.35f, 0.65f, 0.35f, 0.50f, 0.35f };
	const float resil[TRAIT_DIM]   = { 0.65f, 0.35f, 0.65f, 0.50f, 0.65f };
	const trait_tune_t *t;

	TEST_ASSERT_TRUE(trait_set(fragile));
	t = trait_tune();
	/* frag = 0.6*0.65+0.4*0.65 = 0.65 */
	TEST_ASSERT_TRUE(feq(t->frag, 0.65f));
	TEST_ASSERT_TRUE(feq(t->eth_gate, 0.185f));   /* < 冷落 |v|≈0.21 → 可闩 */
	TEST_ASSERT_TRUE(feq(t->eth_load_t, 2.23f));
	TEST_ASSERT_TRUE(feq(t->stim_self_recover, 0.0f));
	TEST_ASSERT_TRUE(feq(t->social_met_mod, 1.15f));  /* 黏人 */

	TEST_ASSERT_TRUE(trait_set(resil));
	t = trait_tune();
	TEST_ASSERT_TRUE(feq(t->frag, 0.35f));
	TEST_ASSERT_TRUE(feq(t->eth_gate, 0.515f));   /* 冷落远不可达 */
	TEST_ASSERT_TRUE(feq(t->stim_self_recover, 8.0e-5f)); /* 2×代谢 */
	TEST_ASSERT_TRUE(feq(t->social_met_mod, 0.85f));

	trait_reset();
}

/* 带宽钳位: band 外拒绝 */
static void golden_band_reject(void)
{
	const float out[TRAIT_DIM] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.20f };
	TEST_ASSERT_FALSE(trait_set(out));
	TEST_ASSERT_FALSE(trait_set(NULL));
}

/* 行为生效: ke(arousal 基线随活力)+kd(社交代谢随温情) */
static void golden_applies_to_engines(void)
{
	const float hi_e[TRAIT_DIM] = { 0.5f, 0.5f, 0.65f, 0.5f, 0.5f };
	const float hi_w[TRAIT_DIM] = { 0.5f, 0.65f, 0.5f, 0.5f, 0.5f };
	struct ke_inputs kin = { .have_drive = true };
	struct ke_output kout;
	struct kd_inputs din = { 0 };
	struct kd_output dout;
	float sat[3] = { 0.5f, 0.5f, 0.5f };
	int i;

	memcpy(kin.drive_sat, sat, sizeof(sat));

	/* E=0.65 → arousal 基线 0.10+0.20*0.15=0.13: 无事件长跑收敛 */
	trait_set(hi_e);
	ke_reset(NULL);
	for (i = 0; i < 600; i++)
		ke_step(i, &kin, &kout);
	TEST_ASSERT_TRUE(kout.state.arousal > 0.12f);

	/* W=0.65 → 社交代谢 ×1.15 → 首拍误差 8e-6*1.15 */
	trait_set(hi_w);
	kd_reset();
	kd_step(0, &din, &dout);
	TEST_ASSERT_TRUE(feq(dout.error[KD_SOCIAL], 8e-6f * 1.15f));
	/* 自娱回充零(E=0.5): stim 代谢照旧 */
	TEST_ASSERT_TRUE(feq(dout.error[KD_STIM], 4e-5f));

	trait_reset();
}

/* ---- S2.5b 双通道锚(docs/24 §3.3/§3.5) ---- */

/* x[d] 全部落在 0.5 的"普通生活"标定密度(soul_trait.c TR_K_* 同源):
 * arousal=基线 0.10; salience=K_SAL; urgency=K_URG(urg_on 0.10);
 * bond=K_BOND; negv=(1−0.5)·TR_NEGV_K ⇒ valence=−0.11 */
static void obs_neutral(struct trait_obs *o)
{
	memset(o, 0, sizeof(*o));
	o->arousal = 0.10f;
	o->salience = 0.003f;
	o->urgency = 0.89f;
	o->social_bond = 0.34f;
	o->valence = -0.11f;
}

/* settle N 天(每日一次中性观测) */
static void settle_days(int n, struct trait_obs *o)
{
	for (int i = 0; i < n; i++) {
		trait_stat_observe(o);
		trait_ema_settle();
	}
}

/* 无观测 settle 不计日不动值(warmup 语义) */
static void ema_no_observation_no_drift(void)
{
	float tr[TRAIT_DIM];
	trait_reset();
	trait_ema_settle();
	trait_ema_settle();
	TEST_ASSERT_EQUAL_INT(0, (int)trait_day_count());
	trait_get(tr);
	for (int d = 0; d < TRAIT_DIM; d++)
		TEST_ASSERT_TRUE(feq(tr[d], 0.5f));
	TEST_ASSERT_FALSE(trait_frozen(0));
}

/* 中性密度×中性性格: EMA 恒零(锚) */
static void ema_neutral_stability(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	trait_reset();
	obs_neutral(&o);
	settle_days(20, &o);
	TEST_ASSERT_EQUAL_INT(20, (int)trait_day_count());
	trait_get(tr);
	for (int d = 0; d < TRAIT_DIM; d++) {
		TEST_ASSERT_TRUE(feq(tr[d], 0.5f));
		TEST_ASSERT_FALSE(trait_frozen(d));
	}
}

/* warmup: 前 7 个结算日不移动 */
static void ema_warmup(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	trait_reset();
	obs_neutral(&o);
	o.arousal = 0.30f;          /* x_E=0.5+3*0.20=1.1→1.0: 强上行密度 */
	settle_days(7, &o);
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_ENERGY], 0.5f));    /* 未满窗不动 */
	TEST_ASSERT_EQUAL_INT(7, (int)trait_day_count());
	settle_days(1, &o);
	trait_get(tr);
	TEST_ASSERT_TRUE(tr[TR_ENERGY] > 0.5f);        /* 第 8 日始动 */
	TEST_ASSERT_TRUE(tr[TR_ENERGY] <= 0.5f + 0.05f);
}

/* band+熔断: 高能观测把 energy 顶到 band 上沿并触发冻结, 解冻后回落 */
static void ema_band_and_freeze(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	trait_reset();
	obs_neutral(&o);
	o.arousal = 0.30f;
	settle_days(30, &o);
	trait_get(tr);
	TEST_ASSERT_TRUE(tr[TR_ENERGY] <= 0.5f + 0.15f + 1e-4f);
	TEST_ASSERT_TRUE(trait_frozen(TR_ENERGY) || tr[TR_ENERGY] > 0.62f);
	/* 冻结期: 继续强上行密度也不再移动 */
	float e = tr[TR_ENERGY];
	if (trait_frozen(TR_ENERGY)) {
		settle_days(3, &o);
		trait_get(tr);
		TEST_ASSERT_TRUE(feq(tr[TR_ENERGY], e));
	}
	/* 转中性密度, 冻结结束后弹簧缓慢回拉(方向断言, 不锚天数) */
	obs_neutral(&o);
	settle_days(40, &o);
	trait_get(tr);
	TEST_ASSERT_TRUE(tr[TR_ENERGY] <= e + 1e-4f);
	TEST_ASSERT_TRUE(tr[TR_ENERGY] >= 0.35f - 1e-4f);
}

/* 互斥: 当日每维净移=|Δ| 大者的值(EMA 胜=补足, 反思胜=EMA 让路) */
static void mutual_exclusion_take_larger(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	struct trait_reflect rf;
	trait_reset();
	obs_neutral(&o);
	o.arousal = 0.30f;                       /* EMA 将上行(warmup 后) */
	settle_days(8, &o);                      /* 过 warmup, 末次 EMA≈0.015 */
	trait_get(tr);
	memset(&rf, 0, sizeof(rf));
	rf.conf = 0.9f;
	rf.evidence_n = 2;
	rf.ref_depth = 2;
	/* 反思小步 0.005 < EMA 步 → EMA 从新基线续走: 终值=EMA 目标 */
	rf.delta[TR_ENERGY] = 0.005f;
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_get(tr);
	float e_ref = tr[TR_ENERGY];
	float e_ema = e_ref + 0.03f * (1.0f - e_ref) +
		      0.005f * (0.5f - e_ref);   /* EMA 目标(含弹簧) */
	trait_stat_observe(&o);
	trait_ema_settle();
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_ENERGY], e_ema));
	/* 反思大步 0.05 > EMA 步 → EMA 让路: 当日净移恰为 0.05 */
	float e0 = tr[TR_ENERGY];
	rf.delta[TR_ENERGY] = 0.05f;
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_stat_observe(&o);
	trait_ema_settle();
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_ENERGY], e0 + 0.05f));
}

/* 反思门: 引用深度/冷却/限幅/同向证据/band */
static void reflection_gates(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	struct trait_reflect rf;
	trait_reset();
	memset(&rf, 0, sizeof(rf));
	rf.conf = 0.9f;
	rf.evidence_n = 2;

	/* 深度>2 整包拒绝 */
	rf.ref_depth = 3;
	rf.delta[TR_CURIOSITY] = 0.04f;
	TEST_ASSERT_FALSE(trait_reflect_apply(&rf));
	rf.ref_depth = 2;
	/* 首次: 无同向前科, 0.04 步直接生效 → 0.54 */
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_CURIOSITY], 0.54f));
	/* 冷却: 同日第二次拒绝 */
	TEST_ASSERT_FALSE(trait_reflect_apply(&rf));
	/* 次日(过 settle): 连续同向, 证据/置信不足 → 该维拒 */
	obs_neutral(&o);
	settle_days(1, &o);
	rf.evidence_n = 1;
	TEST_ASSERT_FALSE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_CURIOSITY], 0.54f));
	/* 证据足+置信足 → 过 → 0.58 */
	rf.evidence_n = 2;
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_CURIOSITY], 0.58f));
	/* 继续同向(证据足) → 0.63 */
	settle_days(1, &o);
	rf.delta[TR_CURIOSITY] = 0.05f;
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_CURIOSITY], 0.63f));
	/* band 钳: 再 +0.05 → 0.68 目标 → 钳 0.65 上沿 */
	settle_days(1, &o);
	TEST_ASSERT_TRUE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_CURIOSITY], 0.65f));
}

/* 冻结: 温情高位连续反思下穿大半 band → 熔断, 且冻结拦反思 */
static void freeze_blocks_reflection(void)
{
	float tr[TRAIT_DIM];
	struct trait_obs o;
	struct trait_reflect rf;
	/* W=0.65 起步(带内), 连续 −0.05/日: 7 个结算日内横穿 0.1125 净移 */
	const float t5[TRAIT_DIM] = { 0.50f, 0.65f, 0.50f, 0.50f, 0.50f };
	trait_reset();
	TEST_ASSERT_TRUE(trait_set(t5));
	obs_neutral(&o);
	memset(&rf, 0, sizeof(rf));
	rf.conf = 0.9f;
	rf.evidence_n = 2;
	rf.ref_depth = 2;
	for (int i = 0; i < 12 && !trait_frozen(TR_WARMTH); i++) {
		trait_stat_observe(&o);
		trait_ema_settle();
		rf.delta[TR_WARMTH] = -0.05f;
		(void)trait_reflect_apply(&rf);
	}
	TEST_ASSERT_TRUE(trait_frozen(TR_WARMTH));
	trait_get(tr);
	float w = tr[TR_WARMTH];
	/* 冻结期内反思无效(两通道全停) */
	rf.delta[TR_WARMTH] = -0.03f;
	TEST_ASSERT_FALSE(trait_reflect_apply(&rf));
	trait_get(tr);
	TEST_ASSERT_TRUE(feq(tr[TR_WARMTH], w));
	/* 冻结自然到期(~7 结算日)后解冻 */
	settle_days(9, &o);
	TEST_ASSERT_FALSE(trait_frozen(TR_WARMTH));
}

void trait_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_anchor_neutral);
	RUN_TEST(golden_extremes);
	RUN_TEST(golden_band_reject);
	RUN_TEST(golden_applies_to_engines);
	RUN_TEST(ema_no_observation_no_drift);
	RUN_TEST(ema_neutral_stability);
	RUN_TEST(ema_warmup);
	RUN_TEST(ema_band_and_freeze);
	RUN_TEST(mutual_exclusion_take_larger);
	RUN_TEST(reflection_gates);
	RUN_TEST(freeze_blocks_reflection);
	UNITY_END();
}
