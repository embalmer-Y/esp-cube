/*
 * soul.c —— 灵魂编排: L0 感知/反射 + 1Hz tick(kd→ke→黑板) + 行为评分
 *           + prompt/消费出口(docs/14 架构蓝图)
 *
 * 移植自 Kaleido tools/hil/convo-stage/main/soul.c(W0-3), 适配 EspCube;
 * Phase A 骨架重排(2026-11-13, docs/14):
 *   - L0 感知: 总线环境事件(网络/播放/存储/校时)→边沿/跃迁检测→
 *     黑板 NET/PLAYING + 显著性充值; 反射(事件直通不过评分):
 *     瞬态表情(已有)/开机放歌(WiFi↑→music:play, AI-first: 电台是
 *     AI 的能力, 决策权从装配层移交灵魂);
 *   - L1 黑板: kd/ke/情境每拍供给(soul_bb), 单写者=soul tick;
 *   - L2 评分: soul_behavior_tick(无聊放歌/勿扰自停/独处微表情),
 *     行动出口=SOUL_ACT 广播(与深思/会话技能同一词汇表);
 *   - 直控学习: 灵魂动作窗口外的停播=用户手动 → radio_start 退避
 *     2h(礼貌红线: 不跟用户抢方向盘)。
 * kd/ke 为纯函数组件(零平台依赖), 本层持有唯一调用权(单写者纪律);
 * 事件钩子任意任务可调(内部只置 pending 位, tick 内按优先级消费:
 * BARGE > ROUND_* > VOICE)。
 *
 * 消费通道:
 *   - soul_prompt_suffix(): 每轮 LLM system prompt 追加内在状态 +
 *     mood 标签协议(LLM 回复尾部 <mood d="+0.1"/>, soul_strip_mood_tag
 *     剥离并回写 —— LLM 禁直写情感数值, 重评价唯一合法通道);
 *   - soul_tts_hint(): 语速/音高调制(cosyvoice speech_rate/pitch);
 *   - CUBE_EVT_SOUL_MOOD: anim_tag/关键维快照(节流: tag 变化或 60s);
 *   - soul_on_round(): 顺带喂记忆(soul_mem, 另文件)。
 */
#include "soul.h"
#include "cube_act.h"
#include "soul_drive.h"
#include "soul_emotion.h"
#include "soul_bb.h"
#include "soul_behavior.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "cubecore.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CUBE_ENABLE_SOUL

/* SOUL 关闭: 空实现(链接面不变) */
esp_err_t soul_start(void) { return ESP_OK; }
void soul_on_round(bool ok, const char *u, const char *r) { (void)ok;(void)u;(void)r; }
void soul_on_barge(void) {}
void soul_on_voice(void) {}
bool soul_strip_mood_tag(char *text) { (void)text; return false; }
void soul_tts_hint(soul_tts_hint_t *out) { if (out) { out->rate = 1.0f; out->pitch = 0; } }
bool soul_state_get(soul_state_t *out) { if (out) memset(out, 0, sizeof(*out)); return false; }
const char *soul_prompt_suffix(void) { return ""; }
const char *soul_mem_prompt_block(void) { return ""; }

#else /* SOUL 开 */

static const char *TAG = "soul";

#define SOUL_TICK_MS       1000
#define SOUL_VOICE_MIN_MS  5000     /* VOICE 事件限频(Kaleido 同款) */
#define SOUL_IDLE_SAT_SEC  7200u    /* IDLE 因子饱和时长(2h) */
#define SOUL_USERSTOP_DEFER 7200u   /* 用户停台 → radio_start 退避 */

/* app_player_state_t 数值口径(soul 不依赖能力层头, 分层纪律):
 * 0=IDLE 1=CONNECTING 2=PLAYING 3=ERROR; 在播位=连接或播放中 */
#define PLAYER_ST_CONNECTING 1
#define PLAYER_ST_PLAYING    2

