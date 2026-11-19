/*
 * test_soul_emotion.c — 九维动力学黄金轨迹回归（Unity 宿主垫片）
 *
 * S2 重锚(2026-11-19, docs/24 §2 定案, R4 纪律=语义变更同批改锚):
 *  - valence 三层栈: 快层(差分+冲量)积分器 s_val 无偏; 显示合成
 *    0.65×bias(s_val)+0.35×mood, 负性偏置×2 只在显示侧;
 *  - mood 慢层: 三水库满足度(L=0.2/H=0.8, WMIN=0.4)导出 μ, τ=10800s;
 *  - ethics: 事件剂量(负面重评价 0.3×|Δ|/轮失败 0.05)+深度门(|v|>0.35
 *    部分计费)+比例泄漏 τ=3d; 解锁=安抚(−0.2/次)+施密特(≤0.9 且
 *    ≥1800s)+再闩免疫 1800s;
 *  - arousal: E1 冲量(Δemax×2)+事件脉冲(重评价|Δ|×2/轮成功 0.10/
 *    轮失败 0.15)+基线回归 τ=64s; 缺席拍回归基线(旧 D5"向 held emax
 *    收敛"语义废止);
 *  - bond: E4 百分比更新(0.02/事件, 900s 宽限, 地板 0.05)。
 * 期望值全部公式手算; 纯快层用例以 sat={0.55,0.55,0.55} 钉住 μ=0。
 */
#include <math.h>
#include <string.h>

#include "soul_drive.h"
#include "soul_emotion.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 纯快层用例的水位: s=0.55 → 满足度=(0.55-0.2)/0.6=0.583?? 不——
 * μ 需为 0: mn=av=0.5 → s=(0.5-0.2)/0.6=0.5 → 水位 0.5。 */
static const float SAT_MU0[3] = { 0.5f, 0.5f, 0.5f };

/* 低通原语：Galatea 阶跃/衰减黄金锚（不变） */
static void test_lowpass_primitive(void)
{
	float s = 0.0f;
	int i;

	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.25f));
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.4375f));
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.578125f));     /* 源锚 s3 */
	s = ke_lowpass(s, 1.0f, 4.0f);
	TEST_ASSERT_TRUE(feq(s, 0.68359375f));   /* 源锚 s4（t63=4 拍） */
	s = 1.0f;
	for (i = 0; i < 16; i++)
		s = ke_lowpass(s, 0.0f, 16.0f);
	TEST_ASSERT_TRUE(fabsf(s - 0.35607413f) < 1e-4f); /* (15/16)^16 */
}

/* 半带宽迟滞（D6）：不变 */
static void test_hysteresis_primitive(void)
{
	bool on = false;

	TEST_ASSERT_TRUE(ke_hysteresis(on, 0.55f, 0.1f));
	on = true;
	TEST_ASSERT_TRUE(ke_hysteresis(on, 0.50f, 0.1f));
	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.45f, 0.1f));
	on = false;
	TEST_ASSERT_FALSE(ke_hysteresis(on, 0.54f, 0.1f));
}

/*
 * 黄金 1：恒定驱力误差 e=0.5(水位钉 μ=0)
 *  拍0：raw = -0.5 → s_val=-0.125 → 显示 0.65×2×(-0.125) = -0.1625
 *  (负性偏置×2 只在显示侧; 积分器无偏)
 *  拍1/2：s_val×0.75/拍 → 显示 ×0.75
 *  arousal(E1+事件脉冲): 拍0 Δemax=0.5 → 冲量 1.0 → 钳 1.0;
 *  拍1 1+(0.1-1)/64=0.9859375; 拍2 → 0.9720947 → surprised 带
 */
static void golden_constant_error(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 0.5f,
		.drive_error_max = 0.5f,
	};
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.1625f));
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.121875f));
	ke_step(2, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.09140625f));
	TEST_ASSERT_TRUE(feq(out.state.arousal, 0.9720947f));
	TEST_ASSERT_EQUAL_STRING("surprised", out.anim_tag);
}

/* 源锚（持续饥渴不难过）: 恒定误差 41 拍 → 快层→0(mood 钉 0 → 显示→0);
 * arousal 自拍0 冲量 1.0 按 τ=64 回归: 0.1+0.9×(63/64)^40 ≈ 0.579 */
static void golden_constant_error_long(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 0.5f,
		.drive_error_max = 0.5f,
	};
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i <= 40; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(fabsf(out.state.valence) < 0.01f);
	TEST_ASSERT_TRUE(out.state.arousal > 0.55f && out.state.arousal < 0.60f);
}

/* 误差跳变 0→1: s_val 单拍 -0.25 → 显示 -0.325(偏置×2 后) */
static void golden_error_step_jump(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	in.drive_error_sum = 1.0f;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.325f));
}

