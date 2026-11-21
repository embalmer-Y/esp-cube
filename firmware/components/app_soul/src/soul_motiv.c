/*
 * soul_motiv.c — kc/ku/kf/kb 四维动机 + 显著性仲裁器 + 令牌桶实现
 * (S5/B-3, docs/24 §4.1/§4.2/§4.5; 见 soul_motiv.h)
 *
 * 结构: kc=台账槽蒸发和+缺席封顶槽+KL 惊奇充能; ku=遗留槽老化×1.02^Δmin
 * /(1+h)+事件余辉; kf=非对称 EMA; kb=五态事件推导。缓存值在 sm_tick
 * 刷新(单写者=tick 侧), sm_gate 只读缓存。
 * 确定性: 零 RNG/零分配/零墙钟(调用方供给单调 now_s/now_min)。
 * R6: 只决定"是否触发深思", 行动出口分账在 soul_budget(S3)。
 */
#include "soul_motiv.h"

#include <math.h>
#include <string.h>

/* ---- 参数(docs/24 §4.1/§4.2 工作初值) ---- */
static const struct sm_cfg SM_CFG_DEFAULT = {
    .q_tau_min = 480.0f,        /* kc 槽蒸发 τ               */
    .absence_cap = 0.25f,       /* 缺席缺口槽权重封顶        */
    .kc_gate = 0.45f,
    .skl_gate = 0.3f,
    .skl_gain = 0.4f,
    .ku_grow = 1.02f,           /* ×1.02^Δmin(CHI'25 逐字)   */
    .ku_thresh = 0.8f,
    .ku_cap = 1.5f,
    .ku_after_tau_s = 600.0f,
    .ku_wake = 0.6f,
    .ku_barge = 0.8f,
    .ku_touch = 0.2f,
    .ku_silence_s = 10,
    .kf_an = 0.30f,
    .kf_ap = 0.10f,
    .kf_frustrate = 0.55f,
    .kb_away_min = 120,
    .kb_attend_min = 45,
    .kb_cost_free = 1.0f,
    .kb_cost_attended = 0.6f,
    .kb_p_gate = 0.35f,
    .s_gate = 0.5f,
    .s_overdraft = 0.8f,
    .tb_capacity = 3,
    .tb_daily = 12,
    .dedup_min = 5,
    .habit_recover = 0.7f,      /* ρ /min                    */
};

static struct sm_cfg s_cfg;
static bool s_inited;             /* 防呆: 未 reset 即用 → 自愈
                                   * (板上 soul_start 漏调 sm_reset 时
                                   * s_cfg 全零=门全开, S5 板验实证) */

/* kc: 问题台账(强 w0×exp(−Δt/τ); 缺席槽=调用方以 w≤absence_cap 开) */
struct q_slot {
    bool open;
    float w0;
    uint32_t t0_s;              /* 开槽单调秒(蒸发时基)      */
};
static struct q_slot s_q[SM_QSLOTS];
static float s_kc;              /* tick 缓存                 */

/* kc: 交互间隔 8 桶 KL(当日直方图 + 昨日冻结基线) */
#define SM_KL_BINS 8
static uint8_t s_kl_cur[SM_KL_BINS];
static uint8_t s_kl_ref[SM_KL_BINS];
static uint32_t s_last_interact_s;
static bool     s_have_interact;
static float    s_skl;              /* 最近一次间隔 KL[0,1]      */
static float    s_kl_burst;         /* 惊奇事件充能当前值        */
static float    s_kl_burst0;        /* 充能初值(自 t0 全量衰减)  */
static float    s_surprise_acc;     /* B-6b: 反思触发惊奇累计(Σ s_kl×100;
                                     * ≥40 触发反思, docs/24 §3.5) */
static uint32_t s_kl_burst_t0;

/* ku: 遗留意图槽 {s, h 被忽次数} + 事件余辉 */
static struct {
    float s;
    uint8_t h;
    bool armed;
    uint32_t t0_s;
    float after;                /* 余辉累积(τ 衰减)          */
    bool after_on;
    uint32_t after_t0_s;
} s_ku;
static float s_ku_cache;

/* kf: 非对称 EMA */
static float s_kf;

/* kb 输入缓存(tick 供给) + 五态缓存 */
static struct {
    bool quiet_hours;
    bool ai_active;
    bool playing;
    uint32_t last_user_s;
    bool have_user;
} s_kb_in;
static enum sm_kb s_kb;
static float s_avail;

