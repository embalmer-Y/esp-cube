/*
 * test_soul_trait.c — trait 层黄金回归(S2.5a, docs/24 §3/§11.3)
 *
 * 锚原则(核心契约): 性格全 0.5 ⇒ 调制系数 ≡ S2 中性常数——
 * ke/kd 全部黄金锚不因 trait 层引入而漂移。
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

void trait_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_anchor_neutral);
	RUN_TEST(golden_extremes);
	RUN_TEST(golden_band_reject);
	RUN_TEST(golden_applies_to_engines);
	UNITY_END();
}