/* urgency 迟滞：0.10 激活 / 0.05 释放（不变; 水位钉 μ=0） */
static void golden_urgency_gate(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_FALSE(out.ignition_candidate);
	in.drive_error_max = 0.9f;
	for (i = 8; i < 24; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.ignition_candidate);
	in.drive_error_max = 0.0f;
	ke_step(24, &in, &out);
	ke_step(25, &in, &out);
	TEST_ASSERT_TRUE(out.state.urgency > 0.05f);
	TEST_ASSERT_TRUE(out.ignition_candidate);
}

/* 重评价（D4 一次性）：越界拒/覆盖/回执/applied 审计——数值更新为
 * 无偏积分器+显示合成(0.65×, 无偏置: 正侧); 附带事件唤醒脉冲不参与
 * valence 断言。负面重评价同时给伦理负荷剂量(见 golden_ethics)。 */
static void golden_reappraisal_channel(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	struct ke_reappraisal r = { .delta = 0.5f, .receipt_id = 42 };
	struct ke_reappraisal r2 = { .delta = -0.1f, .receipt_id = 7 };

	ke_reset(NULL);
	TEST_ASSERT_EQUAL(-2, ke_reappraisal_request(&r)); /* 越界拒 */
	r.delta = 0.2f;
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r));
	ke_step(0, &in, &out);
	/* s_val=0.2 施加后随 raw=0 低通一拍 → 0.2·3/4=0.15 → 显示 0.65×0.15 */
	TEST_ASSERT_TRUE(feq(out.state.valence, 0.0975f));
	TEST_ASSERT_TRUE(feq(ke_reappraisal_applied(), 0.2f));
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(42));
	TEST_ASSERT_EQUAL(0, ke_reappraisal_request(&r2)); /* 覆盖 */
	ke_step(1, &in, &out);
	/* s_val=0.15-0.1=0.05 → 低通 0.0375 → 显示 0.024375 */
	TEST_ASSERT_TRUE(feq(out.state.valence, 0.024375f));
	TEST_ASSERT_TRUE(ke_reappraisal_receipt(7));
	TEST_ASSERT_FALSE(ke_reappraisal_receipt(42));
	TEST_ASSERT_TRUE(feq(ke_reappraisal_applied(), -0.1f));
}

/*
 * mood 慢层（S2 新增锚）:
 *  a) 水位全 0.2 以下 → μ=-1: mood_n = -(1-(1-1/10800)^n)
 *     n=10800 → -(1-e^-1) ≈ -0.632
 *  b) 混合 {1.0,0.5,0.5}: s={1,0.5,0.5} mn=0.5 av=0.667
 *     μ=2×(0.4×0.5+0.6×0.6667)-1=+0.2 → n=10800 → +0.1264
 */
