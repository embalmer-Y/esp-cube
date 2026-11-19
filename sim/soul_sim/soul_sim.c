/*
 * soul_sim.c —— 情绪/快慢思考框架 PC 快 tick 模拟器(2026-11-19)
 *
 * 复用固件真源(零拷贝数值核): soul_bb/behavior/drive(kd)/emotion(ke)/
 * ignition(点火闸) 五件套 —— 全纯 C 标准库(可解释可计算的实证)。
 * 本壳复刻 soul.c 1Hz tick 序(感知→事件→kd→mood→ke→bb→behavior→L4 闸
 * →底色最近邻), 快进 N tick 等价 N 秒墙钟, 纯计算毫秒级。
 *
 * 事件脚本(每行): <秒> <cmd> [arg]
 *   net 0|1 / play 0|1 / ai 0|1 / voice / touch / barge
 *   round ok|fail <用户语截断> / mood +0.15 / timesync 0|1
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
#include "soul_behavior.h"
#include "soul_drive.h"
#include "soul_emotion.h"
#include "soul_face.h"
#include "soul_ignition.h"

/* ---- 模拟世界状态(感知侧) ---- */
static bool w_net, w_play, w_ai, w_timesync;
static uint8_t w_pend;              /* PEND 位(与 soul.c 同语义) */
enum { P_BARGE = 1, P_ROUND_OK = 2, P_ROUND_FAIL = 4, P_VOICE = 8, P_TOUCH = 16 };
static float w_mood_req; static uint32_t w_mood_seq;

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

/* ---- 行为打桩(条件表复刻 soul.c, exec=记录) ---- */
static uint32_t bhv_fires[8];
static void on_radio_start(void) { bhv_fires[0]++; soul_bb_set(SOUL_BB_PLAYING, 1); w_play = true; }
static void on_radio_stop(void)  { bhv_fires[1]++; soul_bb_set(SOUL_BB_PLAYING, 0); w_play = false; }
static void on_self_face(void)   { bhv_fires[2]++; }