/* ---- 事件漏斗(置位任意任务, 消费唯 tick) ---- */
#define PEND_ROUND_OK   0x01
#define PEND_ROUND_FAIL 0x02
#define PEND_BARGE      0x04
#define PEND_VOICE      0x08
static volatile uint8_t s_pend;
/* round 附带文本(单槽, 新覆盖旧 —— 轮结束才写, 会话任务上下文) */
static char s_round_user[192];
static char s_round_reply[96];

/* ---- L0 环境感知漏斗(总线回调置位, tick 消费) ---- */
static volatile bool s_evt_net_up, s_evt_net_down;
static volatile bool s_evt_sd, s_evt_timesync, s_evt_player_chg;
static bool s_time_synced;             /* SNTP 后才有"勿扰时段"裁决权 */
static int  s_prev_playing;            /* 上一拍播放位(跃迁检测) */

/* mood 重评价单槽(值+序号; tick 内消费施加) */
static volatile float s_mood_req;
static volatile uint32_t s_mood_seq;      /* 奇数=待施 */

/* ---- 快照(读写短临界区) ---- */
struct soul_snap {
    struct kd_output drive;
    struct ke_output emo;
    uint64_t tick;
};
static struct soul_snap s_snap;
static SemaphoreHandle_t s_lock;

/* 状态缓存(总线) */
static volatile ai_state_t s_ai_state = AI_IDLE;
static volatile int s_player_state;        /* app_player_state_t */
static int64_t s_last_voice_us;
static uint64_t s_ticks;
static uint64_t s_last_active_tick;        /* 最近非 IDLE 拍(idle 计时) */

/* soul_mem record/boot 声明已收进 soul.h(批次 44, 免手写 extern 漂移) */
/* soul_think 弱耦合(同上) */
extern bool soul_think_fire(uint8_t reason, bool forced, float score);
extern void soul_think_gate_ctx(struct ki_think_ctx *tc);

/* ---- 灵魂 2.0: 表情三层决策(Phase1 实装) ----
 * L0 快反射: 会话事件拍 → 瞬态段(播完回底色, ui 侧 8s 回拉);
 * L1 底色: ke valence/arousal 连续坐标 → 素材坐标最近邻 + 迟滞
 *   (最小驻留 180s 且距离优势 >30% 才切 —— 防频繁跳段);
 * L2 深思: ki_think_gate 触发 → soul_think_fire(深思轮另文件)。 */
static const struct {
    const char *clip;      /* face/ 段名 */
    float v, a;            /* valence -1..1, arousal 0..1 坐标 */
} s_face_map[] = {
    { "smile",       0.6f, 0.50f },
    { "smile_static",0.5f, 0.30f },
    { "laugh",       0.8f, 0.80f },
    { "snigger",     0.6f, 0.60f },
    { "yummy",       0.7f, 0.60f },
    { "confident",   0.5f, 0.55f },
    { "leisure",     0.3f, 0.30f },
    { "music",       0.5f, 0.60f },
    { "shy",         0.2f, 0.45f },
    { "mock",       -0.1f, 0.55f },
    { "sad",        -0.6f, 0.25f },
    { "cry",        -0.7f, 0.45f },
    { "sigh",       -0.4f, 0.20f },
    { "angry",      -0.7f, 0.80f },
    { "panic",      -0.5f, 0.90f },
    { "shocked",     0.0f, 0.90f },
    { "question",    0.0f, 0.60f },
    { "ponder",      0.0f, 0.45f },
    { "investigate", 0.1f, 0.55f },
    { "yawn",        0.0f, 0.10f },
    { "asleep",      0.1f, 0.05f },
    { "mute",        0.0f, 0.30f },
    { "badminton",   0.6f, 0.70f },
};
#define FACE_MAP_N  (sizeof(s_face_map) / sizeof(s_face_map[0]))

static void face_act_publish(uint8_t kind, const char *clip)
{
    cube_soul_act_t a = { .kind = kind };
    strlcpy(a.content, clip, sizeof(a.content));
    cube_event_publish(CUBE_EVT_SOUL_ACT, &a, sizeof(a));
}

