/*
 * soul_sim.c —— 情绪/快慢思考框架 PC 快 tick 模拟器(2026-11-19)
 *
 * 复用固件真源(零拷贝数值核): soul_bb/budget/drive(kd)/emotion(ke)/
 * ignition(点火闸) 五件套 —— 全纯 C 标准库(可解释可计算的实证)。
 * 本壳复刻 soul.c 1Hz tick 序(感知→事件→kd→mood→ke→bb→深思闸→预算
 * 行动占位→底色最近邻; S3 起 L2 行为表退役, docs/24 §7), 快进 N tick
 * 等价 N 秒墙钟, 纯计算毫秒级。
 *
 * 事件脚本(每行): <秒> <cmd> [arg]
 *   net 0|1 / play 0|1 / ai 0|1 / voice / touch / barge
 *   round ok|fail <用户语截断> / mood +0.15 / timesync 0|1
 *   dose <soc> <eth> <val> / trait C W E A R / reflect dC dW dE dA dR [conf evid depth]
 *   (S2.5b: trait=建角色注入, reflect=深思轮 trait_delta 注入)
 * 默认(无脚本): 空场景纯代谢(无人陪跑一天=系统自然演化)。
 *
 * 输出: stdout 摘要; --csv FILE 轨迹(1Hz 全量或 --stride N 抽稀)。
 * 模拟边界(与板上差异, 如实声明):
 *   ① 深思内容=LLM 占位(触发/配额/冷静期/新意门全真, 回复语义假)
 *   ② 感知由脚本注入(板上来自真实事件流)
 *   ③ 勿扰钟=模拟墙钟(t/3600), timesync 事件后生效(板上 SNTP 口径)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "soul_bb.h"
#include "soul_budget.h"
#include "soul_motiv.h"
#include "soul_drive.h"
#include "soul_emotion.h"
#include "soul_face.h"
#include "soul_ignition.h"
#include "soul_trait.h"

/* ---- 模拟世界状态(感知侧) ---- */
static bool w_net, w_play, w_ai, w_timesync;
static uint8_t w_pend;              /* PEND 位(与 soul.c 同语义) */
enum { P_BARGE = 1, P_ROUND_OK = 2, P_ROUND_FAIL = 4, P_VOICE = 8, P_TOUCH = 16,
       P_DOSE = 32 };   /* S2.5 双臂: appraisal 三元组剂量(LLM 评价臂) */
static float w_dose[3];           /* social / ethics / valence */
static float w_mood_req; static uint32_t w_mood_seq;
static uint32_t s_sm_t;   /* 当前拍(inject 时基; 主循环每拍刷新) */
static uint32_t w_dose_seq;

/* ---- 深思记账(复刻 soul_think.c 静态) ---- */
static uint32_t s_last_think_min;
static uint8_t  s_thought_today;
static bool     s_last_silent;
static float    s_last_silent_score;
static uint32_t s_think_fires, s_think_blocked[4], s_think_reason[6];

/* ---- 底色(S2: soul_face 同源模块——扇区三件套单一真源,
 *      与 soul.c 完全同代码同参数; 旧双份 FMAP 表废弃) ---- */
static char cur_face[24] = "leisure";
static uint32_t face_dwell[SOUL_FACE_N];

/* ---- 深思行动占位(S3: 行为面改深思模拟, docs/24 §7; 旧 L2 行为表
 * 随 R6 退役 —— 行动只来自深思, sim 以最强驱力缺口选类, 预算硬门
 * 同款执法; 真实行动语义=板上 LLM, 占位仅保预算/结算通路有真值) ---- */
static uint32_t s_act_fires[SB_CNT];
static uint32_t s_budget_blocks;

static int act_pick(const struct kd_output *kd)
{
    if (kd->error[KD_SOCIAL] >= kd->error[KD_STIM])
        return kd->error[KD_SOCIAL] > 0.10f ? SB_SPEAK : SB_FACE;
    return kd->error[KD_STIM] > 0.10f ? SB_MUSIC : SB_FACE;
}

