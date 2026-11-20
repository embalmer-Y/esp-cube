/*
 * soul_trait.c — 性格 trait 层实现(S2.5a 静态底座+S2.5b 双通道, docs/24)
 *
 * 公式全部 docs/24 §3.2(ke 白名单表)+§11.3(ethics/kd 性格化表)的定案值;
 * S2.5b 双通道(§3.3)+参数表(§3.5): EMA α=0.03/日, 弹簧 0.005/日, 单步
 * 钳 0.05, 7 日窗 warmup, 互斥取大者, 熔断 0.75×band→冻 7 天。
 * 确定性: 零 RNG/零分配; 状态文件级 static, trait_reset 全清。
 * x[d] 全部为"残差密度"(观测 − trait 预测基线, §3.3 防 ALMA 自激励;
 * 逐维公式为本批 v1 标定, 30 天模拟验收=唯一裁判, 见 soul_accept C5)。
 */
#include "soul_trait.h"

#include <math.h>
#include <string.h>

#include "soul_drive.h"
#include "soul_emotion.h"

#define TRAIT_BASE 0.5f

/* §3.5 参数表 */
#define TR_EMA_ALPHA    0.03f   /* τ≈23 天                      */
#define TR_SPRING_K     0.005f  /* 回底座弹簧/日                */
#define TR_MAX_STEP     0.05f   /* 单步限幅(=PSN_EVO_MAX_DELTA) */
#define TR_WINDOW       7       /* 净移回看窗                   */
#define TR_WARMUP       7       /* 7 日窗≥7 样本才生效          */
#define TR_FREEZE_DAYS  7
#define TR_INIT_K       200.0f  /* 发起次数密度换算(§3.3 次数/k) */
#define TR_NEGV_K       0.22f   /* 韧性预测基线系数(30 天标定: 中性
                                  * life 月 negv≈0.11=(1-0.5)*K) */
/* 残差基线常数(§3.5 "30 天模拟标定"产物, 2026-11-20 life_month 实测):
 * 中性性格 × 普通生活(life 月测)的密度均值——x[d] 以此为零点, 正常
 * 生活不漂移(否则任何部署里 trait 都单调漂到 band 边缘); neglect/
 * abuse 等异常密度自然偏离零点 → 通道一开动。 */
#define TR_K_URG        0.89f   /* life 月 urgency 均值(kd 误差慢积累) */
#define TR_K_BOND       0.34f   /* life 月 bond 均值                    */
#define TR_K_SAL        0.003f  /* life 月 salience 均值                */

static float s_tr[TRAIT_DIM];
static trait_tune_t s_tune;

/* S2.5b 双通道状态 */
static struct {
    double sum_ar, sum_nov, sum_urg, sum_bond, sum_negv;
    uint32_t n_obs, n_init;
} s_stat;
static float s_ema_last[TRAIT_DIM];       /* 最近 EMA 步(审计)     */
static float s_reflect_today[TRAIT_DIM];  /* 当日反思已施加(互斥)  */
static bool  s_reflect_moved;             /* 当日反思有生效        */
static int   s_last_reflect_day = -1000;  /* 上次反思生效结算日    */
static int   s_last_reflect_dir[TRAIT_DIM]; /* 0=无 -1=负 +1=正    */
static float s_valhist[TRAIT_DIM][TR_WINDOW]; /* 结算值环形(净移)  */
static int   s_hist_head;                 /* 下写位(=最旧值)       */
static int   s_hist_n;                    /* 刷新以来已写样本数    */
static uint8_t s_frozen[TRAIT_DIM];       /* 剩余冻结天数          */
static uint32_t s_days;                   /* 已结算天数            */

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static void hist_refresh(void);

