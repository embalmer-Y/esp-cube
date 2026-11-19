/*
 * soul_behavior.c —— L2 评分器 + L3 行为注册表实现(docs/14 §5)
 *
 * 纯 C 零平台依赖(宿主单测可编, 与 kd/ke 同纪律); 只读黑板。
 * RNG = xorshift32(确定性; 产品低温加权随机足够, 换 esp_random
 * 会破单测可复现性 —— 有意为之)。
 */
#include "soul_behavior.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SOUL_BHV_NAME_MAX 16

struct bhv_slot {
    char         name[SOUL_BHV_NAME_MAX];
    soul_bhv_t   def;
    float        last_score;
    bool         ever_fired;    /* 冷却判据(0s 时刻未触发≠已触发) */
    uint32_t     last_fire;
    uint32_t     defer_until;    /* 退避截止(单调秒) */
    /* E3(docs/23 §2.5, 2026-11-19): 习惯化+多样性状态。hab=leaky 记忆
     * (执行↑, 静默自发恢复), 权重调制见 tick; hab_last=上次 tick 秒
     * (dt 时基, 0=尚未 tick 过)。 */
    float        hab;
    uint32_t     hab_last;
};

static struct bhv_slot s_tab[SOUL_BHV_MAX];
static int      s_cnt;
static uint32_t s_rng  = 0xC0FFEEu;
static uint32_t s_now;           /* 最近一拍单调秒(defer 时基) */