static void golden_mood_layer(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	int i;

	ke_reset(NULL);
	in.drive_sat[0] = in.drive_sat[1] = in.drive_sat[2] = 0.1f;
	for (i = 0; i < 10800; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(fabsf(out.state.mood - (-0.6321f)) < 0.01f);

	ke_reset(NULL);
	in.drive_sat[0] = 1.0f;
	in.drive_sat[1] = in.drive_sat[2] = 0.5f;
	for (i = 0; i < 10800; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(fabsf(out.state.mood - 0.1264f) < 0.01f);
}

/* 显示合成（S2 新增锚）: mood=-1(先 2 万拍压满) 后误差跳变 0→0.5:
 * 显示 = 0.65×2×(-0.125) + 0.35×(-1) = -0.5125(偏置与 mood 混合) */
static void golden_display_compose(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	struct ke_output out;
	int i;

	ke_reset(NULL);
	in.drive_sat[0] = in.drive_sat[1] = in.drive_sat[2] = 0.1f;
	for (i = 0; i < 20000; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(out.state.mood < -0.84f);
	in.drive_error_sum = 0.5f;
	in.drive_error_max = 0.5f;
	ke_step(20000, &in, &out);
	/* mood=-(1-(10799/10800)^20000)=-0.84317 → -0.1625+0.35×-0.84317 */
	TEST_ASSERT_TRUE(feq(out.state.valence, -0.457608f));
}

/*
 * arousal 事件脉冲（S2 新增锚）: 轮成功 +0.10; 重评价 -0.2 → +0.40;
 * 施加拍后 τ=64 回归(64 拍余 ~36.8%)
 */
static void golden_arousal_events(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	struct ke_reappraisal r = { .delta = -0.2f, .receipt_id = 1 };
	int i;

	ke_reset(NULL);
	in.receipt = KE_RECEIPT_SUCCESS;
	ke_step(0, &in, &out);
	/* 0 + 脉冲0.10 + 基线回归 0.1/64 = 0.1015625 */
	TEST_ASSERT_TRUE(feq(out.state.arousal, 0.10f + (0.10f - 0.0f) / 64.0f));
	in.receipt = KE_RECEIPT_NONE;
	ke_reappraisal_request(&r);
	ke_step(1, &in, &out);
	/* 上拍 0.1015625 回归一拍 + 重评价脉冲 0.4 */
	TEST_ASSERT_TRUE(feq(out.state.arousal,
			     0.1015625f + (0.10f - 0.1015625f) / 64.0f + 0.40f));
	for (i = 2; i < 66; i++)
		ke_step(i, &in, &out);
	/* 施加拍值 0.50154 经 64 拍回归: ≈0.1+0.40154×e^-1 ≈ 0.2477 */
	TEST_ASSERT_TRUE(out.state.arousal > 0.24f && out.state.arousal < 0.26f);
}

/*
 * 伦理闩锁+解锁（S2 L2 方案锚）:
 *  a) 剂量闩锁: 连续负面重评价(0.06/拍) 轮失败(0.05/拍) → 负荷净增
 *     0.11-泄漏 → ~25 拍过阈 2.5 → 闩锁;
 *  b) 施密特+最短闩锁: 停止负面+持续安抚(0.2/拍) → 负荷速降, 但
 *     1800s 内不解锁; 1800s 后解锁;
 *  c) 再闩免疫: 解锁后 1800s 内剂量堆积不过阈被免疫门拦。
 */
static void golden_ethics_latch(void)
{
	struct ke_inputs in = { .have_drive = true, 0 };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out = { 0 };   /* 循环条件读 latched: 禁栈垃圾 */
	struct ke_reappraisal r = { .delta = -0.2f, .receipt_id = 100 };
	int i;

	ke_reset(NULL);
	in.receipt = KE_RECEIPT_FAIL;
	for (i = 0; i < 60 && !out.state.ethics_latched; i++) {
		r.receipt_id = 100 + (uint64_t)i;
		ke_reappraisal_request(&r);
		ke_step(i, &in, &out);
	}
	TEST_ASSERT_TRUE(out.state.ethics_latched);      /* ≤60 拍剂量闩锁 */
	TEST_ASSERT_TRUE(out.state.ethics_load >= 2.5f); /* 负荷遥测 */

	/* 停虐+安抚: 负荷速降但最短闩锁 1800s 把门 */
	in.receipt = KE_RECEIPT_SUCCESS;
	in.soothe_event = true;
	for (; i < 2000; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(i > 1850);          /* 确实跑到 ≥1800s */
	TEST_ASSERT_FALSE(out.state.ethics_latched);  /* 解锁 */
	TEST_ASSERT_TRUE(out.state.ethics_load <= 0.9f);

	/* 再闩免疫: 立刻恢复剂量 60 拍(净负荷可过阈)但免疫期内不闩 */
	in.receipt = KE_RECEIPT_FAIL;
	in.soothe_event = false;
	bool ever = false;
	for (int k = 0; k < 60; k++) {
		r.receipt_id = 1000 + (uint64_t)k;
		ke_reappraisal_request(&r);
		ke_step(i + k, &in, &out);
		ever = ever || out.state.ethics_latched;
	}
	TEST_ASSERT_FALSE(ever);
	/* ke_ethics_clear: 显式重置兜底(owner/出厂路径) */
	ke_ethics_clear();
	ke_step(i + 100, &in, &out);
	TEST_ASSERT_FALSE(out.state.ethics_latched);
}

/* ke_reset 全清（S6）: 残值不得泄漏(E4 bond 百分比口径) */
static void golden_reset_clears(void)
{
	struct ke_inputs in = {
		.have_drive = true,
		.drive_error_sum = 1.0f,
		.drive_error_max = 1.0f,
		.social_event = true,
	};
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	int i;

	ke_reset(NULL);
	for (i = 0; i < 10; i++)
		ke_step(i, &in, &out);
	/* E4: 10 次 0.02 百分比上行 → 1-0.98^10 = 0.1829 */
	TEST_ASSERT_TRUE(feq(out.state.social_bond, 0.182937f));
	ke_reset(NULL);
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.social_bond, 0.02f));
	TEST_ASSERT_TRUE(feq(out.state.urgency, 0.25f)); /* e_max=1 */
}

/*
 * 缺席拍（S2 语义变更: E1 缺席=回归基线, 旧"向 held emax 收敛"废止）:
 * !have_drive 拍 valence 向 0 衰减、arousal 向基线 0.10 回归(不再上爬)
 */
static void golden_no_drive_hold(void)
{
	struct ke_inputs in = { .have_drive = true,
				.drive_error_sum = 1.0f,
				.drive_error_max = 0.9f };
	memcpy(in.drive_sat, SAT_MU0, sizeof(in.drive_sat));
	struct ke_output out;
	float v, a, u;

	ke_reset(NULL);
	ke_step(0, &in, &out);
	ke_step(1, &in, &out);
	v = out.state.valence;
	a = out.state.arousal;
	u = out.state.urgency;
	in.have_drive = false;
	ke_step(2, &in, &out);
	TEST_ASSERT_TRUE(out.state.valence > v);      /* 向 0 衰减非跳变 */
	TEST_ASSERT_TRUE(out.state.arousal < a);      /* 回归基线(S2)      */
	TEST_ASSERT_TRUE(out.state.arousal > 0.45f);  /* 64s 时标不瞬降    */
	TEST_ASSERT_TRUE(out.state.urgency > u);
}

/* D7 有效性位（不变） */
static void golden_residual_hold(void)
{
	struct ke_inputs in = { 0 };
	struct ke_output out;

	ke_reset(NULL);
	in.sensory_residual_mean = 0.8f;
	ke_step(0, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.novelty, 0.0f));
	in.have_residual = true;
	ke_step(1, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.novelty, 0.2f));
}