/* ---- 统一动作门提交(docs/18 T13 首段): 行为胜出不再直发 SOUL_ACT,
 * 改经 cube_act(策略/资源/停止锁存/回执) → 装配执行器走原下游链。
 * 拒绝/失败回执记 WARN; 回执 started=已交付执行器(受理级, T35 收口)。 */
static void act_submit_log(const char *cap, const char *param, size_t plen)
{
    cact_req_t r;
    memset(&r, 0, sizeof(r));
    r.origin = CACT_ORIGIN_AUTONOMOUS_LOCAL;
    strlcpy(r.capability, cap, sizeof(r.capability));
    if (param != NULL && plen > 0 && plen <= CACT_PARAMS_MAX) {
        memcpy(r.params, param, plen);
        r.params_len = plen;
    }
    uint32_t id = 0;
    cact_res_t res = cact_submit(cact_default(), &r, &id);
    if (res != CACT_RES_OK) {
        ESP_LOGW(TAG, "act %s rejected: %d", cap, res);
        return;
    }
    cact_pump(cact_default());
    cact_receipt_t rc;
    if (cact_receipt_last(cact_default(), id, &rc) == CACT_RES_OK) {
        ESP_LOGI(TAG, "act %s -> #%u %s%s%s", cap, id,
                 cact_status_name(rc.status),
                 rc.reason[0] ? "(" : "", rc.reason[0] ? rc.reason : "");
    }
}

/* L1 底色选择(最近邻 + 迟滞; 挂在 mood_publish 拍) */
static void face_bg_select(const struct soul_snap *sn)
{
    static char cur[24] = "leisure";     /* 开机底色 */
    static uint32_t since_tick;
    int best = -1;
    float bd = 1e9f;
    for (unsigned i = 0; i < FACE_MAP_N; i++) {
        float dv = sn->emo.state.valence - s_face_map[i].v;
        float da = sn->emo.state.arousal - s_face_map[i].a;
        float d = dv * dv + da * da * 0.5f;
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (best < 0) return;
    if (strcmp(cur, s_face_map[best].clip) == 0) {
        since_tick = (uint32_t)sn->tick;
        return;
    }
    /* 迟滞: 驻留 <180s 不切; 距离优势不足(新点距 > 0.7×当前段距)不切 */
    if (sn->tick - since_tick < 180) return;
    for (unsigned i = 0; i < FACE_MAP_N; i++) {
        if (strcmp(cur, s_face_map[i].clip) == 0) {
            float cv = sn->emo.state.valence - s_face_map[i].v;
            float ca = sn->emo.state.arousal - s_face_map[i].a;
            if (bd > 0.7f * 0.7f * (cv * cv + ca * ca * 0.5f)) return;
            break;
        }
    }
    strlcpy(cur, s_face_map[best].clip, sizeof(cur));
    since_tick = (uint32_t)sn->tick;
    ESP_LOGI(TAG, "face bg -> %s (v=%.2f a=%.2f)", cur,
             sn->emo.state.valence, sn->emo.state.arousal);
    face_act_publish(0, cur);
}

/* L0 事件瞬态(单拍, 消费事件时调) */
static void face_evt_transient(enum kd_event evt)
{
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
    switch (evt) {
    case KD_EVT_BARGE:     face_act_publish(1, "shocked"); break;
    case KD_EVT_ROUND_OK:  face_act_publish(1, "smile");   break;
    case KD_EVT_ROUND_FAIL:face_act_publish(1, "sigh");    break;
    default: break;                   /* VOICE/NONE 无瞬态 */
    }
#endif
}

/* ---- 总线缓存 ---- */
static void soul_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                        void *ctx)
{
    (void)len; (void)ctx;
    if (id == CUBE_EVT_AI_STATE && data) {
        ai_state_t prev = s_ai_state;
        s_ai_state = *(const ai_state_t *)data;
        if (s_ai_state == AI_LISTENING) {
            /* 唤醒进入聆听 = 强社交在场信号(等价 Kaleido VOICE 拍) */
            soul_on_voice();
        }
        if (prev != s_ai_state) soul_bb_salience(0.3f);
    } else if (id == CUBE_EVT_PLAYER_STATE && data) {
        int prev = s_player_state;
        s_player_state = *(const int *)data;
        if (prev != s_player_state) s_evt_player_chg = true;
    } else if (id == CUBE_EVT_WIFI_CONNECTED) {
        s_evt_net_up = true;             /* 边沿去重在 tick 消费侧 */
    } else if (id == CUBE_EVT_WIFI_DISCONNECTED) {
        s_evt_net_down = true;
    } else if (id == CUBE_EVT_SD_READY) {
        s_evt_sd = true;
    } else if (id == CUBE_EVT_TIME_SYNCED) {
        s_evt_timesync = true;
    }
}