static float clamp01(float x)
{
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

static uint32_t rng_next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* 响应曲线(consideration: 因子→[0,1]) */
static float curve_eval(const soul_cons_t *c)
{
    float x = clamp01(soul_bb_get(c->key) * c->k);
    switch (c->curve) {
    case SOUL_C_RISE: return x;
    case SOUL_C_FALL: return 1.0f - x;
    case SOUL_C_BAND: return 1.0f - (x > 0.5f ? 2 * x - 1 : 1 - 2 * x);
    default:          return 1.0f;
    }
}

static int find(const char *name)
{
    for (int i = 0; i < s_cnt; i++) {
        if (strcmp(s_tab[i].name, name) == 0) return i;
    }
    return -1;
}

int soul_behavior_register(const soul_bhv_t *b)
{
    if (b == NULL || b->name == NULL || b->name[0] == '\0') return -1;
    int id = find(b->name);
    if (id < 0) {
        if (s_cnt >= SOUL_BHV_MAX) return -1;
        id = s_cnt++;
    }
    struct bhv_slot *s = &s_tab[id];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", b->name);
    s->def = *b;
    return id;
}

void soul_behavior_defer(const char *name, uint32_t sec)
{
    int id = find(name);
    if (id < 0) return;
    s_tab[id].defer_until = s_now + sec;
}

bool soul_behavior_deferred(const char *name)
{
    int id = find(name);
    return id >= 0 && s_tab[id].defer_until > s_now;
}

float soul_behavior_score(int id)
{
    if (id < 0 || id >= s_cnt) return -1;
    float s = s_tab[id].def.w;
    for (uint8_t i = 0; i < s_tab[id].def.cons_n; i++) {
        s *= curve_eval(&s_tab[id].def.cons[i]);
    }
    if (s < 0) s = 0;
    s_tab[id].last_score = s;
    return s;
}

uint32_t soul_behavior_last_fire(int id)
{
    if (id < 0 || id >= s_cnt) return 0;
    return s_tab[id].last_fire;
}

void soul_behavior_tick(uint32_t now_s)
{
    s_now = now_s;

    /* 候选收集: 过阈值 ∧ 过冷却 ∧ 未退避。
     * E3: 权重三重调制(Rankin 习惯化+Cozmo recency+count bonus):
     *   hab 休眠恢复 −= dt·BH_HAB_RECOVER(指数自发恢复);
     *   w = score × 1/(1+hab²) × [recency< 执行间隔则乘 BH_RECENCY_PEN]
     *       + BH_NOVELTY_BONUS/(久未执行秒数开方) —— 冷却打满的恒过
     *       阈值行为被重复疲劳压低, 久未执行的自动升权, 10% 级随机性
     *       已由既有 WTA 提供。 */
    static const float BH_HAB_RECOVER = 6.0e-4f; /* ≈28min 常数(冷却 480s
                                                  * 仅恢复 0.29<0.3, 连打
                                                  * hab 累积 0.3→0.6→0.9) */
    static const float BH_HAB_ONFIRE  = 0.55f;   /* 单次执行习惯化增量:
                                                  * hab 序列 0.55→1.1 帽?不,clamp
                                                  * 实际 0.55→0.81(帽 1)→eff 0.45
                                                  * <th 0.5 → 第 3 次必跳 */
    static const float BH_RECENCY_PEN = 0.25f;   /* 间隔<冷却×0.5 的乘数 */
    static const float BH_NOVELTY_BONUS = 0.35f; /* bonus 系数(×1/√dt) */

    int   cand[SOUL_BHV_MAX];
    float w[SOUL_BHV_MAX];
    int   n = 0;
    float wsum = 0;

    for (int i = 0; i < s_cnt; i++) {
        float s = soul_behavior_score(i);
        /* habituation 记忆自发恢复(每拍) */
        float dt = (float)(now_s - s_tab[i].hab_last);
        if (s_tab[i].hab_last != 0 && dt > 0) {
            s_tab[i].hab -= dt * BH_HAB_RECOVER;
            if (s_tab[i].hab < 0) s_tab[i].hab = 0;
        }
        if (s_tab[i].hab > 1.0f) s_tab[i].hab = 1.0f;
        s_tab[i].hab_last = now_s;
        if (s < s_tab[i].def.threshold) continue;
        uint16_t cd = s_tab[i].def.cooldown_s;
        if (s_tab[i].ever_fired && cd != 0 &&
            now_s - s_tab[i].last_fire < cd) continue;
        if (s_tab[i].defer_until > now_s) continue;
        if (s_tab[i].def.exec == NULL) continue;   /* 观察位不执行 */

        /* 习惯化作闸门: 有效分 = score × (1−hab) — 重复疲劳直接压低
         * 得分而非只调赢面(候选唯一时权重调制无效的教训); hab 高到
         * 1-score 时行为自然落回阈值下, 等 hab 衰减"新鲜感恢复"。 */
        float eff = s * (1.0f - s_tab[i].hab);
        if (eff < s_tab[i].def.threshold) continue;
        float weight = eff;
        if (s_tab[i].ever_fired) {
            uint32_t since = now_s - s_tab[i].last_fire;
            if (cd != 0 && since < cd / 2) {
                weight *= BH_RECENCY_PEN;          /* 刚执行过→压 */
            }
            weight += BH_NOVELTY_BONUS /
                      (sqrtf((float)(since > 1 ? since : 1)));
        }
        cand[n] = i;
        w[n] = weight;
        wsum += weight;
        n++;
    }
    if (n == 0 || wsum <= 0) return;

    /* 加权随机 WTA(低温: 分高者大概率胜, 平分时非确定) */
    float r = (float)(rng_next() & 0xFFFF) / 65535.0f * wsum;
    int win = cand[n - 1];
    for (int i = 0; i < n; i++) {
        r -= w[i];
        if (r <= 0) { win = cand[i]; break; }
    }

    s_tab[win].ever_fired = true;
    s_tab[win].last_fire = now_s;
    s_tab[win].hab += BH_HAB_ONFIRE;               /* 习惯化上行 */
    s_tab[win].def.exec();
}

void soul_behavior_dump(char *buf, size_t cap)
{
    size_t off = 0;
    for (int i = 0; i < s_cnt && off + 1 < cap; i++) {
        int n = snprintf(buf + off, cap - off,
                         "%s[%s] score=%.2f th=%.2f",
                         i ? "\n" : "", s_tab[i].name,
                         s_tab[i].last_score, s_tab[i].def.threshold);
        if (n < 0 || (size_t)n >= cap - off) { off = cap - 1; break; }
        off += (size_t)n;
    }
    buf[off] = '\0';
}

void soul_behavior_set_seed(uint32_t seed)
{
    s_rng = seed ? seed : 0xC0FFEEu;
}

void soul_behavior_reset(void)
{
    s_cnt = 0;
    s_now = 0;
    memset(s_tab, 0, sizeof(s_tab));
}
