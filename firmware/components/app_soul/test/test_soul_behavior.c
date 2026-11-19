/*
 * test_soul_behavior.c —— 黑板+行为评分器黄金回归(宿主, 自带 main)
 *
 * 编译运行(仓库根):
 *   gcc -DSOUL_BB_HOST_TEST -I firmware/components/app_soul/include \
 *       -o /tmp/test_soul_bh \
 *       firmware/components/app_soul/test/test_soul_behavior.c \
 *       firmware/components/app_soul/src/soul_bb.c \
 *       firmware/components/app_soul/src/soul_behavior.c -lm \
 *   && /tmp/test_soul_bh
 *
 * 与 test_soul_drive/emotion(Unity, IDF 壳)不同: 本件零依赖自跑,
 * 供 CI/本机快速回归(docs/14 §9)。黄金值全手算。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "soul_bb.h"
#include "soul_behavior.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

/* ---- 计数执行器(注入) ---- */
static int s_exec_a, s_exec_b;
static void exec_a(void) { s_exec_a++; }
static void exec_b(void) { s_exec_b++; }

/* 行为 A: 仅 quiet 驱动(radio_stop 形: quiet×playing) */
static const soul_cons_t c_a[] = {
    { SOUL_BB_QUIET,   SOUL_C_RISE, 1.0f },
    { SOUL_BB_PLAYING, SOUL_C_RISE, 1.0f },
};
/* 行为 B: 刺激缺口驱动(radio_start 形: stim×¬playing×¬quiet×¬ai) */
static const soul_cons_t c_b[] = {
    { SOUL_BB_M_STIM,    SOUL_C_RISE, 1.0f },
    { SOUL_BB_PLAYING,   SOUL_C_FALL, 1.0f },
    { SOUL_BB_QUIET,     SOUL_C_FALL, 1.0f },
    { SOUL_BB_AI_ACTIVE, SOUL_C_FALL, 1.0f },
};

static void reg(void)
{
    soul_bb_reset();
    soul_behavior_reset();
    s_exec_a = s_exec_b = 0;
    soul_behavior_register(&(soul_bhv_t){
        .name = "a", .cons = c_a, .cons_n = 2,
        .threshold = 0.8f, .w = 1.0f, .cooldown_s = 60, .exec = exec_a,
    });
    soul_behavior_register(&(soul_bhv_t){
        .name = "b", .cons = c_b, .cons_n = 4,
        .threshold = 0.35f, .w = 1.0f, .cooldown_s = 1800, .exec = exec_b,
    });
}

static void test_curve_math(void)
{
    reg();
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    soul_bb_set(SOUL_BB_AI_ACTIVE, 0.0f);
    float s = soul_behavior_score(0);
    CHECK(FEQ(s, 1.0f), "a(quiet=1,playing=1)=1.0");
    soul_bb_set(SOUL_BB_PLAYING, 0.0f);
    s = soul_behavior_score(0);
    CHECK(FEQ(s, 0.0f), "a(playing=0)=0");
    /* b: 未播+勿扰假+非会话 → 纯由刺激缺口决定 */
    soul_bb_set(SOUL_BB_QUIET, 0.0f);
    soul_bb_set(SOUL_BB_M_STIM, 0.5f);
    s = soul_behavior_score(1);
    CHECK(FEQ(s, 0.5f), "b(stim=.5,not playing)=0.5");
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    s = soul_behavior_score(1);
    CHECK(FEQ(s, 0.0f), "b(playing=1)=0(已在播不重起)");
    soul_bb_set(SOUL_BB_PLAYING, 0.0f);
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    s = soul_behavior_score(1);
    CHECK(FEQ(s, 0.0f), "b(quiet=1)=0(勿扰不放)");
}

static void test_fire_and_cooldown(void)
{
    reg();
    soul_behavior_set_seed(42);
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    soul_bb_tick(100);
    soul_behavior_tick(100);
    CHECK(s_exec_a == 1, "a fires t=100");
    soul_behavior_tick(101);
    CHECK(s_exec_a == 1, "cooldown holds t=101");
    soul_behavior_tick(159);
    CHECK(s_exec_a == 1, "cooldown holds t=159");
    /* E3(docs/23 §2.5): 习惯化闸门——t=100 首发后 hab=0.55, 冷却 60s
     * 仅恢复 60×6e-4=0.036, t=160 出冷却时 eff=score×(1−0.514)=0.486
     * 阈 0.8 → 被习惯化压住不再立即重发(新契约: 冷却到期≠必发,
     * 新鲜感恢复才是发火条件)。恢复到 eff=1−hab≥0.8 → hab≤0.2,
     * 从 0.514 需 0.314/6e-4≈524s → t≈100+60+524=684 重发。 */
    soul_behavior_tick(160);
    CHECK(s_exec_a == 1, "habituation gate holds at cooldown expiry t=160");
    soul_behavior_tick(700);
    CHECK(s_exec_a == 2, "refires once habituation decays t=700");
}