static void behaviors_register_sim(void)
{
    static const soul_cons_t c_start[] = {
        {SOUL_BB_M_STIM, SOUL_C_RISE, 1.0f}, {SOUL_BB_PLAYING, SOUL_C_FALL, 1.0f},
        {SOUL_BB_QUIET, SOUL_C_FALL, 1.0f}, {SOUL_BB_AI_ACTIVE, SOUL_C_FALL, 1.0f},
    };
    static const soul_cons_t c_stop[] = {
        {SOUL_BB_QUIET, SOUL_C_RISE, 1.0f}, {SOUL_BB_PLAYING, SOUL_C_RISE, 1.0f},
    };
    static const soul_cons_t c_face[] = {
        {SOUL_BB_IDLE, SOUL_C_RISE, 1.2f}, {SOUL_BB_M_SOCIAL, SOUL_C_RISE, 1.0f},
        {SOUL_BB_AI_ACTIVE, SOUL_C_FALL, 1.0f},
    };
    soul_behavior_register(&(soul_bhv_t){ .name = "radio_start", .cons = c_start,
        .cons_n = 4, .threshold = 0.35f, .w = 1.0f, .cooldown_s = 1800, .exec = on_radio_start });
    soul_behavior_register(&(soul_bhv_t){ .name = "radio_stop", .cons = c_stop,
        .cons_n = 2, .threshold = 0.80f, .w = 1.0f, .cooldown_s = 3600, .exec = on_radio_stop });
    soul_behavior_register(&(soul_bhv_t){ .name = "self_face", .cons = c_face,
        .cons_n = 3, .threshold = 0.50f, .w = 1.0f, .cooldown_s = 480, .exec = on_self_face });
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
        if (v != (int)w_net) soul_bb_salience(0.6f);
        w_net = v;
        soul_bb_set(SOUL_BB_NET, w_net ? 1 : 0);
    } else if (!strcmp(cmd, "play")) {
        int v = atoi(arg);
        soul_bb_salience(0.3f);
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
        "m_social,m_stim,salience,idle,quiet,net,play,face,think_fire,think_block,ethics\n"); }

    soul_behavior_reset();
    soul_face_bg_reset();
    behaviors_register_sim();
    kd_reset(); ke_reset(NULL);
    w_net = true; soul_bb_set(SOUL_BB_NET, 1);
    uint32_t last_active = 0;
    float v_min = 1e9f, v_max = -1e9f, a_max = -1e9f;
    uint32_t quiet_secs = 0;

    struct kd_output kd_out;
    struct ke_output ke_out;

    for (uint64_t t = 1; t <= ticks; t++) {
        while (ev_n > 0 && evs[0].t == t) { inject(evs[0].line + strspn(evs[0].line, "0123456789 ")); memmove(&evs[0], &evs[1], (size_t)(--ev_n) * sizeof(evs[0])); }

        /* 事件消费(BARGE>ROUND>VOICE>TOUCH; 复刻 soul.c 优先级) */
        enum kd_event evt = KD_EVT_NONE;
        int receipt = KE_RECEIPT_NONE;
        if (w_pend & P_BARGE) { w_pend &= ~P_BARGE; evt = KD_EVT_BARGE; }
        else if (w_pend & P_ROUND_OK) { w_pend &= ~P_ROUND_OK; evt = KD_EVT_ROUND_OK; receipt = KE_RECEIPT_SUCCESS; }
        else if (w_pend & P_ROUND_FAIL) { w_pend &= ~P_ROUND_FAIL; evt = KD_EVT_ROUND_FAIL; receipt = KE_RECEIPT_FAIL; }
        else if (w_pend & P_VOICE) { w_pend &= ~P_VOICE; evt = KD_EVT_VOICE; }
        else if (w_pend & P_TOUCH) { w_pend &= ~P_TOUCH; evt = KD_EVT_TOUCH; }

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

        soul_behavior_tick((uint32_t)t);

        /* L4 思维闸(记账复刻 soul_think.c, 含 2026-11-19 日翻转清零修复) */
        uint32_t now_min = (uint32_t)((t / 60) % 1440);
        static uint32_t s_last_ctx_min;
        if (s_last_ctx_min != 0 && now_min < s_last_ctx_min &&
            (s_last_ctx_min - now_min) > 12 * 60) {
            s_thought_today = 0;
        }
        s_last_ctx_min = now_min;
        struct ki_think_ctx tc = {
            .arousal = ke->arousal,
            .urgency_max = kd_out.error_max,
            .pending_command = w_ai,
            .quiet_hours = quiet_hrs,
            .new_event = soul_bb_get(SOUL_BB_SALIENCE) > 0.15f,
            .now_min = now_min,
            .last_think_min = s_last_think_min,
            .thought_today = s_thought_today,
            .last_was_silent = s_last_silent,
            .last_silent_score = s_last_silent_score,
        };
        struct ki_think_gate g;
        ki_think_gate(&tc, &g);
        int fired = 0, blocked = 0;
        /* 注: 闸语义=blocked 时 trigger 已被清 false(ignition.c 尾部),
         * 被拦观测须独立看 blocked_mask */
        if (g.trigger || g.blocked_mask) {
            if (g.blocked_mask) { blocked = 1; for (int b = 0; b < 4; b++) if (g.blocked_mask & (1 << b)) s_think_blocked[b]++; }
            else {
                fired = 1; s_think_fires++;
                for (int b = 0; b < 6; b++) if (g.reason_mask & (1 << b)) s_think_reason[b]++;
                s_last_think_min = tc.now_min;
                s_thought_today++;
                s_last_silent = true;         /* 模拟: LLM 多为沉默(板上口径) */
                s_last_silent_score = 0.5f;
            }
        }

        {
            uint32_t idle_s = (uint32_t)t > last_active
                ? (uint32_t)t - last_active : 0;
            if (idle_s > 3600) idle_s = 3600;
            face_bg_select(ke, (uint32_t)t, idle_s);
        }
        if (ke->valence < v_min) v_min = ke->valence;
        if (ke->valence > v_max) v_max = ke->valence;
        if (ke->arousal > a_max) a_max = ke->arousal;

        if (cf && (t % stride == 0))
            fprintf(cf, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%d,%d,%d,%s,%d,%d,%d\n",
                (unsigned long long)t, ke->valence, ke->arousal, ke->mood,
                ke->urgency, ke->social_bond, ke->persistence,
                kd_out.error[KD_SOCIAL], kd_out.error[KD_STIM],
                soul_bb_get(SOUL_BB_SALIENCE), soul_bb_get(SOUL_BB_IDLE),
                quiet_hrs, w_net, w_play, cur_face, fired, blocked,
                ke->ethics_latched ? 1 : 0);
    }

    printf("\n===== SOUL SIM 摘要(%.2f 天) =====\n", ticks / 86400.0);
    printf("深思: 触发 %u 次 | 被拦: quiet=%u interval=%u cap=%u no_new=%u\n",
        s_think_fires, s_think_blocked[0], s_think_blocked[1],
        s_think_blocked[2], s_think_blocked[3]);
    printf("深思原因位: idle=%u low_arousal=%u bin=%u salient=%u new_event=%u mem=%u\n",
        s_think_reason[0], s_think_reason[1], s_think_reason[2],
        s_think_reason[3], s_think_reason[4], s_think_reason[5]);
    printf("情绪: valence [%.3f, %.3f] | arousal max %.3f\n", v_min, v_max, a_max);
    printf("行为: radio_start=%u radio_stop=%u self_face=%u\n",
        bhv_fires[0], bhv_fires[1], bhv_fires[2]);
    printf("勿扰时段占比: %.1f%% | 底色终态: %s\n",
        100.0f * quiet_secs / ticks, cur_face);
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