/* 调制系数导出(§3.2+§11.3 公式; 全部钳位到文档值域) */
static void tune_recalc(void)
{
    const float *tr = s_tr;
    struct ke_tune ke;

    /* frag/selfs 复合量(§11.3): 脆弱=低韧性+高温情; 自足=高活力+好奇 */
    s_tune.frag = clampf(0.6f * (1.0f - tr[TR_RESILIENCE]) +
                         0.4f * tr[TR_WARMTH], 0.0f, 1.0f);

    /* ke 白名单 */
    s_tune.arousal_base = clampf(0.10f + 0.20f * (tr[TR_ENERGY] - 0.5f),
                                 0.0f, 0.20f);
    s_tune.valence_off = 0.10f * (tr[TR_WARMTH] - 0.5f);
    s_tune.tau_dec_mod = clampf(1.0f - 0.4f * (tr[TR_RESILIENCE] - 0.5f),
                                0.8f, 1.2f);
    s_tune.bond_up_mod = clampf(1.0f + 0.5f * (tr[TR_WARMTH] - 0.5f),
                                0.75f, 1.25f);
    s_tune.urg_on = clampf(0.10f - 0.04f * (tr[TR_ASSERT] - 0.5f),
                           0.08f, 0.12f);
    s_tune.novelty_gain_mod = clampf(1.0f + 0.6f * (tr[TR_CURIOSITY] - 0.5f),
                                     0.7f, 1.3f);

    /* ethics 触发面。锚原则: frag=0.5 ⇒ 全部≡S2 中性常数(gate 0.35/
     * 阈 2.5/剂量 1×——黄金锚不动, 性格只在 band 内偏移)。脆弱档校验:
     * frag=0.65(R0.35+W0.65) ⇒ gate 0.185 < 冷落可达 |v|≈0.21 →
     * 纯冷落可闩; 中性 0.35 > 0.21 → 不可闩(S2 语义保持); 皮实 0.465。 */
    s_tune.eth_gate = clampf(0.35f - 1.1f * (s_tune.frag - 0.5f),
                             0.08f, 0.55f);
    s_tune.eth_dose_mod = clampf(1.0f + 1.8f * (s_tune.frag - 0.5f),
                                 0.6f, 1.5f);
    s_tune.eth_load_t = clampf(2.5f - 1.8f * (s_tune.frag - 0.5f),
                               1.6f, 3.2f);
    s_tune.eth_heal_tau_mod = clampf(1.0f + 1.34f * (s_tune.frag - 0.5f),
                                     0.5f, 1.67f);
    s_tune.eth_soothe_mod = clampf(1.0f + 0.5f * (tr[TR_WARMTH] - 0.5f),
                                   0.75f, 1.25f);

    /* kd 节奏(锚: W=E=0.5 ⇒ 1.0×/0)。自娱回充零点=0.5(中性与低温情
     * 角色 stim 照常代谢=S2 行为); E=0.65 ⇒ 8e-5=2×代谢 → stim 自足。 */
    s_tune.social_met_mod = clampf(0.5f + tr[TR_WARMTH], 0.5f, 1.5f);
    s_tune.stim_self_recover =
        8.0e-5f * clampf((tr[TR_ENERGY] - 0.5f) / 0.15f, 0.0f, 1.0f);

    /* 立即应用到 ke 运行调谐 */
    ke.arousal_base = s_tune.arousal_base;
    ke.valence_off = s_tune.valence_off;
    ke.tau_dec_mod = s_tune.tau_dec_mod;
    ke.bond_up_mod = s_tune.bond_up_mod;
    ke.urg_on = s_tune.urg_on;
    ke.novelty_gain_mod = s_tune.novelty_gain_mod;
    ke.eth_gate = s_tune.eth_gate;
    ke.eth_dose_mod = s_tune.eth_dose_mod;
    ke.eth_load_t = s_tune.eth_load_t;
    ke.eth_heal_tau_mod = s_tune.eth_heal_tau_mod;
    ke.eth_soothe_mod = s_tune.eth_soothe_mod;
    ke_tune_set(&ke);

    trait_tune_apply_kd();
}

void trait_tune_apply_kd(void)
{
    /* 基于内置冻结表 × 调制覆写(kd_init 契约: 参数静态生存期) */
    static struct kd_params p;
    const struct kd_params *def = kd_params_default();

    p = *def;
    p.metabolic[KD_SOCIAL] = def->metabolic[KD_SOCIAL] * s_tune.social_met_mod;
    p.stim_self_recover = s_tune.stim_self_recover;
    kd_init(&p);
}

void trait_reset(void)
{
    for (int i = 0; i < TRAIT_DIM; i++)
        s_tr[i] = TRAIT_BASE;
    memset(&s_stat, 0, sizeof(s_stat));
    memset(s_ema_last, 0, sizeof(s_ema_last));
    memset(s_reflect_today, 0, sizeof(s_reflect_today));
    s_reflect_moved = false;
    s_last_reflect_day = -1000;
    memset(s_last_reflect_dir, 0, sizeof(s_last_reflect_dir));
    memset(s_valhist, 0, sizeof(s_valhist));
    memset(s_frozen, 0, sizeof(s_frozen));
    s_days = 0;
    tune_recalc();
    hist_refresh();
}