/* ---- 事件钩子(任意任务) ---- */
static void voice_rate_gate(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_voice_us < SOUL_VOICE_MIN_MS * 1000LL) return;
    s_last_voice_us = now;
    s_pend |= PEND_VOICE;
}

void soul_on_voice(void) { voice_rate_gate(); }

void soul_on_barge(void)
{
    s_pend |= PEND_BARGE;             /* 最高优先级(消费序保证) */
}

void soul_on_round(bool ok, const char *user_text, const char *reply_first)
{
    strlcpy(s_round_user, user_text ? user_text : "",
            sizeof(s_round_user));
    strlcpy(s_round_reply, reply_first ? reply_first : "",
            sizeof(s_round_reply));
    s_pend |= ok ? PEND_ROUND_OK : PEND_ROUND_FAIL;
}

/* ---- mood 标签: 剥离 + 受理 ---- */
bool soul_strip_mood_tag(char *text)
{
    if (text == NULL) return false;
    char *p = strstr(text, "<mood");
    if (p == NULL) return false;
    char *q = strchr(p, '>');
    if (q == NULL) return false;
    /* d="±0.x" 提取 */
    float d = 0;
    char *dp = strstr(p, "d=\"");
    bool ok = false;
    if (dp && dp < q) {
        ok = sscanf(dp + 3, "%f", &d) == 1;
    }
    *p = '\0';                        /* 剥掉标签(含), TTS 不念 */
    if (!ok) return false;
    if (d > 0.2f) d = 0.2f;
    if (d < -0.2f) d = -0.2f;
    s_mood_req = d;
    s_mood_seq++;                     /* 奇数槽=待施(单写者序) */
    if ((s_mood_seq & 1) == 0) s_mood_seq++;
    return true;
}

/* ---- 消费出口 ---- */
void soul_tts_hint(soul_tts_hint_t *out)
{
    if (out == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50))) {
        out->rate = s_snap.emo.tts_hint.rate;
        out->pitch = s_snap.emo.tts_hint.pitch;
        xSemaphoreGive(s_lock);
    } else {
        out->rate = 1.0f;
        out->pitch = 0;
    }
}

bool soul_state_get(soul_state_t *out)
{
    if (out == NULL) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    out->valence = s_snap.emo.state.valence;
    out->arousal = s_snap.emo.state.arousal;
    out->urgency = s_snap.emo.state.urgency;
    out->social_bond = s_snap.emo.state.social_bond;
    out->ethics_latched = s_snap.emo.state.ethics_latched;
    strlcpy(out->anim_tag, s_snap.emo.anim_tag, sizeof(out->anim_tag));
    for (int i = 0; i < KD_N; i++) {
        out->drive_level[i] = s_snap.drive.level[i];
    }
    out->ai_idle = (s_ai_state == AI_IDLE);
    out->idle_sec = (int)(s_ticks - s_last_active_tick);
    xSemaphoreGive(s_lock);
    return true;
}

static const char *anim_cn(const char *tag)
{
    static const struct { const char *en, *cn; } m[] = {
        { "happy", "开心" }, { "relaxed", "放松" }, { "angry", "烦躁" },
        { "sad", "低落" }, { "surprised", "惊讶" }, { "confused", "困惑" },
        { "sleepy", "困倦" },
    };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (strcmp(tag, m[i].en) == 0) return m[i].cn;
    }
    return "平静";
}