/* 令牌桶 + 习惯化 */
static struct {
    float tokens;               /* 可到 −1(透支债)           */
    uint32_t day;
    bool day_seen;
    uint32_t last_accrual_min;  /* 上次累积时基(日补 12 按
                                 * 12/1440 per min 滴灌, 帽=容量) */
    bool accrual_seen;
    float habit;                /* n(触发 +1, 恢复 ρ/min)    */
} s_tb;

/* 仲裁材料(tick 侧经 sm_event 记录; gate 消费) */
static enum sm_evt s_evt;
static uint32_t s_low_ar_last_min;
static bool     s_low_ar_seen;
/* per-key 去重(docs §4.2 "5min 同 key"; key=触发通道) */
static uint32_t s_last_trig_min[5];
static bool     s_last_trig_valid[5];
/* 习惯化恢复时基(ρ=0.7/min 按真实间隔) */
static uint32_t s_habit_last_min;
static bool     s_habit_seen;

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

void sm_reset(void)
{
    s_inited = true;
    s_cfg = SM_CFG_DEFAULT;
    memset(s_q, 0, sizeof(s_q));
    memset(s_kl_cur, 0, sizeof(s_kl_cur));
    memset(s_kl_ref, 0, sizeof(s_kl_ref));
    s_last_interact_s = 0;
    s_have_interact = false;
    s_skl = 0.0f;
    s_kl_burst = 0.0f;
    s_kl_burst_t0 = 0;
    memset(&s_ku, 0, sizeof(s_ku));
    s_surprise_acc = 0.0f;
    s_ku_cache = 0.0f;
    s_kf = 0.5f;
    memset(&s_kb_in, 0, sizeof(s_kb_in));
    s_kb = SM_KB_AWAY;
    s_avail = 1.0f;
    memset(&s_tb, 0, sizeof(s_tb));
    s_tb.tokens = SM_CFG_DEFAULT.tb_capacity;
    s_evt = SM_EVT_NONE;
    s_low_ar_last_min = 0;
    s_low_ar_seen = false;
    memset(s_last_trig_min, 0, sizeof(s_last_trig_min));
    memset(s_last_trig_valid, 0, sizeof(s_last_trig_valid));
    s_habit_last_min = 0;
    s_habit_seen = false;
    s_kc = 0.0f;
}

/* KL(今日‖昨日), Dirichlet-1 平滑(0 桶不炸) */
static float kl_compute(void)
{
    float tot_c = 0.0f, tot_r = 0.0f;
    for (int i = 0; i < SM_KL_BINS; i++) {
        tot_c += s_kl_cur[i];
        tot_r += s_kl_ref[i];
    }
    if (tot_c <= 0.0f)
        return 0.0f;
    /* cur 侧不去平滑(计数即概率; 零桶不计=单样本也能表达"惊奇");
     * ref 侧 +1 平滑防零除; 昨日无样本(首日)基线=均匀 1/8 */
    float kl = 0.0f;
    for (int i = 0; i < SM_KL_BINS; i++) {
        if (s_kl_cur[i] == 0)
            continue;
        float pc = (float)s_kl_cur[i] / tot_c;
        float pr = tot_r > 0.0f
                       ? ((float)s_kl_ref[i] + 1.0f) / (tot_r + SM_KL_BINS)
                       : 1.0f / SM_KL_BINS;
        kl += pc * logf(pc / pr);
    }
    return clampf(kl, 0.0f, 1.0f);
}