bool trait_set(const float tr[TRAIT_DIM])
{
    if (tr == NULL)
        return false;
    for (int i = 0; i < TRAIT_DIM; i++) {
        if (tr[i] < TRAIT_BASE - TRAIT_BAND || tr[i] > TRAIT_BASE + TRAIT_BAND)
            return false;
    }
    memcpy(s_tr, tr, sizeof(s_tr));
    tune_recalc();
    hist_refresh();               /* genesis/加载=新纪元, 净移重新起算 */
    return true;
}

void trait_get(float tr[TRAIT_DIM])
{
    if (tr != NULL)
        memcpy(tr, s_tr, sizeof(s_tr));
}

const trait_tune_t *trait_tune(void)
{
    return &s_tune;
}

/* ---- S2.5b 双通道(docs/24 §3.3/§3.5) ---- */

/* 净移窗基线刷新(=新纪元: 出厂底座/genesis 建角色/角色加载)。窗内
 * 全部填当前值, 下一次结算起对照新基线算 7 日净移。 */
static void hist_refresh(void)
{
    for (int d = 0; d < TRAIT_DIM; d++)
        for (int k = 0; k < TR_WINDOW; k++)
            s_valhist[d][k] = s_tr[d];
    s_hist_head = 0;
    s_hist_n = 0;
}

void trait_stat_observe(const struct trait_obs *o)
{
    if (o == NULL)
        return;
    s_stat.sum_ar += clampf(o->arousal, 0.0f, 1.0f);
    s_stat.sum_nov += clampf(o->salience, 0.0f, 1.0f);
    s_stat.sum_urg += clampf(o->urgency, 0.0f, 1.0f);
    s_stat.sum_bond += clampf(o->social_bond, 0.0f, 1.0f);
    /* 韧性观测: 负效价密度(减 valence_off=trait 预测的显示抬升) */
    s_stat.sum_negv += fmaxf(0.0f, -(o->valence - s_tune.valence_off));
    if (o->initiative)
        s_stat.n_init++;
    s_stat.n_obs++;
}

void trait_ema_settle(void)
{
    if (s_stat.n_obs == 0)
        return;                       /* 无观测日不计窗(warmup 语义) */
    s_days++;

    /* 残差密度 x[d]: 0.5 + (观测 − 生活基线 TR_K_* − trait 预测增量)。
     * trait 增量=该性状抬升其观测通道的部分(减去即残差, 自阻尼)。 */
    float n = (float)s_stat.n_obs;
    float m_ar = (float)(s_stat.sum_ar / n);
    float m_sal = (float)(s_stat.sum_nov / n);   /* salience(历史字段名) */
    float m_urg = (float)(s_stat.sum_urg / n);
    float m_bond = (float)(s_stat.sum_bond / n);
    float m_negv = (float)(s_stat.sum_negv / n);
    float x[TRAIT_DIM];
    /* 能量 = arousal 残差(arousal_base 即 trait 预测基线) + 发起密度 */
    x[TR_ENERGY] = clampf(0.5f + 3.0f * (m_ar - s_tune.arousal_base) +
                              (float)s_stat.n_init / TR_INIT_K, 0.0f, 1.0f);
    /* 温情 = 关系密度(生活基线之上) − bond 上行调制 */
    x[TR_WARMTH] = clampf(0.5f + (m_bond - TR_K_BOND) -
                              (s_tune.bond_up_mod - 1.0f), 0.0f, 1.0f);
    /* 好奇 = 新异事件流密度 − novelty 增益调制 */
    x[TR_CURIOSITY] = clampf(0.5f + (m_sal - TR_K_SAL) -
                                 (s_tune.novelty_gain_mod - 1.0f),
                             0.0f, 1.0f);
    /* 主见 = urgency 密度对生活基线的超出 − 门槛下移(urg_on 归一化锚
     * 见 soul_emotion urg_raw; K_URG 吸收场景均值) */
    x[TR_ASSERT] = clampf(0.5f + 2.5f * ((m_urg - TR_K_URG) -
                                             (s_tune.urg_on - 0.10f)),
                          0.0f, 1.0f);
    /* 韧性 = 0.5 − 2×(负效价密度 − 该性状预测的负性基线(1−R)·K)。
     * 自阻尼: R↑ → 预测基线↓ → 同观测下 x_R↓ → 拉回(ALMA 防护) */
    x[TR_RESILIENCE] = clampf(0.5f - 2.0f * (m_negv - (1.0f -
                                  s_tr[TR_RESILIENCE]) * TR_NEGV_K),
                              0.0f, 1.0f);

    /* EMA 步(§3.3 通道一)。warmup 未满: 只积累不移动。 */
    memset(s_ema_last, 0, sizeof(s_ema_last));
    if (s_days > TR_WARMUP) {
        for (int d = 0; d < TRAIT_DIM; d++) {
            if (s_frozen[d])
                continue;             /* 冻结维两通道全停        */
            float delta = TR_EMA_ALPHA * (x[d] - s_tr[d]) +
                          TR_SPRING_K * (TRAIT_BASE - s_tr[d]);
            delta = clampf(delta, -TR_MAX_STEP, TR_MAX_STEP);
            /* 互斥(§3.3): 反思先行当日已生效; 该维反思步≥EMA 步时 EMA
             * 让路, 否则 EMA 从新基线续走自己的步(delta 恒对当前值
             * 计算, 非对旧基线叠加——当日主导通道=|Δ| 大者)。 */
            if (s_reflect_moved &&
                fabsf(s_reflect_today[d]) >= fabsf(delta))
                delta = 0.0f;
            s_tr[d] = clampf(s_tr[d] + delta,
                             TRAIT_BASE - TRAIT_BAND,
                             TRAIT_BASE + TRAIT_BAND);
            s_ema_last[d] = delta;
        }
    }

    /* 净移窗+熔断(§3.3): 7 日净移>0.75×band → 冻结该维 7 天。
     * 窗满(刷新后≥7 个结算样本)才判定, 逐日滑动对照——字面口径
     * "7 日窗≥7 样本"; genesis 后未满窗的爆发由 band 钳位兜底。 */
    for (int d = 0; d < TRAIT_DIM; d++) {
        float oldest = s_valhist[d][s_hist_head];
        s_valhist[d][s_hist_head] = s_tr[d];
        if (s_hist_n >= TR_WINDOW && !s_frozen[d] &&
            fabsf(s_tr[d] - oldest) > 0.75f * TRAIT_BAND) {
            s_frozen[d] = TR_FREEZE_DAYS;
            for (int k = 0; k < TR_WINDOW; k++)
                s_valhist[d][k] = s_tr[d];   /* 解冻后重新起算 */
        }
    }
    s_hist_head = (s_hist_head + 1) % TR_WINDOW;
    if (s_hist_n < TR_WINDOW)
        s_hist_n++;

    /* 冻结倒计时与当日记账清零 */
    for (int d = 0; d < TRAIT_DIM; d++)
        if (s_frozen[d])
            s_frozen[d]--;
    memset(&s_stat, 0, sizeof(s_stat));
    memset(s_reflect_today, 0, sizeof(s_reflect_today));
    s_reflect_moved = false;
}

