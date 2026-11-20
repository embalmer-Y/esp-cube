/*
 * soul_moodgate.c — mood 回写三断法实现(S6 D4; 见 soul_moodgate.h)
 *
 * 状态: 轮计数(隔轮)/AR 环(50 槽, 浮点)/暂停截止。确定性; 零分配。
 */
#include "soul_moodgate.h"

#include <math.h>
#include <string.h>

#define MG_AR_N        50
#define MG_AR_GATE     0.8f
#define MG_PAUSE_S     (24u * 3600)
#define MG_DECAY       0.5f

static struct {
    uint32_t last_applied_round;    /* 上次生效轮(隔轮断) */
    bool     have_round;
    float    ar[MG_AR_N];           /* 生效值环(最旧在前)  */
    int      ar_n;
    uint32_t pause_until;           /* 暂停截止(秒)         */
    bool     pause_seen;            /* now_s 时基首见       */
    uint32_t last_now;
} s_mg;

void soul_moodgate_reset(void)
{
    memset(&s_mg, 0, sizeof(s_mg));
}

bool soul_moodgate_paused(uint32_t now_s)
{
    if (!s_mg.pause_seen) {
        s_mg.pause_seen = true;
        s_mg.last_now = now_s;
    }
    if (s_mg.pause_until == 0)
        return false;
    if (now_s >= s_mg.pause_until) {
        s_mg.pause_until = 0;
        return false;
    }
    return true;
}

/* AR(1) 滞后-1 自相关(环内有效样本; <3 样本不判) */
static float ar1(void)
{
    if (s_mg.ar_n < 3)
        return 0.0f;
    int n = s_mg.ar_n;
    const float *a = s_mg.ar;
    float ma = 0.0f;
    for (int i = 0; i < n; i++)
        ma += a[i];
    ma /= (float)n;
    float num = 0.0f, den = 0.0f;
    for (int i = 1; i < n; i++)
        num += (a[i] - ma) * (a[i - 1] - ma);
    for (int i = 0; i < n; i++)
        den += (a[i] - ma) * (a[i] - ma);
    if (den <= 1e-12f)
        return 0.0f;
    return num / den;
}

bool soul_moodgate_allow(uint32_t round, int engine_trend,
                         float llm_delta, float *applied)
{
    if (applied == NULL)
        return false;
    *applied = 0.0f;
    if (llm_delta == 0.0f)
        return false;                        /* 零回写不占隔轮账 */
    /* 断一: 隔轮采纳(≤1/2 轮) */
    if (s_mg.have_round && round <= s_mg.last_applied_round + 1)
        return false;
    /* 断二: ×0.5 */
    float v = llm_delta * MG_DECAY;
    /* 断三: 方向失配再 ×0.5(engine_trend 非 0 且异号) */
    if (engine_trend != 0 &&
        ((v > 0.0f && engine_trend < 0) || (v < 0.0f && engine_trend > 0)))
        v *= MG_DECAY;
    if (v == 0.0f)
        return false;
    *applied = v;
    s_mg.last_applied_round = round;
    s_mg.have_round = true;
    return true;
}

void soul_moodgate_record(float applied)
{
    if (s_mg.ar_n < MG_AR_N) {
        s_mg.ar[s_mg.ar_n++] = applied;   /* 填充期直接追加 */
    } else {
        memmove(s_mg.ar, s_mg.ar + 1,
                sizeof(float) * (size_t)(MG_AR_N - 1));
        s_mg.ar[MG_AR_N - 1] = applied;   /* 满环滑动丢最旧 */
    }
    /* 自增强指纹: AR(1)>0.8 → 暂停 24h(以最近一次 now_s 计) */
    if (ar1() > MG_AR_GATE)
        s_mg.pause_until = s_mg.last_now + MG_PAUSE_S;
}
