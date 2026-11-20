/*
 * test_soul_budget.c — 深思行动预算分账黄金回归(S3, docs/24 §4.5/§6)
 *
 * 锚(合同): 分账日帽 speak 4/music 6/face 6; 主动音频≤3/拖尾小时
 * (docs/17 §4.3); 同类冷却 30min; E3 新鲜度 τ=30min 线性恢复;
 * 跨 86400s 自动换账日。R6: 本件只执法不生成。
 */
#include <math.h>
#include <string.h>

#include "soul_budget.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* 日帽: speak 第 5 次拒; 换日重置(间隔≥同类冷却 1800s) */
static void golden_daily_cap(void)
{
	const uint32_t t0 = 1000;
	sb_reset();
	for (int i = 0; i < 4; i++) {
		TEST_ASSERT_TRUE(sb_allow(SB_SPEAK, t0 + i * 1800));
		sb_charge(SB_SPEAK, t0 + i * 1800);
	}
	/* 第 4 次后同日: 冷却已过但日帽满 */
	TEST_ASSERT_FALSE(sb_allow(SB_SPEAK, t0 + 4 * 1800));
	TEST_ASSERT_EQUAL_INT(4, sb_fired_today(SB_SPEAK));
	/* 换日(跳 24h): 清零可再言 */
	TEST_ASSERT_TRUE(sb_allow(SB_SPEAK, t0 + 86400 + 1800));
}

/* 同类冷却 30min: music 连打拒, 过冷却放行 */
static void golden_cat_cooldown(void)
{
	const uint32_t t0 = 86400;
	sb_reset();
	TEST_ASSERT_TRUE(sb_allow(SB_MUSIC, t0));
	sb_charge(SB_MUSIC, t0);
	TEST_ASSERT_FALSE(sb_allow(SB_MUSIC, t0 + 1799));
	TEST_ASSERT_TRUE(sb_allow(SB_MUSIC, t0 + 1800));
}

/* 音频时窗: speak+music 合计 ≤3/拖尾 1h; face 不占音频账 */
static void golden_audio_hour(void)
{
	const uint32_t t0 = 172800;
	sb_reset();
	sb_charge(SB_MUSIC, t0);
	sb_charge(SB_SPEAK, t0 + 60);
	sb_charge(SB_MUSIC, t0 + 120);
	TEST_ASSERT_FALSE(sb_allow(SB_MUSIC, t0 + 200));   /* 时窗满(且冷却) */
	TEST_ASSERT_FALSE(sb_allow(SB_SPEAK, t0 + 200));
	TEST_ASSERT_TRUE(sb_allow(SB_FACE, t0 + 200));     /* 非音频不占 */
	/* 1h 滑出: 最早一枚出窗 → 又可一言 */
	TEST_ASSERT_TRUE(sb_allow(SB_SPEAK, t0 + 3600 + 61));
	/* 同类冷却先行: music 距上次 3480s<1800? 否→时窗 2/3 → 可放 */
	TEST_ASSERT_TRUE(sb_allow(SB_MUSIC, t0 + 3700));
}

/* face 日帽独立(speak/music 时窗不影响 face 计数) */
static void golden_face_independent(void)
{
	const uint32_t t0 = 259200;
	sb_reset();
	for (int i = 0; i < 6; i++) {
		TEST_ASSERT_TRUE(sb_allow(SB_FACE, t0 + i * 2000));
		sb_charge(SB_FACE, t0 + i * 2000);
	}
	TEST_ASSERT_FALSE(sb_allow(SB_FACE, t0 + 20000));
	/* 音频账仍全空 */
	TEST_ASSERT_TRUE(sb_allow(SB_SPEAK, t0 + 20000));
	TEST_ASSERT_EQUAL_INT(6, sb_fired_today(SB_FACE));
}

/* E3 新鲜度: 刚执行→0, τ 线性恢复→1; 从未执行=1 */
static void golden_recency(void)
{
	const uint32_t t0 = 345600;
	sb_reset();
	TEST_ASSERT_TRUE(feq(sb_recency(SB_SPEAK, t0), 1.0f));
	sb_charge(SB_SPEAK, t0);
	TEST_ASSERT_TRUE(feq(sb_recency(SB_SPEAK, t0), 0.0f));
	TEST_ASSERT_TRUE(feq(sb_recency(SB_SPEAK, t0 + 900), 0.5f));
	TEST_ASSERT_TRUE(feq(sb_recency(SB_SPEAK, t0 + 1800), 1.0f));
	TEST_ASSERT_TRUE(feq(sb_recency(SB_MUSIC, t0 + 1800), 1.0f));
}

/* 类别越界安全 + snapshot 形态 */
static void golden_bounds_and_snapshot(void)
{
	char buf[96];
	sb_reset();
	TEST_ASSERT_FALSE(sb_allow(-1, 0));
	TEST_ASSERT_FALSE(sb_allow(SB_CNT, 0));
	TEST_ASSERT_EQUAL_INT(255, sb_fired_today(-1));
	TEST_ASSERT_TRUE(feq(sb_recency(SB_CNT, 0), 0.0f));
	sb_charge(SB_SPEAK, 100);
	sb_snapshot(buf, sizeof(buf));
	TEST_ASSERT_TRUE(strstr(buf, "1/4") != NULL);
	TEST_ASSERT_TRUE(strstr(buf, "说话") != NULL);
}

void budget_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_daily_cap);
	RUN_TEST(golden_cat_cooldown);
	RUN_TEST(golden_audio_hour);
	RUN_TEST(golden_face_independent);
	RUN_TEST(golden_recency);
	RUN_TEST(golden_bounds_and_snapshot);
	UNITY_END();
}