void sm_event(enum sm_evt e, uint32_t now_s)
{
    s_evt = e;                      /* 本拍仲裁材料(gate 消费) */
    if (e == SM_EVT_NONE)
        return;
    switch (e) {
    case SM_EVT_WAKE:
    case SM_EVT_BARGE:
    case SM_EVT_TOUCH:
        if (!s_ku.after_on) {
            s_ku.after_on = true;
            s_ku.after = 0.0f;
            s_ku.after_t0_s = now_s;
        }
        s_ku.after += (e == SM_EVT_WAKE ? s_cfg.ku_wake
                       : e == SM_EVT_BARGE ? s_cfg.ku_barge
                                           : s_cfg.ku_touch);
        break;
    case SM_EVT_ROUND_OK:
        /* 结算非对称 EMA(docs §4.1: α_负0.30/α_正0.10, 事件值 +1/−1) */
        s_kf = clampf(s_kf + s_cfg.kf_ap * (1.0f - s_kf), 0.0f, 1.0f);
        break;
    case SM_EVT_ROUND_FAIL:
        /* 事件值 −0.5(docs §4.1 表) */
        s_kf = clampf(s_kf + s_cfg.kf_an * (-0.5f - s_kf), 0.0f, 1.0f);
        break;
    default:
        break;
    }
    /* 用户交互记账(kb 时基 + kc KL 间隔分布) */
    if (e == SM_EVT_WAKE || e == SM_EVT_TOUCH ||
        e == SM_EVT_ROUND_OK || e == SM_EVT_ROUND_FAIL) {
        s_kb_in.last_user_s = now_s;
        s_kb_in.have_user = true;
        if (s_have_interact) {
            uint32_t gap_s = now_s - s_last_interact_s;
            static const uint32_t edges_s[SM_KL_BINS - 1] = {
                120, 300, 900, 3600, 14400, 43200, 86400
            };
            int b = SM_KL_BINS - 1;
            for (int i = 0; i < SM_KL_BINS - 1; i++) {
                if (gap_s < edges_s[i]) { b = i; break; }
            }
            if (s_kl_cur[b] < 255)
                s_kl_cur[b]++;
            s_skl = kl_compute();
            if (s_skl > s_cfg.skl_gate) {
                /* 惊奇事件 → kc 充能(τ=1h 全量衰减突发项;
                 * 叠加取现值+增量, 初值重置时基) */
                s_kl_burst += s_cfg.skl_gain * s_skl;
                if (s_kl_burst > 0.5f)
                    s_kl_burst = 0.5f;
                s_kl_burst0 = s_kl_burst;
                s_kl_burst_t0 = now_s;
                /* B-6b: 反思触发面记账(一次 0.4+ 惊奇事件即过 40 门) */
                s_surprise_acc += s_skl * 100.0f;
                if (s_surprise_acc > 10000.0f)
                    s_surprise_acc = 10000.0f;
            }
        }
        s_last_interact_s = now_s;
        s_have_interact = true;
    }
}

void sm_settle(enum sm_settle s, uint32_t now_s)
{
    (void)now_s;
    switch (s) {
    case SM_SETTLE_RESPONDED:
        s_kf = clampf(s_kf + s_cfg.kf_ap * (1.0f - s_kf), 0.0f, 1.0f);
        break;
    case SM_SETTLE_SILENT:
        /* SILENT 事件值 +0.5(docs §4.1) */
        s_kf = clampf(s_kf + s_cfg.kf_ap * (0.5f - s_kf), 0.0f, 1.0f);
        break;
    case SM_SETTLE_IGNORED:
        s_kf = clampf(s_kf + s_cfg.kf_an * (-1.0f - s_kf), 0.0f, 1.0f);
        /* R2 被忽即清仓(docs §4.1: IGNORED→槽清仓 h++, s=0)——h 使
         * 再武装的老化除子增大(反复被忽→意图愈发难再触发) */
        s_ku.h++;
        if (s_ku.armed) {
            s_ku.armed = false;
            s_ku.s = 0.0f;
        }
        break;
    }
}

/* B-6b: 反思触发面(Σimp 走 soul_mem; surprise 在此记账) */
float sm_surprise_pending(void) { return s_surprise_acc; }

void sm_surprise_reset(void) { s_surprise_acc = 0.0f; }

void sm_intent_arm(float s, uint32_t now_s)
{
    if (s <= 0.0f)
        return;
    s_ku.s = clampf(s, 0.0f, 1.0f);
    s_ku.t0_s = now_s;
    s_ku.armed = true;
}

bool sm_question_open(uint8_t slot, float w, uint32_t open_t0_s)
{
    if (slot >= SM_QSLOTS || w <= 0.0f)
        return false;
    s_q[slot].open = true;
    s_q[slot].w0 = w;
    s_q[slot].t0_s = open_t0_s;
    return true;
}

void sm_question_close(uint8_t slot)
{
    if (slot < SM_QSLOTS)
        s_q[slot].open = false;
}