const char *soul_prompt_suffix(void)
{
    static char buf[512];
    struct soul_snap sn;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return "";
    sn = s_snap;
    xSemaphoreGive(s_lock);

    const char *mood = anim_cn(sn.emo.anim_tag);
    /* 词表全部单字(3B)/双字(6B): %.Ns 按"字节"截断会撕裂多字节续字节
     * → system prompt 非法 UTF-8 → DashScope 400(2026-11-12 板上定位:
     * arousal 中档时 "%.4s"×"中等" 必现, 高/低单字侥幸安全)。
     * 纪律: 进 %.4s 槽位的词只许单字 */
    const char *vigor = sn.emo.state.arousal > 0.55f ? "高" :
                        sn.emo.state.arousal < 0.2f ? "低" : "中";
    const char *urge = sn.emo.state.urgency > 0.3f ? "强" : "弱";
    const char *ethics = sn.emo.state.ethics_latched
        ? "(当前处于安抚模式: 只能用温柔安抚的语气说话。)" : "";
    snprintf(buf, sizeof(buf),
        "[内在状态] 你此刻: 心情%.8s, 活力%.4s, 表达冲动%.4s。请自然融入"
        "回复语气, 不要直接复述这些词。在回复的最末尾附加恰好一个形如"
        "<mood d=\"+0.1\"/> 的标签, d 是 -0.2 到 +0.2 的一位小数, 表示"
        "你听完这轮对话后的心情变化, 不要输出其他标签或解释。%s",
        mood, vigor, urge, ethics);
    return buf;
}

/* ---- mood 事件(节流: tag 变化或 60s) ---- */
static void mood_publish(const struct soul_snap *sn, bool force)
{
    static char last_tag[KE_ANIM_TAG_MAX];
    static uint32_t last_pub_tick;
    if (!force && strcmp(last_tag, sn->emo.anim_tag) == 0 &&
        sn->tick - last_pub_tick < 60) {
        return;
    }
    strlcpy(last_tag, sn->emo.anim_tag, sizeof(last_tag));
    last_pub_tick = (uint32_t)sn->tick;

    cube_soul_mood_t m = { 0 };
    strlcpy(m.anim_tag, sn->emo.anim_tag, sizeof(m.anim_tag));
    m.valence = sn->emo.state.valence;
    m.arousal = sn->emo.state.arousal;
    m.urgency = sn->emo.state.urgency;
    m.social_bond = sn->emo.state.social_bond;
    m.ethics_latched = sn->emo.state.ethics_latched;
    cube_event_publish(CUBE_EVT_SOUL_MOOD, &m, sizeof(m));
}

/* ---- L0 感知消费(tick 内; 边沿/跃迁 → 黑板/显著性/反射/直控学习) ---- */
static void perception_consume(void)
{
    if (s_evt_net_up) {
        s_evt_net_up = false;
        bool was_up = soul_bb_get(SOUL_BB_NET) > 0.5f;
        soul_bb_set(SOUL_BB_NET, 1.0f);
        soul_bb_salience(0.6f);
#if CONFIG_ESP_CUBE_SOUL_BOOT_RADIO
        /* 开机反射(AI-first): 联网即自娱放歌 —— 原装配层演示行为
         * 移交灵魂; 用户手动停台退避期内不复播(尊重直控) */
        if (!was_up && soul_bb_get(SOUL_BB_PLAYING) < 0.5f &&
            !soul_behavior_deferred("radio_start")) {
            ESP_LOGI(TAG, "L0 reflex: boot radio play");
            act_submit_log("radio.play", NULL, 0);
        }
#endif
    }
    if (s_evt_net_down) {
        s_evt_net_down = false;
        soul_bb_set(SOUL_BB_NET, 0.0f);
        soul_bb_salience(0.6f);
    }
    if (s_evt_sd) {
        s_evt_sd = false;
        soul_bb_salience(0.4f);
    }
    if (s_evt_timesync) {
        s_evt_timesync = false;
        s_time_synced = true;          /* 此后勿扰时段才有裁决权 */
        soul_bb_salience(0.5f);
    }

    /* 播放跃迁: 停止归因已改为入口显式调用(soul_note_user_stop, T13 续)
     * —— docs/15 P0-3 批评的"时间窗猜测误归因"随之移除; 自然结束/
     * 电台断流只做显著性充值, 不再误判为用户拒绝 */
    int playing = (s_player_state == PLAYER_ST_CONNECTING ||
                   s_player_state == PLAYER_ST_PLAYING) ? 1 : 0;
    if (s_evt_player_chg) {
        s_evt_player_chg = false;
        soul_bb_salience(0.3f);
    }
    s_prev_playing = playing;
    soul_bb_set(SOUL_BB_PLAYING, (float)playing);
}