static void face_bg_select(const struct ke_state *ke, uint32_t t, uint32_t idle_s)
{
    const char *clip = soul_face_bg(ke->valence, ke->arousal, ke->mood,
                                    t, idle_s, ke->ethics_latched);
    if (strcmp(cur_face, clip) != 0) {
        snprintf(cur_face, sizeof(cur_face), "%s", clip);
        printf("[%6ds] face bg -> %s (v=%.2f a=%.2f m=%.2f)\n",
               (int)t, cur_face, ke->valence, ke->arousal, ke->mood);
    }
    for (int i = 0; i < SOUL_FACE_N; i++)
        if (strcmp(cur_face, soul_face_map[i].clip) == 0) {
            face_dwell[i]++; break;
        }
}

/* ---- 事件脚本 ---- */
typedef struct { uint32_t t; char line[96]; } ev_t;
static ev_t evs[4096]; static int ev_n;
static int ev_cmp(const void *x, const void *y)
{ return (int)((const ev_t *)x)->t - (int)((const ev_t *)y)->t; }

static void inject(const char *ln)
{
    char cmd[24], arg[48];
    int n = sscanf(ln, "%23s %47s", cmd, arg);
    (void)n;
    /* 显著性充值复刻 soul.c perception_consume 真源口径:
     * net 边沿 0.6 / timesync 0.5 / play 跃迁 0.3 / ai 变化 0.3;
     * voice/barge/round 按 ai 翻转近似 0.3(壳内无会话生命周期) */
    if (!strcmp(cmd, "net")) {
        int v = atoi(arg);
        if (v != (int)w_net) { soul_bb_salience(0.6f); sm_event(SM_EVT_NET, s_sm_t); }
        w_net = v;
        soul_bb_set(SOUL_BB_NET, w_net ? 1 : 0);
    } else if (!strcmp(cmd, "play")) {
        int v = atoi(arg);
        soul_bb_salience(0.3f);
        sm_event(SM_EVT_PLAY_CHG, s_sm_t);
        w_play = v; soul_bb_set(SOUL_BB_PLAYING, w_play);
    } else if (!strcmp(cmd, "ai")) {
        int v = atoi(arg);
        if (v != (int)w_ai) soul_bb_salience(0.3f);
        w_ai = v;
    } else if (!strcmp(cmd, "timesync")) { w_timesync = atoi(arg); soul_bb_salience(0.5f); }
    else if (!strcmp(cmd, "voice")) { w_pend |= P_VOICE; soul_bb_salience(0.3f); }
    else if (!strcmp(cmd, "barge")) { w_pend |= P_BARGE; soul_bb_salience(0.3f); }
    else if (!strcmp(cmd, "touch")) {
        /* S2: touch 接 kd 事件(对齐 soul.c; 旧壳只写黑板不进 kd=保真缺口) */
        w_pend |= P_TOUCH; soul_bb_salience(0.3f); soul_bb_set(SOUL_BB_TOUCH, 1.0f);
    }
    else if (!strcmp(cmd, "round")) {
        soul_bb_salience(0.3f);
        if (!strcmp(arg, "ok")) w_pend |= P_ROUND_OK;
        else w_pend |= P_ROUND_FAIL;
    } else if (!strcmp(cmd, "mood")) {
        w_mood_req = strtof(arg, NULL); w_mood_seq |= 1;
    } else if (!strcmp(cmd, "dose")) {
        /* dose <social> <ethics> <valence>: LLM appraisal 剂量直注生理层 */
        if (sscanf(ln, "%*s %f %f %f", &w_dose[0], &w_dose[1], &w_dose[2]) == 3)
            w_pend |= P_DOSE;
    } else if (!strcmp(cmd, "trait")) {
        /* S2.5: trait C E W A R(五维, band 内)——双性格对照实验入口 */
        float t[5];
        if (sscanf(ln, "%*s %f %f %f %f %f",
                   &t[0], &t[1], &t[2], &t[3], &t[4]) == 5) {
            printf("trait set %s (C=%.2f W=%.2f E=%.2f A=%.2f R=%.2f)\n",
                   trait_set(t) ? "ok" : "OUT_OF_BAND",
                   t[0], t[1], t[2], t[3], t[4]);
        }
    } else if (!strcmp(cmd, "reflect")) {
        /* S2.5b 通道二: 反思跃迁注入(LLM 深思轮 trait_delta 的替身)。
         * 行格式: <t> reflect <dC> <dW> <dE> <dA> <dR> [conf [evid [depth]]]
         * 缺省 conf=0.9 evid=2 depth=2(过门; 调低即测拒绝门) */
        struct trait_reflect rf;
        memset(&rf, 0, sizeof(rf));
        float conf = 0.9f; int evid = 2, depth = 2;
        int k = sscanf(ln, "%*s %f %f %f %f %f %f %d %d",
                       &rf.delta[0], &rf.delta[1], &rf.delta[2],
                       &rf.delta[3], &rf.delta[4], &conf, &evid, &depth);
        if (k >= 5) {
            rf.conf = conf; rf.evidence_n = (uint8_t)evid;
            rf.ref_depth = (uint8_t)depth;
            bool ok = trait_reflect_apply(&rf);
            printf("reflect %s (dC=%+.2f dW=%+.2f dE=%+.2f dA=%+.2f"
                   " dR=%+.2f)\n", ok ? "applied" : "REJECTED",
                   rf.delta[0], rf.delta[1], rf.delta[2], rf.delta[3],
                   rf.delta[4]);
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t ticks = 86400;                 /* 默认模拟一天 */
    const char *csv = NULL, *scenario = NULL;
    uint32_t stride = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tune-only") && i + 1 < argc) {
            /* genesis 面试协议出口(docs/24 §11.6 参数层): 五维 → 全调制
             * 系数 JSON(真源=soul_trait tune_recalc, 宿主工具零复刻) */
            float t[TRAIT_DIM];
            if (sscanf(argv[++i], "%f %f %f %f %f",
                       &t[0], &t[1], &t[2], &t[3], &t[4]) != TRAIT_DIM) {
                fprintf(stderr, "usage: --tune-only \"C W E A R\"\n");
                return 2;
            }
            trait_reset();
            if (!trait_set(t)) {
                fprintf(stderr, "OUT_OF_BAND\n");
                return 3;
            }
            const trait_tune_t *u = trait_tune();
            printf("{\n"
                   "  \"trait\": [%.3f, %.3f, %.3f, %.3f, %.3f],\n"
                   "  \"frag\": %.3f,\n"
                   "  \"ke\": {\"arousal_base\": %.3f, \"valence_off\": %.3f, "
                   "\"tau_dec_mod\": %.3f, \"bond_up_mod\": %.3f, "
                   "\"urg_on\": %.3f, \"novelty_gain_mod\": %.3f},\n"
                   "  \"ethics\": {\"gate\": %.3f, \"dose_mod\": %.3f, "
                   "\"load_t\": %.3f, \"heal_tau_mod\": %.3f, "
                   "\"soothe_mod\": %.3f},\n"
                   "  \"kd\": {\"social_met_mod\": %.3f, "
                   "\"stim_self_recover\": %.1e}\n"
                   "}\n",
                   t[0], t[1], t[2], t[3], t[4], u->frag,
                   u->arousal_base, u->valence_off, u->tau_dec_mod,
                   u->bond_up_mod, u->urg_on, u->novelty_gain_mod,
                   u->eth_gate, u->eth_dose_mod, u->eth_load_t,
                   u->eth_heal_tau_mod, u->eth_soothe_mod,
                   u->social_met_mod, u->stim_self_recover);
            return 0;
        }
    }
    if (scenario) {
        FILE *f = fopen(scenario, "r");
        if (!f) { perror(scenario); return 1; }
        char ln[96];
        while (fgets(ln, sizeof ln, f) && ev_n < 4096) {
            if (ln[0] == '#' || ln[0] == '\n') continue;
            evs[ev_n].t = strtoul(ln, NULL, 10);
            snprintf(evs[ev_n].line, sizeof(evs[0].line), "%s", ln);
            ev_n++;
        }
        fclose(f);
        qsort(evs, ev_n, sizeof(evs[0]), ev_cmp);
    }
    FILE *cf = NULL;
    if (csv) { cf = fopen(csv, "w"); fprintf(cf, "t,v,a,mood,urgency,bond,persist,"
        "m_social,m_stim,salience,idle,quiet,net,play,face,think_fire,think_block,ethics,novelty\n"); }

    sb_reset();
    soul_face_bg_reset();
    trait_reset();      /* S2.5: 每局回中性(场景内 trait 行可覆写) */
    kd_reset(); ke_reset(NULL);
    w_net = true; soul_bb_set(SOUL_BB_NET, 1);
    uint32_t last_active = 0;
    float v_min = 1e9f, v_max = -1e9f, a_max = -1e9f;
    uint32_t quiet_secs = 0;

    struct kd_output kd_out;
    struct ke_output ke_out;

    for (uint64_t t = 1; t <= ticks; t++) {
        s_sm_t = (uint32_t)t;
        while (ev_n > 0 && evs[0].t == t) { inject(evs[0].line + strspn(evs[0].line, "0123456789 ")); memmove(&evs[0], &evs[1], (size_t)(--ev_n) * sizeof(evs[0])); }

        /* 事件消费(BARGE>ROUND>VOICE>TOUCH; 复刻 soul.c 优先级) */
        enum kd_event evt = KD_EVT_NONE;
        int receipt = KE_RECEIPT_NONE;
        if (w_pend & P_BARGE) { w_pend &= ~P_BARGE; evt = KD_EVT_BARGE; sm_event(SM_EVT_BARGE, (uint32_t)t); }
        else if (w_pend & P_ROUND_OK) { w_pend &= ~P_ROUND_OK; evt = KD_EVT_ROUND_OK; receipt = KE_RECEIPT_SUCCESS; sm_event(SM_EVT_ROUND_OK, (uint32_t)t); }
        else if (w_pend & P_ROUND_FAIL) { w_pend &= ~P_ROUND_FAIL; evt = KD_EVT_ROUND_FAIL; receipt = KE_RECEIPT_FAIL; sm_event(SM_EVT_ROUND_FAIL, (uint32_t)t); }
        else if (w_pend & P_VOICE) { w_pend &= ~P_VOICE; evt = KD_EVT_VOICE; }
        else if (w_pend & P_TOUCH) { w_pend &= ~P_TOUCH; evt = KD_EVT_TOUCH; sm_event(SM_EVT_TOUCH, (uint32_t)t); }
        else if (w_pend & P_DOSE) {
            w_pend &= ~P_DOSE;
            /* 滋养性剂量(gc>0)等价温和互动=安抚语义(对齐 round ok/touch) */
            if (w_dose[0] > 0.0f) receipt = KE_RECEIPT_SUCCESS;
            kd_inject_social(w_dose[0]);
            ke_ethics_dose(w_dose[1]);
            if (w_dose[2] != 0.0f) {
                struct ke_reappraisal r = { .delta = w_dose[2],
                                            .receipt_id = 0xBEEF + (w_dose_seq++) };
                ke_reappraisal_request(&r);
            }
        }

        bool quiet_world = !w_ai && !w_play;        /* kd quiet 口径 */
        if (w_ai) last_active = (uint32_t)t;
        struct kd_inputs din = { .tick = t, .quiet = quiet_world, .charging = false,
                                 .stimulating = (w_play || w_ai), .event = evt };
        kd_step(t, &din, &kd_out);

        if ((w_mood_seq & 1) && w_mood_req != 0) {
            struct ke_reappraisal r = { .delta = w_mood_req, .receipt_id = w_mood_seq };
            ke_reappraisal_request(&r);
            printf("[%6llus] mood reappraisal %+.2f\n", (unsigned long long)t, w_mood_req);
            w_mood_seq++;
        }
        struct ke_inputs kin = {
            .have_drive = true,
            .drive_error_sum = kd_out.error_sum,
            .drive_error_max = kd_out.error_max,
            .drive_sat = {
                [KD_SOCIAL] = kd_out.level[KD_SOCIAL],
                [KD_STIM] = kd_out.level[KD_STIM],
                [KD_REST] = kd_out.level[KD_REST],
            },
            .receipt = receipt,
            .social_event = evt != KD_EVT_NONE,
            .soothe_event = (evt == KD_EVT_TOUCH ||
                             receipt == KE_RECEIPT_SUCCESS),
        };
        ke_step(t, &kin, &ke_out);
        const struct ke_state *ke = ke_state_snapshot();

        /* bb 供给(复刻 bb_feed) */
        soul_bb_set(SOUL_BB_VALENCE, ke->valence);
        soul_bb_set(SOUL_BB_AROUSAL, ke->arousal);
        soul_bb_set(SOUL_BB_URGENCY, ke->urgency);
        soul_bb_set(SOUL_BB_BOND, ke->social_bond);
        soul_bb_set(SOUL_BB_ETHICS, ke->ethics_latched ? 1 : 0);
        soul_bb_set(SOUL_BB_M_SOCIAL, kd_out.error[KD_SOCIAL]);
        soul_bb_set(SOUL_BB_M_STIM, kd_out.error[KD_STIM]);
        soul_bb_set(SOUL_BB_AI_ACTIVE, w_ai ? 1 : 0);
        float idle = (float)((uint32_t)t - last_active) / 7200.0f;
        soul_bb_set(SOUL_BB_IDLE, idle > 1 ? 1 : idle);
        bool quiet_hrs = false;
        if (w_timesync) { uint32_t h = (uint32_t)((t / 3600) % 24); quiet_hrs = h >= 23 || h < 8; }
        soul_bb_set(SOUL_BB_QUIET, quiet_hrs ? 1 : 0);
        if (quiet_hrs) quiet_secs++;
        soul_bb_tick((uint32_t)t);

        /* L4 思维闸(S5 重构): sm_ctx/event/tick + sm_gate 仲裁(令牌桶
         * +kb 礼貌闸; docs/24 §4.2), 与 soul.c 同款接线 */
        int fired = 0, blocked = 0;
        uint32_t now_min = (uint32_t)((t / 60) % 1440);
        static uint8_t s_prev_sev;
        {
            uint8_t sev = (uint8_t)((kd_out.sev[KD_SOCIAL] << 4) |
                                    (kd_out.sev[KD_STIM] << 2) |
                                    kd_out.sev[KD_REST]);
            bool sev_jump = s_prev_sev != sev;
            s_prev_sev = sev;
            sm_ctx(quiet_hrs, w_ai, w_play);
            sm_tick((uint32_t)t);
            struct sm_gate_in gin = {
                .now_s = (uint32_t)t, .now_min = (uint32_t)(t / 60),
                .arousal = ke->arousal,
                .e_social = kd_out.error[KD_SOCIAL],
                .e_stim = kd_out.error[KD_STIM],
                .ai_active = w_ai, .playing = w_play,
                .net_up = w_net, .quiet_hours = quiet_hrs,
                .kd_sev_jump = sev_jump, .mem_hook = false,
                .thinking = false,
            };
            struct sm_gate_out g;
            sm_gate(&gin, &g);
            /* 拦截观测(bit0 quiet/1 无令牌/2 kb/3 降采样/4 去重/5 单飞) */
            for (int b = 0; b < 7; b++)
                if (g.blocked_mask & (1 << b)) {
                    s_think_blocked[b < 4 ? b : 3]++;
                    blocked = 1;
                }
            if (g.trigger) {
                fired = 1;
                s_think_fires++;
                for (int b = 0; b < 5; b++)
                    if (g.reason_mask & (1 << b)) s_think_reason[b]++;
                s_last_think_min = now_min;
                s_thought_today++;
                s_last_silent = true;         /* 模拟: LLM 结算多为沉默 */
                s_last_silent_score = 0.5f;
                /* 行动面占位(S3): 最强驱力缺口选类 → 预算硬门执法 */
                int cat = act_pick(&kd_out);
                if (sb_allow(cat, (uint32_t)t)) {
                    sb_charge(cat, (uint32_t)t);
                    s_act_fires[cat]++;
                } else {
                    s_budget_blocks++;
                }
            }
        }

        {
            uint32_t idle_s = (uint32_t)t > last_active
                ? (uint32_t)t - last_active : 0;
            if (idle_s > 3600) idle_s = 3600;
            face_bg_select(ke, (uint32_t)t, idle_s);
        }

        /* S2.5b trait 双通道: 每拍残差观测(自发发起=深思行动成立),
         * 日界结算 EMA+互斥+熔断(与 soul.c 同语义同参数) */
        struct trait_obs tobs = {
            .arousal = ke->arousal,
            .salience = soul_bb_get(SOUL_BB_SALIENCE),
            .urgency = ke->urgency,
            .social_bond = ke->social_bond,
            .valence = ke->valence,
            .initiative = fired == 1,
        };
        trait_stat_observe(&tobs);
        if (t % 86400 == 0) {
            float tr_after[TRAIT_DIM];
            trait_ema_settle();
            trait_get(tr_after);
            for (int d = 0; d < TRAIT_DIM; d++)
                if (trait_last_ema(d) != 0.0f || trait_frozen(d)) {
                    printf("[%6llus] trait day %u: C=%.3f W=%.3f E=%.3f"
                           " A=%.3f R=%.3f%s\n", (unsigned long long)t,
                           trait_day_count(), tr_after[0], tr_after[1],
                           tr_after[2], tr_after[3], tr_after[4],
                           trait_frozen(d) ? " [FROZEN]" : "");
                    break;
                }
        }
        if (ke->valence < v_min) v_min = ke->valence;
        if (ke->valence > v_max) v_max = ke->valence;
        if (ke->arousal > a_max) a_max = ke->arousal;

        if (cf && (t % stride == 0))
            fprintf(cf, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%d,%d,%d,%s,%d,%d,%d,%.3f\n",
                (unsigned long long)t, ke->valence, ke->arousal, ke->mood,
                ke->urgency, ke->social_bond, ke->persistence,
                kd_out.error[KD_SOCIAL], kd_out.error[KD_STIM],
                soul_bb_get(SOUL_BB_SALIENCE), soul_bb_get(SOUL_BB_IDLE),
                quiet_hrs, w_net, w_play, cur_face, fired, blocked,
                ke->ethics_latched ? 1 : 0, ke->novelty);
    }

    printf("\n===== SOUL SIM 摘要(%.2f 天) =====\n", ticks / 86400.0);
    printf("深思: 触发 %u 次 | 被拦: quiet=%u 无令牌=%u kb=%u 其他=%u\n",
        s_think_fires, s_think_blocked[0], s_think_blocked[1],
        s_think_blocked[2], s_think_blocked[3]);
    printf("深思原因位: 仲裁S=%u kc=%u ku=%u kd跃迁=%u mem=%u\n",
        s_think_reason[0], s_think_reason[1], s_think_reason[2],
        s_think_reason[3], s_think_reason[4]);
    printf("情绪: valence [%.3f, %.3f] | arousal max %.3f\n", v_min, v_max, a_max);
    printf("深思行动(预算分账): speak=%u music=%u face=%u 预算拦=%u\n",
        s_act_fires[0], s_act_fires[1], s_act_fires[2], s_budget_blocks);
    printf("勿扰时段占比: %.1f%% | 底色终态: %s\n",
        100.0f * quiet_secs / ticks, cur_face);
    {
        float tr[TRAIT_DIM];
        trait_get(tr);
        printf("trait 终值(day %u): C=%.3f W=%.3f E=%.3f A=%.3f R=%.3f\n",
               trait_day_count(), tr[0], tr[1], tr[2], tr[3], tr[4]);
    }
    printf("底色驻留 top5: ");
    for (int pass = 0; pass < 5; pass++) {
        int bi = -1; uint32_t bv = 0;
        for (int i = 0; i < SOUL_FACE_N; i++) if (face_dwell[i] > bv) { bv = face_dwell[i]; bi = i; }
        if (bi < 0) break;
        printf("%s=%us(%.0f%%) ", soul_face_map[bi].clip, bv, 100.0f * bv / ticks);
        face_dwell[bi] = 0;
    }
    printf("\n");
    if (cf) { fclose(cf); printf("轨迹已写 %s\n", csv); }
    return 0;
}