/* kb 五态推导(由真实事件输入; tick 与 gate 各自调用刷新缓存) */
static void kb_derive(uint32_t now_s)
{
    uint32_t silence = s_kb_in.have_user
                           ? (now_s > s_kb_in.last_user_s
                                  ? now_s - s_kb_in.last_user_s : 0)
                           : now_s;
    if (s_kb_in.quiet_hours) {
        s_kb = SM_KB_DND;
        s_avail = 0.0f;
    } else if (!s_kb_in.have_user ||
               silence > (uint32_t)s_cfg.kb_away_min * 60) {
        s_kb = SM_KB_AWAY;
        s_avail = 1.0f;
    } else if (s_kb_in.ai_active || s_kb_in.playing) {
        s_kb = SM_KB_BUSY;
        s_avail = 0.0f;
    } else if (silence < (uint32_t)s_cfg.kb_attend_min * 60) {
        s_kb = SM_KB_ATTENDED;
        s_avail = 1.0f;
    } else {
        s_kb = SM_KB_FREE;
        s_avail = 1.0f;
    }
}

/* kb net 方程(B-6a 落地, docs/24 §4.1 + §55.29 自由度裁决修订):
 * net = ku×avail − cost(FREE=1/ATTENDED=0.6/其他 0)。
 * **执法面按用户裁决收窄**: 说话中(∞ 硬门)与勿扰态才拒 speak——
 * 成本阈值不拦截(多说少说由模型自决, §55.29), net 仅作遥测观察
 * 值随 EXEC 日志留痕(冲动经济学数据积累, 供后续反思轮/校准用)。 */
bool sm_speak_allow(void)
{
    if (!s_inited)
        sm_reset();
    if (s_kb_in.ai_active)
        return false;                    /* 说话中=∞ 硬门 */
    return s_kb != SM_KB_DND;            /* 勿扰拒; 其余态模型自决 */
}

float sm_speak_net(void)
{
    if (!s_inited)
        sm_reset();
    float cost = s_kb == SM_KB_FREE ? s_cfg.kb_cost_free
                : s_kb == SM_KB_ATTENDED ? s_cfg.kb_cost_attended : 0.0f;
    return s_ku_cache * s_avail - cost;  /* 遥测观察值(不执法) */
}

void sm_tick(uint32_t now_s)
{
    if (!s_inited)
        sm_reset();
    /* kc = Σ 槽强(蒸发) + 缺席缺口(>2h 无交互, 封顶槽) + KL 突发(τ=1h) */
    float kc = 0.0f;
    for (int i = 0; i < SM_QSLOTS; i++) {
        if (!s_q[i].open)
            continue;
        float dt_min = (float)((now_s > s_q[i].t0_s ? now_s - s_q[i].t0_s
                                                    : 0) / 60);
        kc += s_q[i].w0 * expf(-dt_min / s_cfg.q_tau_min);
    }
    uint32_t silence = s_kb_in.have_user
                           ? (now_s > s_kb_in.last_user_s
                                  ? now_s - s_kb_in.last_user_s : 0)
                           : now_s;
    if (silence > 7200u)
        kc += s_cfg.absence_cap;
    if (s_kl_burst > 0.0f) {
        float dt = (float)(now_s > s_kl_burst_t0
                               ? now_s - s_kl_burst_t0 : 0);
        /* 初值×exp(−dt/τ) 全量式(旧实现每拍对现值复合衰减=二次坍缩,
         * S5 审计 P1-1) */
        s_kl_burst = s_kl_burst0 * expf(-dt / 3600.0f);
        kc += s_kl_burst;
    }
    s_kc = clampf(kc, 0.0f, 1.0f);

    /* ku = 遗留槽老化 ×1.02^Δmin/(1+h) + 余辉(τ=600s 指数)。
     * 寿命 48h 上限(1.02^Δmin 在 ~75h float 溢出且 R3 自消退精神
     * 不允许意图永不过期, 审计 P2-8) */
    float ku = 0.0f;
    if (s_ku.armed) {
        float dt_min = (float)((now_s > s_ku.t0_s ? now_s - s_ku.t0_s
                                                  : 0) / 60);
        if (dt_min > 2880.0f) {
            s_ku.armed = false;        /* 48h 自消退 */
            s_ku.s = 0.0f;
        } else {
            ku += s_ku.s * powf(s_cfg.ku_grow, dt_min) / (1.0f + s_ku.h);
        }
    }
    if (s_ku.after_on) {
        float dt = (float)(now_s > s_ku.after_t0_s
                               ? now_s - s_ku.after_t0_s : 0);
        ku += s_ku.after * expf(-dt / s_cfg.ku_after_tau_s);
    }
    s_ku_cache = clampf(ku, 0.0f, s_cfg.ku_cap);

    kb_derive(now_s);
}

