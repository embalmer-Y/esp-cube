/*
 * test_soul_moodgate.c — mood 回写三断法黄金回归(S6 D4, docs/24 §5.4)
 *
 * 锚: 隔轮采纳(≤1/2 轮)/×0.5 衰减/方向失配再 ×0.5/AR(1)>0.8 暂停 24h。
 */
#include <math.h>
#include <string.h>

#include "soul_moodgate.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 隔轮+衰减+方向 */
static void golden_three_gates(void)
{
	float ap;
	soul_moodgate_reset();
	/* 轮 1: 采纳, ×0.5 */
	TEST_ASSERT_TRUE(soul_moodgate_allow(1, 0, 0.2f, &ap));
	TEST_ASSERT_TRUE(feq(ap, 0.1f));
	/* 轮 2: 隔轮丢弃 */
	TEST_ASSERT_FALSE(soul_moodgate_allow(2, 0, 0.2f, &ap));
	/* 轮 3: 再采纳 */
	TEST_ASSERT_TRUE(soul_moodgate_allow(3, 0, -0.2f, &ap));
	TEST_ASSERT_TRUE(feq(ap, -0.1f));
	/* 方向失配: LLM 说正, 引擎动量为负 → 再 ×0.5 */
	TEST_ASSERT_TRUE(soul_moodgate_allow(5, -1, 0.2f, &ap));
	TEST_ASSERT_TRUE(feq(ap, 0.05f));
	/* 方向同号: 不再折半 */
	TEST_ASSERT_TRUE(soul_moodgate_allow(7, 1, 0.2f, &ap));
	TEST_ASSERT_TRUE(feq(ap, 0.1f));
	/* 零回写不占隔轮账(丢弃后同轮重试仍受隔轮约束) */
	TEST_ASSERT_FALSE(soul_moodgate_allow(8, 0, 0.0f, &ap));
	TEST_ASSERT_FALSE(soul_moodgate_allow(8, 0, 0.1f, &ap));
	TEST_ASSERT_TRUE(soul_moodgate_allow(9, 0, 0.1f, &ap));
}

/* 自增强指纹: 定值序列 AR(1)=1 → 第 50 次记录后暂停 */
static void golden_ar_pause(void)
{
	soul_moodgate_reset();
	/* 同向定值回写 ×48: AR(1)=1(方差恒零时 ar1()=0 保护, 故加扰动) */
	float ap;
	for (int i = 0; i < 10; i++) {
		TEST_ASSERT_TRUE(soul_moodgate_allow((uint32_t)(i * 2 + 1),
						     0, 0.2f, &ap));
		soul_moodgate_record(ap);
	}
	TEST_ASSERT_FALSE(soul_moodgate_paused(0));
	/* 构造强自相关: 50 点线性爬坡(AR(1)≈0.94>0.8; 10 点 ramp 仅
	 * ≈0.70 不达门——样本量即合同) */
	soul_moodgate_reset();
	for (int i = 0; i < 50; i++)
		soul_moodgate_record(0.01f * (float)i);
	TEST_ASSERT_TRUE(soul_moodgate_paused(1000));
	/* 24h 后解禁 */
	TEST_ASSERT_FALSE(soul_moodgate_paused(1000 + 24u * 3600));
	/* 少样本(<3)不判 */
	soul_moodgate_reset();
	soul_moodgate_record(0.1f);
	soul_moodgate_record(0.2f);
	TEST_ASSERT_FALSE(soul_moodgate_paused(0));
}

void moodgate_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_three_gates);
	RUN_TEST(golden_ar_pause);
	UNITY_END();
}
