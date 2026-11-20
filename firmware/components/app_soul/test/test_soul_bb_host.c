/*
 * test_soul_bb_host.c — 黑板黄金回归(宿主, 自带 main; S3 起 L2 评分器
 * 随 R6 退役, 本件保留黑板件契约: 覆写 TTL/显著性衰减/真值语义,
 * docs/14 §9。行为表断言迁至 test_soul_budget.c(深思行动预算)。)
 *
 * 编译运行见 tools/host_tests.sh [1/14]。黄金值全手算。
 */
#include <math.h>
#include <stdio.h>

#include "soul_bb.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

static void test_override_ttl(void)
{
    soul_bb_reset();
    soul_bb_set(SOUL_BB_QUIET, 0.0f);
    soul_bb_tick(1000);
    soul_bb_override(SOUL_BB_QUIET, 1.0f, 10);
    CHECK(soul_bb_get(SOUL_BB_QUIET) > 0.99f, "override active");
    soul_bb_tick(1005);
    CHECK(soul_bb_get(SOUL_BB_QUIET) > 0.99f, "override still active");
    soul_bb_tick(1011);
    CHECK(FEQ(soul_bb_get(SOUL_BB_QUIET), 0.0f), "override expired");
    /* 真值不被覆写破坏 */
    soul_bb_set(SOUL_BB_AROUSAL, 0.3f);
    soul_bb_override(SOUL_BB_AROUSAL, 0.9f, 100);
    CHECK(soul_bb_get(SOUL_BB_AROUSAL) > 0.89f, "arousal overridden");
    soul_bb_override(SOUL_BB_AROUSAL, 0, 0);
    CHECK(FEQ(soul_bb_get(SOUL_BB_AROUSAL), 0.3f), "clear restores truth");
}

static void test_salience_decay(void)
{
    soul_bb_reset();
    soul_bb_salience(0.8f);
    for (int i = 0; i < 60; i++) soul_bb_tick((uint32_t)i);
    float v = soul_bb_get(SOUL_BB_SALIENCE);
    CHECK(v < 0.8f * 0.9885f * 0.9885f, "salience decays");
    CHECK(v > 0.3f, "salience ~half-life 60s");
    for (int i = 60; i < 800; i++) soul_bb_tick((uint32_t)i);
    CHECK(FEQ(soul_bb_get(SOUL_BB_SALIENCE), 0.0f), "salience floors to 0");
}

static void test_set_get_passthrough(void)
{
    soul_bb_reset();
    /* 契约: set 为原值透传(钳位由供给方负责, 与固件 bb_feed 口径一致) */
    soul_bb_set(SOUL_BB_VALENCE, -0.75f);
    CHECK(FEQ(soul_bb_get(SOUL_BB_VALENCE), -0.75f), "valence pass-through");
    soul_bb_set(SOUL_BB_IDLE, 0.25f);
    CHECK(FEQ(soul_bb_get(SOUL_BB_IDLE), 0.25f), "idle pass-through");
    soul_bb_set((soul_bb_key_t)SOUL_BB_CNT, 1.0f);   /* 越界键忽略 */
    CHECK(FEQ(soul_bb_get(SOUL_BB_VALENCE), -0.75f), "oob key ignored");
}

int main(void)
{
    test_override_ttl();
    test_salience_decay();
    test_set_get_passthrough();
    if (s_fail == 0) {
        printf("test_soul_bb: ALL PASS\n");
        return 0;
    }
    printf("test_soul_bb: %d FAIL\n", s_fail);
    return 1;
}