void sm_snapshot(struct sm_state *out)
{
    if (out == NULL)
        return;
    out->kc = s_kc;
    out->ku = s_ku_cache;
    out->kf = s_kf;
    out->kb = s_kb;
    out->avail = s_avail;
    out->e_frustrate = s_kf < s_cfg.kf_frustrate
                           ? s_cfg.kf_frustrate - s_kf
                           : 0.0f;
    out->s_kl = s_skl;
    out->tokens = (uint8_t)clampf(s_tb.tokens, 0.0f, 255.0f);
}

/* ---- 仲裁器 + 令牌桶(docs/24 §4.2) ----
 * 事件突发度表(U): 唤醒1.0/打断0.8/轮失败0.6/播放切换0.5/触摸0.35/
 * 网络0.35/长静默0.30/kd跃迁0.5(§4.2 表) */
static float burst_u(enum sm_evt e)
{
    switch (e) {
    case SM_EVT_WAKE:       return 1.0f;
    case SM_EVT_BARGE:      return 0.8f;
    case SM_EVT_PLAY_CHG:   return 0.5f;
    case SM_EVT_TOUCH:      return 0.35f;
    case SM_EVT_NET:        return 0.35f;
    case SM_EVT_ROUND_FAIL:     return 0.6f;
    case SM_EVT_LONG_SILENCE:   return 0.30f;
    default:                    return 0.0f;   /* ROUND_OK/静默窗不进 U 表 */
    }
}

void sm_ctx(bool quiet_hours, bool ai_active, bool playing)
{
    s_kb_in.quiet_hours = quiet_hours;
    s_kb_in.ai_active = ai_active;
    s_kb_in.playing = playing;
}

