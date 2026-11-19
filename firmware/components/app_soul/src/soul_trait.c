/*
 * soul_trait.c — 性格 trait 层实现(S2.5a, docs/24 §3/§11.3 定案)
 *
 * 公式全部 docs/24 §3.2(ke 白名单表)+§11.3(ethics/kd 性格化表)的定案值;
 * 确定性: 零 RNG/零分配; 状态文件级 static, trait_reset 全清。
 * S2.5a: 静态底座(建角色时定/模拟器注入); S2.5b 将挂 EMA/反思双通道
 * (docs/24 §3.3: 残差密度输入+24h 冷却+带宽钳位, 防 ALMA 式自激励)。
 */
#include "soul_trait.h"

#include <string.h>

#include "soul_drive.h"
#include "soul_emotion.h"

#define TRAIT_BASE 0.5f

static float s_tr[TRAIT_DIM];
static trait_tune_t s_tune;

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

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
    tune_recalc();
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