static void test_threshold_blocks(void)
{
    reg();
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 0.7f);   /* score=0.7 < 0.8 */
    soul_behavior_tick(10);
    CHECK(s_exec_a == 0, "below threshold no fire");
    soul_behavior_tick(11);
    CHECK(s_exec_a == 0, "still blocked");
}

static void test_defer(void)
{
    reg();
    soul_bb_set(SOUL_BB_M_STIM, 1.0f);    /* b score=1 */
    soul_bb_set(SOUL_BB_PLAYING, 0.0f);
    soul_bb_set(SOUL_BB_QUIET, 0.0f);
    soul_bb_tick(200);
    soul_behavior_defer("b", 7200);
    CHECK(soul_behavior_deferred("b"), "deferred immediately");
    soul_behavior_tick(200);
    CHECK(s_exec_b == 0, "deferred blocks fire");
    soul_behavior_tick(200 + 7200);
    CHECK(s_exec_b == 1, "fires after defer expiry");
    CHECK(!soul_behavior_deferred("b"), "defer cleared");
}

static void test_never_fired_not_cooldown_blocked(void)
{
    reg();
    /* 冷却语义陷阱: last_fire=0 ≠ 已触发 —— 首拍必须可触发 */
    soul_bb_set(SOUL_BB_M_STIM, 0.9f);
    soul_bb_set(SOUL_BB_PLAYING, 0.0f);
    soul_bb_set(SOUL_BB_QUIET, 0.0f);
    soul_behavior_tick(0);
    CHECK(s_exec_b == 1, "first tick at t=0 fires");
}

static void test_weighted_rng_deterministic(void)
{
    reg();
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    soul_behavior_set_seed(7);
    for (int t = 0; t < 50; t++) soul_behavior_tick((uint32_t)t);
    int first = s_exec_a;
    reg();
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    soul_behavior_set_seed(7);
    for (int t = 0; t < 50; t++) soul_behavior_tick((uint32_t)t);
    CHECK(first == s_exec_a, "same seed -> same fire count");
    CHECK(first > 0, "fired at least once");
}

static void test_null_exec_observed_only(void)
{
    reg();
    /* exec=NULL 观察位: 评分记账但不执行, 且不影响 a 的仲裁胜出 */
    soul_behavior_register(&(soul_bhv_t){
        .name = "watch", .cons = c_a, .cons_n = 2,
        .threshold = 0.1f, .w = 1.0f, .cooldown_s = 0, .exec = NULL,
    });
    soul_bb_set(SOUL_BB_QUIET, 1.0f);
    soul_bb_set(SOUL_BB_PLAYING, 1.0f);
    soul_behavior_tick(300);
    soul_behavior_tick(400);
    CHECK(s_exec_a >= 1, "a still wins despite watch scoring high");
    CHECK(soul_behavior_score(2) > 0, "watch scored");
}

static void test_override_ttl(void)
{
    reg();
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
    reg();
    soul_bb_salience(0.8f);
    for (int i = 0; i < 60; i++) soul_bb_tick((uint32_t)i);
    float v = soul_bb_get(SOUL_BB_SALIENCE);
    CHECK(v < 0.8f * 0.9885f * 0.9885f, "salience decays");
    CHECK(v > 0.3f, "salience ~half-life 60s");
    for (int i = 60; i < 800; i++) soul_bb_tick((uint32_t)i);
    CHECK(FEQ(soul_bb_get(SOUL_BB_SALIENCE), 0.0f), "salience floors to 0");
}

int main(void)
{
    test_curve_math();
    test_fire_and_cooldown();
    test_threshold_blocks();
    test_defer();
    test_never_fired_not_cooldown_blocked();
    test_weighted_rng_deterministic();
    test_null_exec_observed_only();
    test_override_ttl();
    test_salience_decay();
    if (s_fail == 0) {
        printf("test_soul_behavior: ALL PASS\n");
        return 0;
    }
    printf("test_soul_behavior: %d FAIL\n", s_fail);
    return 1;
}