/* 用户显式停止归因(T13 续): 停止入口(CLI/触摸/语音)调用 —— 替代旧
 * "灵魂动作窗口 + 时间猜测"归因(docs/15 P0-3/4 批评的误归因根因):
 * 自然结束/电台断流不再被误判为用户拒绝; 锁存由入口 cact_user_stop 负责 */
void soul_note_user_stop(void)
{
    soul_behavior_defer("radio_start", SOUL_USERSTOP_DEFER);
    ESP_LOGI(TAG, "user stop noted -> radio_start deferred %us",
             SOUL_USERSTOP_DEFER);
}

/* ---- 黑板供给(kd/ke + 情境; 单写者=tick) ---- */
static void bb_feed(const struct kd_output *kd, const struct ke_output *ke)
{
    soul_bb_set(SOUL_BB_VALENCE, ke->state.valence);
    soul_bb_set(SOUL_BB_AROUSAL, ke->state.arousal);
    soul_bb_set(SOUL_BB_URGENCY, ke->state.urgency);
    soul_bb_set(SOUL_BB_BOND,    ke->state.social_bond);
    soul_bb_set(SOUL_BB_ETHICS,  ke->state.ethics_latched ? 1 : 0);
    /* 动机缺口 = kd 误差(水库缺口口径, docs/14 §4) */
    soul_bb_set(SOUL_BB_M_SOCIAL, kd->error[KD_SOCIAL]);
    soul_bb_set(SOUL_BB_M_STIM,   kd->error[KD_STIM]);

    soul_bb_set(SOUL_BB_AI_ACTIVE, s_ai_state != AI_IDLE ? 1 : 0);
    uint32_t idle_sec = (uint32_t)(s_ticks - s_last_active_tick);
    float idle = (float)idle_sec / SOUL_IDLE_SAT_SEC;
    soul_bb_set(SOUL_BB_IDLE, idle > 1 ? 1 : idle);

    /* 勿扰: SNTP 前无裁决权(1970 假夜里会误停播, 板上口径) */
    bool quiet = false;
    if (s_time_synced) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        quiet = tmv.tm_hour >= 23 || tmv.tm_hour < 8;
    }
    soul_bb_set(SOUL_BB_QUIET, quiet ? 1 : 0);

    soul_bb_tick((uint32_t)s_ticks);
}

/* ---- L2/L3 行为(注册表+执行器; 行动出口=SOUL_ACT 词汇表) ---- */
static void exec_radio_start(void)
{
    ESP_LOGI(TAG, "bh radio_start (boredom)");
    act_submit_log("radio.play", NULL, 0);
}

static void exec_radio_stop(void)
{
    ESP_LOGI(TAG, "bh radio_stop (quiet hours)");
    soul_behavior_defer("radio_start", 600);  /* 防同夜反复起播 */
    act_submit_log("music.stop", NULL, 0);
}

static void exec_self_face(void)
{
    /* 独处自娱微表情(存在感): 轮换挑段, 不引入 RNG(单测可复现) */
    static const char *picks[] = { "yawn", "ponder", "music" };
    static unsigned pi;
    const char *clip = picks[pi++ % 3];
    ESP_LOGI(TAG, "bh self_face (%s)", clip);
    char blob[24];
    size_t n = strlcpy(blob, clip, sizeof(blob)) + 1;  /* 含 NUL */
    act_submit_log("face.show", blob, n);
}