void sm_gate(const struct sm_gate_in *in, struct sm_gate_out *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_inited)
        sm_reset();
    /* 令牌桶: 日翻转重置债/习惯化 + KL 基线冻结; 滴灌日补
     * (12/1440 per min 至容量帽, docs/24 §4.2 "日补 12") */
    uint32_t d = in->now_min / 1440u;
    if (!s_tb.day_seen) {
        s_tb.day = d;
        s_tb.day_seen = true;
    } else if (d != s_tb.day) {
        s_tb.day = d;
        s_tb.accrual_seen = false;
        s_tb.habit = 0.0f;
        memcpy(s_kl_ref, s_kl_cur, sizeof(s_kl_ref));
        memset(s_kl_cur, 0, sizeof(s_kl_cur));
    }
    if (!s_tb.accrual_seen) {
        s_tb.last_accrual_min = in->now_min;
        s_tb.accrual_seen = true;
    } else {
        uint32_t dm = in->now_min >= s_tb.last_accrual_min
                          ? in->now_min - s_tb.last_accrual_min : 0;
        if (dm > 0) {
            s_tb.tokens += (float)dm * (float)s_cfg.tb_daily / 1440.0f;
            if (s_tb.tokens > (float)s_cfg.tb_capacity)
                s_tb.tokens = (float)s_cfg.tb_capacity;
            s_tb.last_accrual_min = in->now_min;
        }
    }
    /* 习惯化自发恢复 ρ/min(按真实分钟间隔, 调用粒度无关) */
    if (!s_habit_seen) {
        s_habit_last_min = in->now_min;
        s_habit_seen = true;
    } else {
        uint32_t dm = in->now_min >= s_habit_last_min
                          ? in->now_min - s_habit_last_min : 0;
        s_tb.habit -= s_cfg.habit_recover * (float)dm;
        if (s_tb.habit < 0.0f)
            s_tb.habit = 0.0f;
        s_habit_last_min = in->now_min;
    }

    /* ---- 前置硬门(quiet/单飞) ---- */
    if (in->quiet_hours) {
        out->blocked_mask |= 1 << 0;
        goto done;
    }
    if (in->thinking) {
        out->blocked_mask |= 1 << 5;
        goto done;
    }

    /* ---- 触发通道(L7 修复内建) ---- */
    /* 通道 0: 仲裁 S=U×M×H。全部 U 表事件参与, S>0.5 门裁决
     * (docs §4.2 "new_event 只收 S>0.5 打分事件"; S5 初版的 U≥0.5
     * 参与门使低突发事件永不可触发=死参数, 审计 P1-5 修正) */
    float u = (s_evt != SM_EVT_NONE) ? burst_u(s_evt) : 0.0f;
    if (u > 0.0f) {
        float m = 1.0f;
        if (in->ai_active)
            m *= 1.0f + 0.5f * clampf(in->e_social, 0.0f, 1.0f);
        if (in->playing)
            m *= 1.0f + 0.5f * clampf(in->e_stim, 0.0f, 1.0f);
        if (in->net_up)
            m *= 1.0f + 0.3f * s_kc;
        float h = 1.0f / (1.0f + s_tb.habit);
        out->S = u * m * h;
        if (out->S > s_cfg.s_gate)
            out->reason_mask |= 1 << 0;
    }
    /* 通道 1: kc(门 ∧ s_kl 门) */
    if (s_kc > s_cfg.kc_gate && s_skl > s_cfg.skl_gate)
        out->reason_mask |= 1 << 1;
    /* 通道 2: ku(阈 ∧ 静默窗满; 孤立 idle 不触发) */
    {
        uint32_t silence = s_kb_in.have_user
                               ? (in->now_s > s_kb_in.last_user_s
                                      ? in->now_s - s_kb_in.last_user_s
                                      : 0)
                               : in->now_s;
        if (s_ku_cache > s_cfg.ku_thresh &&
            silence >= s_cfg.ku_silence_s)
            out->reason_mask |= 1 << 2;
    }
    /* 通道 3: kd sev 跃迁沿 */
    if (in->kd_sev_jump)
        out->reason_mask |= 1 << 3;
    /* 通道 4: mem_hook(用户锚定, B-6 前恒 false) */
    if (in->mem_hook)
        out->reason_mask |= 1 << 4;
    if (out->reason_mask == 0) {
        out->blocked_mask |= 1 << 6;
        goto done;
    }

    /* ---- kb 允许面: DND/BUSY 拒 + salient 成功率门 p=kf×avail ---- */
    if (s_kb == SM_KB_DND || s_kb == SM_KB_BUSY) {
        out->blocked_mask |= 1 << 2;
        out->reason_mask = 0;
        goto done;
    }
    if (s_kf * s_avail < s_cfg.kb_p_gate) {
        out->blocked_mask |= 1 << 2;
        out->reason_mask = 0;
        goto done;
    }

    /* ---- 低唤醒降采样(L7: arousal<0.1 检查 1/5min) ---- */
    if (in->arousal < 0.1f) {
        if (s_low_ar_seen && in->now_min >= s_low_ar_last_min &&
            in->now_min - s_low_ar_last_min < 5) {
            out->blocked_mask |= 1 << 3;
            goto done;
        }
        s_low_ar_last_min = in->now_min;
        s_low_ar_seen = true;
    }
    /* ---- 同 key 去重(5min; key=触发通道) ---- */
    for (int b = 0; b < 5; b++) {
        if ((out->reason_mask & (1 << b)) && s_last_trig_valid[b] &&
            in->now_min >= s_last_trig_min[b] &&
            in->now_min - s_last_trig_min[b] < s_cfg.dedup_min) {
            out->reason_mask &= (uint8_t)~(1 << b);   /* 该 key 冷却 */
        }
    }
    if (out->reason_mask == 0) {
        out->blocked_mask |= 1 << 4;
        goto done;
    }

    /* ---- 令牌: ≥1 直接用; 无令牌仅 S≥透支门可透支到 −1 ---- */
    if (s_tb.tokens < 1.0f &&
        !(out->S >= s_cfg.s_overdraft)) {
        out->blocked_mask |= 1 << 1;
        goto done;
    }
    s_tb.tokens -= 1.0f;
    if (s_tb.tokens < -1.0f)
        s_tb.tokens = -1.0f;
    s_tb.habit += 1.0f;
    for (int b = 0; b < 5; b++) {
        if (out->reason_mask & (1 << b)) {
            s_last_trig_min[b] = in->now_min;
            s_last_trig_valid[b] = true;
        }
    }
    out->trigger = true;

done:
    s_evt = SM_EVT_NONE;    /* 事件材料单拍消费 */
}