bool trait_reflect_apply(const struct trait_reflect *r)
{
    if (r == NULL || r->ref_depth > 2)
        return false;                 /* 引用深度>2 = 脱根, 整包拒绝 */
    if (r->conf < 0.0f || r->conf > 1.0f)
        return false;
    if ((int)s_days - s_last_reflect_day < 1)
        return false;                 /* 24h 冷却(结算日粒度)       */

    float applied[TRAIT_DIM];
    bool any = false;
    for (int d = 0; d < TRAIT_DIM; d++) {
        float dv = clampf(r->delta[d], -TR_MAX_STEP, TR_MAX_STEP);
        if (dv == 0.0f || s_frozen[d]) {
            applied[d] = 0.0f;    /* 冻结维两通道全停(自动护栏) */
            continue;
        }
        /* 连续同向需 evidence≥2 且 conf≥0.6(§3.3) */
        int dir = dv > 0.0f ? 1 : -1;
        if (s_last_reflect_dir[d] == dir &&
            !(r->evidence_n >= 2 && r->conf >= 0.6f)) {
            applied[d] = 0.0f;
            continue;
        }
        float nv = clampf(s_tr[d] + dv,
                          TRAIT_BASE - TRAIT_BAND,
                          TRAIT_BASE + TRAIT_BAND);
        applied[d] = nv - s_tr[d];
        if (fabsf(applied[d]) > 1e-6f) {
            s_tr[d] = nv;
            s_last_reflect_dir[d] = dir;
            any = true;
        }
    }
    if (!any)
        return false;
    memcpy(s_reflect_today, applied, sizeof(applied));
    s_reflect_moved = true;
    s_last_reflect_day = (int)s_days;
    tune_recalc();
    return true;
}

bool trait_frozen(int dim)
{
    return dim >= 0 && dim < TRAIT_DIM && s_frozen[dim] > 0;
}

uint32_t trait_day_count(void)
{
    return s_days;
}

float trait_last_ema(int dim)
{
    return (dim >= 0 && dim < TRAIT_DIM) ? s_ema_last[dim] : 0.0f;
}