static void behaviors_register(void)
{
    static const soul_cons_t c_radio_start[] = {
        { SOUL_BB_M_STIM,     SOUL_C_RISE, 1.0f },
        { SOUL_BB_PLAYING,    SOUL_C_FALL, 1.0f },
        { SOUL_BB_QUIET,      SOUL_C_FALL, 1.0f },
        { SOUL_BB_AI_ACTIVE,  SOUL_C_FALL, 1.0f },
    };
    static const soul_cons_t c_radio_stop[] = {
        { SOUL_BB_QUIET,      SOUL_C_RISE, 1.0f },
        { SOUL_BB_PLAYING,    SOUL_C_RISE, 1.0f },
    };
    static const soul_cons_t c_self_face[] = {
        { SOUL_BB_IDLE,       SOUL_C_RISE, 1.2f },
        { SOUL_BB_M_SOCIAL,   SOUL_C_RISE, 1.0f },
        { SOUL_BB_AI_ACTIVE,  SOUL_C_FALL, 1.0f },
    };
    soul_behavior_register(&(soul_bhv_t){
        .name = "radio_start", .cons = c_radio_start,
        .cons_n = 4, .threshold = 0.35f, .w = 1.0f,
        .cooldown_s = 1800, .exec = exec_radio_start,
    });
    soul_behavior_register(&(soul_bhv_t){
        .name = "radio_stop", .cons = c_radio_stop,
        .cons_n = 2, .threshold = 0.80f, .w = 1.0f,
        .cooldown_s = 3600, .exec = exec_radio_stop,
    });
    soul_behavior_register(&(soul_bhv_t){
        .name = "self_face", .cons = c_self_face,
        .cons_n = 3, .threshold = 0.50f, .w = 1.0f,
        .cooldown_s = 480, .exec = exec_self_face,
    });
}