/* kd→ke 全链锚(S2 合成口径): 代谢 0.5/拍 → 水位 0.5(μ=0), E 0→1.5→3
 * 快层 -0.375/-0.75/-0.5625 → 显示×(-1.3)(偏置×2×0.65) */
static void golden_kd_ke_chain(void)
{
	const struct kd_params p = {
		.metabolic = { 0.5f, 0.5f, 0.5f },
		.band = 0.1f,
	};
	struct kd_inputs din = { 0 };
	struct kd_output dout;
	struct ke_inputs kin = { .have_drive = true };
	struct ke_output kout;

	kd_init(&p);
	kd_reset();
	ke_reset(NULL);
	kd_step(0, &din, &dout);
	kin.drive_error_sum = dout.error_sum;
	kin.drive_error_max = dout.error_max;
	memcpy(kin.drive_sat, dout.level, sizeof(kin.drive_sat));
	ke_step(0, &kin, &kout);
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.4875f)); /* 0.65×2×-0.375 */
	kd_step(1, &din, &dout);
	kin.drive_error_sum = dout.error_sum;    /* E 1.5→3.0 */
	kin.drive_error_max = dout.error_max;
	memcpy(kin.drive_sat, dout.level, sizeof(kin.drive_sat));
	ke_step(1, &kin, &kout);
	/* 快层 -0.375+(-1.5+0.375)/4=-0.65625 → 显示 0.65×2×-0.65625 */
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.853125f));
	kd_step(2, &din, &dout);          /* E 持稳 3.0 → raw 0 */
	kin.drive_error_sum = dout.error_sum;
	kin.drive_error_max = dout.error_max;
	memcpy(kin.drive_sat, dout.level, sizeof(kin.drive_sat));
	ke_step(2, &kin, &kout);
	/* 快层 -0.65625×0.75=-0.4921875 → 显示 -0.63984 */
	TEST_ASSERT_TRUE(feq(kout.state.valence, -0.639844f)); /* 回升 */
}

/* social_bond（E4 口径）: 8 事件 → 1-0.98^8=0.149; 32 拍宽限窗内不衰减 */
static void golden_social_bond(void)
{
	struct ke_inputs in = { 0 };
	struct ke_output out;
	int i;

	ke_reset(NULL);
	in.social_event = true;
	for (i = 0; i < 8; i++)
		ke_step(i, &in, &out);
	TEST_ASSERT_TRUE(feq(out.state.social_bond, 0.149316f));
	in.social_event = false;
	for (i = 8; i < 40; i++)
		ke_step(i, &in, &out);
	/* 宽限窗 900s: 32 拍内零衰减 */
	TEST_ASSERT_TRUE(feq(out.state.social_bond, 0.149316f));
}

void emotion_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_lowpass_primitive);
	RUN_TEST(test_hysteresis_primitive);
	RUN_TEST(golden_constant_error);
	RUN_TEST(golden_constant_error_long);
	RUN_TEST(golden_error_step_jump);
	RUN_TEST(golden_urgency_gate);
	RUN_TEST(golden_reappraisal_channel);
	RUN_TEST(golden_mood_layer);
	RUN_TEST(golden_display_compose);
	RUN_TEST(golden_arousal_events);
	RUN_TEST(golden_ethics_latch);
	RUN_TEST(golden_reset_clears);
	RUN_TEST(golden_no_drive_hold);
	RUN_TEST(golden_residual_hold);
	RUN_TEST(golden_kd_ke_chain);
	RUN_TEST(golden_social_bond);
	UNITY_END();
}