/* ---- 1Hz 灵魂任务(单写者: kd/ke/黑板唯一调用权) ---- */
static void soul_task(void *arg)
{
    (void)arg;
    struct kd_output kd_out;
    struct ke_output ke_out;
    kd_reset();                        /* level 全 1(初始零误差) */
    ke_reset(NULL);
    TickType_t next = xTaskGetTickCount();
    /* 开机静默 60s(Kaleido: WiFi 未稳/不打扰) —— 期间照常 tick 但不发
     * 事件; 静默语义在本层没有主动出口(proactive 独立门控), 仅保拍 */
    bool booted = false;

    while (1) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SOUL_TICK_MS));
        s_ticks++;
        if (s_ticks == 60) booted = true;
        (void)booted;

        /* 1) 感知: 环境事件(网络/播放/存储/校时) → 黑板/反射 */
        perception_consume();

        /* 2) 事件消费(每拍至多一个, 优先级 BARGE > ROUND > VOICE) */
        uint8_t p = s_pend;
        enum kd_event evt = KD_EVT_NONE;
        char ru[192], rr[96];
        ru[0] = rr[0] = '\0';
        bool round_now = false;
        bool round_ok = false;
        if (p & PEND_BARGE) {
            s_pend &= ~PEND_BARGE;
            evt = KD_EVT_BARGE;
            face_evt_transient(KD_EVT_BARGE);       /* L0 快反射 */
        } else if (p & PEND_ROUND_OK) {
            s_pend &= ~PEND_ROUND_OK;
            evt = KD_EVT_ROUND_OK;
            face_evt_transient(KD_EVT_ROUND_OK);
            round_now = round_ok = true;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_ROUND_FAIL) {
            s_pend &= ~PEND_ROUND_FAIL;
            evt = KD_EVT_ROUND_FAIL;
            face_evt_transient(KD_EVT_ROUND_FAIL);
            round_now = true;
            round_ok = false;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_VOICE) {
            s_pend &= ~PEND_VOICE;
            evt = KD_EVT_VOICE;
        }

        /* 3) 驱力: quiet = AI idle 且播放器停/电台待机(rest 恢复口径) */
        bool quiet = (s_ai_state == AI_IDLE) && (s_player_state == 0);
        if (s_ai_state != AI_IDLE) s_last_active_tick = s_ticks;
        struct kd_inputs din = {
            .tick = s_ticks,
            .quiet = quiet,
            .charging = false,
            .event = evt,
        };
        kd_step(s_ticks, &din, &kd_out);

        /* 4) mood 重评价(先于 ke_step: 本拍情绪即受影响) */
        if ((s_mood_seq & 1) && s_mood_req != 0) {
            struct ke_reappraisal r = {
                .delta = s_mood_req,
                .receipt_id = s_mood_seq,
            };
            if (ke_reappraisal_request(&r) == 0) {
                ESP_LOGI(TAG, "mood reappraisal %+.2f", s_mood_req);
            }
            s_mood_seq++;             /* 消费完清奇偶(待施槽复位) */
        }

        /* 5) 感受态 */
        struct ke_inputs kin = {
            .have_drive = true,
            .drive_error_sum = kd_out.error_sum,
            .drive_error_max = kd_out.error_max,
            .receipt = round_now ? (round_ok ? KE_RECEIPT_SUCCESS
                                              : KE_RECEIPT_FAIL)
                                 : KE_RECEIPT_NONE,
            .social_event = (evt != KD_EVT_NONE),
        };
        ke_step(s_ticks, &kin, &ke_out);

        /* 6) 快照(短临界区写) + 黑板供给 + 消费出口 */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20))) {
            s_snap.drive = kd_out;
            s_snap.emo = ke_out;
            s_snap.tick = s_ticks;
            xSemaphoreGive(s_lock);
        }
        bb_feed(&kd_out, &ke_out);
        mood_publish(&s_snap, false);
        face_bg_select(&s_snap);        /* L1 底色(连续坐标+迟滞) */

        /* 7) L2 行为评分器(本地自主; 与深思共享词汇表不共享决策) */
        soul_behavior_tick((uint32_t)s_ticks);

#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
        /* 8) L4 思维闸: J(黑板显著性/判据)+E(ke)+V(kd 误差) → 深思 */
        {
            struct ki_think_ctx tc = {
                .arousal = ke_out.state.arousal,
                .urgency_max = kd_out.error_max,
                .pending_command = (s_ai_state != AI_IDLE),
                .quiet_hours = soul_bb_get(SOUL_BB_QUIET) > 0.5f,
                .new_event = soul_bb_get(SOUL_BB_SALIENCE) > 0.15f,
                .mem_hook = 0,             /* W4 记忆钩子后续接 */
            };
            soul_think_gate_ctx(&tc);
            struct ki_think_gate g;
            ki_think_gate(&tc, &g);
            if (g.trigger && !g.blocked_mask) {
                soul_think_fire(g.reason_mask, false, 0.6f);
            }
        }
#endif

        /* 9) 记忆(轮结束拍; 与快照同拍, 拿的是当拍前情绪——可接受) */
        if (round_now && ru[0]) {
            soul_mem_record(ru, rr, ke_out.state.valence,
                            ke_out.state.arousal);
        }
    }
}

esp_err_t soul_start(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;

    soul_mem_boot();
    behaviors_register();
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(cube_event_subscribe(soul_evt_cb, NULL),
                        TAG, "evt subscribe");
    /* 栈预分配(docs/22 §3B): 内部静态, 3KB 小任务链接期占位 */
    static StaticTask_t s_soul_tcb;
    static StackType_t s_soul_stack[3072 / sizeof(StackType_t)];
    if (xTaskCreateStatic(soul_task, "soul", 3072, NULL, 4,
                          s_soul_stack, &s_soul_tcb) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "soul up (1Hz, percep→kd→ke→bb→behavior)");
    return ESP_OK;
}

#endif /* CONFIG_ESP_CUBE_ENABLE_SOUL */
