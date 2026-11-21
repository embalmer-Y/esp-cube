/*
 * soul.c —— 灵魂编排: L0 感知 + 1Hz tick(kd→ke→黑板→trait) + 深思触发
 *           + prompt/消费出口(docs/14 架构蓝图)
 *
 * 移植自 Kaleido tools/hil/convo-stage/main/soul.c(W0-3), 适配 EspCube;
 * Phase A 骨架重排(2026-11-13, docs/14); S3 重构(2026-11-20, docs/24 §13):
 *   - L0 感知: 总线环境事件(网络/播放/存储/校时)→边沿/跃迁检测→
 *     黑板 NET/PLAYING + 显著性充值。反射已全部移除(R6 行为来源
 *     原则: 行动只来自用户指令与 LLM 深思; 开机/重连放歌反射
 *     与 L2 行为表于 S3 退役);
 *   - L1 黑板: kd/ke/情境每拍供给(soul_bb), 单写者=soul tick;
 *   - 行动面: 深思(ki)经预算硬门(soul_budget 分账)+SOUL_ACT 广播;
 *     用户手动停台 → 2h 深思音频抑制窗(礼貌红线: 不跟用户抢
 *     方向盘, docs/17 §5.5)。
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
#include "soul_face.h"
#include "soul_trait.h"
#include "soul_bb.h"
#include "soul_budget.h"
#include "soul_motiv.h"
#include "soul_moodgate.h"
#include "soul_jsonfix.h"

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
static bool s_initiative_tick;
static uint32_t s_reflect_cd_until;     /* B-6b: 反思冷却截止(tick) */   /* 本拍自发发起(trait 观测) */
static uint32_t s_audio_stop_until;  /* 用户停台→深思音频抑制窗止点 */
static int s_engine_trend;              /* 上拍 Δvalence 符号(moodgate 断三) */
/* round 附带文本(单槽, 新覆盖旧 —— 轮结束才写, 会话任务上下文) */
static char s_round_user[192];
static char s_round_reply[96];

/* ---- L0 环境感知漏斗(总线回调置位, tick 消费) ---- */
static volatile bool s_evt_net_up, s_evt_net_down;
static volatile bool s_evt_sd, s_evt_timesync, s_evt_player_chg;
static volatile bool s_evt_wake, s_evt_touch;   /* B-6a 喂点桥 */
static bool s_time_synced;             /* SNTP 后才有"勿扰时段"裁决权 */
static int  s_prev_playing;            /* 上一拍播放位(跃迁检测) */

/* mood 重评价单槽(值+序号; tick 内消费施加) */
static volatile float s_mood_req;
static volatile uint32_t s_mood_seq;      /* 奇数=待施 */

/* B-6c 深思 appraisal 漏斗(think_task 提交, tick 消费剂量) */
static struct soul_appraisal s_appr;
static volatile uint32_t s_appr_seq;      /* 奇数=待施 */

/* B-6c 深思结算漏斗(§14.2-1 收口: sm_settle 收敛回 soul tick 单写者;
 * 值=sm_settle+1, 0=无 pending) */
static volatile uint8_t s_settle_pend;
static volatile uint32_t s_settle_s;

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
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
extern void soul_think_round_answered(void);   /* B-6a: kc 问句闭合 */
extern bool soul_think_reflect_fire(void);     /* B-6b: 反思轮触发 */
#endif

/* ---- 灵魂 2.0: 表情三层决策(Phase1 实装; S2 升级 docs/24 §2.3) ----
 * L0 快反射: 会话事件拍 → 瞬态段(播完回底色, ui 侧 8s 回拉);
 * L1 底色: soul_face 模块(Russell 扇区+中性死区+分档驻留+mood 底色
 *   回落; 素材表单一真源 soul_face.c, 与 soul_sim 同源编译);
 * L4 深思: sm_gate 仲裁触发(S5) → soul_think_fire(深思轮另文件)。 */

static void face_act_publish(uint8_t kind, const char *clip)
{
    cube_soul_act_t a = { .kind = kind };
    strlcpy(a.content, clip, sizeof(a.content));
    cube_event_publish(CUBE_EVT_SOUL_ACT, &a, sizeof(a));
}

/* trait 慢变量注入(S2.5b; act_gate 在 persona 加载/genesis 后调用) */
void soul_trait_load(const float tr[TRAIT_DIM])
{
    float cur[TRAIT_DIM];
    trait_get(cur);
    if (memcmp(cur, tr, sizeof(cur)) == 0)
        return;
    if (!trait_set(tr)) {
        ESP_LOGW(TAG, "trait load OUT_OF_BAND rejected");
        return;
    }
    ESP_LOGI(TAG, "trait load C=%.2f W=%.2f E=%.2f A=%.2f R=%.2f",
             tr[0], tr[1], tr[2], tr[3], tr[4]);
}

/* L1 底色选择(soul_face 扇区三件套, docs/24 §2.3; 挂在 mood_publish 拍) */
static void face_bg_select(const struct soul_snap *sn)
{
    static char cur[24] = "leisure";     /* 上次发布(开机底色) */
    uint64_t idle = sn->tick > s_last_active_tick
        ? sn->tick - s_last_active_tick : 0;
    const char *clip = soul_face_bg(sn->emo.state.valence,
                                    sn->emo.state.arousal,
                                    sn->emo.state.mood,
                                    (uint32_t)sn->tick,
                                    (uint32_t)(idle > 3600 ? 3600 : idle),
                                    sn->emo.state.ethics_latched);
    if (strcmp(cur, clip) == 0)
        return;
    strlcpy(cur, clip, sizeof(cur));
    ESP_LOGI(TAG, "face bg -> %s (v=%.2f a=%.2f m=%.2f)", cur,
             sn->emo.state.valence, sn->emo.state.arousal,
             sn->emo.state.mood);
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
    } else if (id == CUBE_EVT_WAKE_DETECTED) {
        /* B-6a 喂点: 唤醒词命中(U=1.0 最高突发事件, docs/24 §14.2-6) */
        s_evt_wake = true;
    } else if (id == CUBE_EVT_TOUCH) {
        s_evt_touch = true;
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
    /* 三断法(S6/D4, docs/24 §5.4): 隔轮采纳+×0.5+方向校验; 自增强
     * 指纹暂停期不施加(标签照剥, 回写丢弃)。 */
    static uint32_t s_mg_round;
    s_mg_round++;
    if (soul_moodgate_paused((uint32_t)(esp_timer_get_time() / 1000000LL)))
        return true;
    float applied = 0;
    if (!soul_moodgate_allow(s_mg_round, s_engine_trend, d, &applied))
        return true;
    s_mood_req = applied;
    s_mood_seq++;                     /* 奇数槽=待施(单写者序) */
    if ((s_mood_seq & 1) == 0) s_mood_seq++;
    return true;
}

/* ---- 深思 appraisal 快通道(B-6c; think_task 提交, tick 消费) ---- */
void soul_appraisal_submit(const struct soul_appraisal *a)
{
    if (a == NULL) return;
    s_appr = *a;                    /* 单飞单写者: 序号升奇前写完 */
    s_appr_seq++;                   /* 奇数槽=待施(单写者序, 同 mood 槽) */
    if ((s_appr_seq & 1) == 0) s_appr_seq++;
}

/* ---- 深思结算漏斗(B-6c, §14.2-1 收口; think_task 调, tick 消费) ---- */
void soul_settle_post(uint8_t s, uint32_t now_s)
{
    s_settle_s = now_s;
    s_settle_pend = (uint8_t)(s + 1);   /* 0=无 pending(编码错开) */
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

/* 人格叙事注入(§55.29: 深思背景块"你是谁"; act_gate 调) */
static char s_persona_desc[112];

void soul_persona_desc(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        s_persona_desc[0] = '\0';
        return;
    }
    size_t len = strlen(line);
    if (len >= sizeof(s_persona_desc))
        len = sizeof(s_persona_desc) - 1;
    memcpy(s_persona_desc, line, len);
    /* UTF-8 安全边界回退(截断不落半个汉字, D2 同款纪律) */
    while (len > 0 && (s_persona_desc[len] & 0xC0) == 0x80)
        len--;
    s_persona_desc[len] = '\0';
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
    out->audio_user_stopped = s_ticks < s_audio_stop_until;
    out->idle_sec = (int)(s_ticks - s_last_active_tick);
    strlcpy(out->persona_desc, s_persona_desc, sizeof(out->persona_desc));
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
        soul_bb_set(SOUL_BB_NET, 1.0f);
        soul_bb_salience(0.6f);
        sm_event(SM_EVT_NET, (uint32_t)s_ticks);
        /* S3(B-2/R6): 开机/重连反射已移除(docs/24 §6, docs/17 §5.5 合同)
         * —— 设备开机静默, 电台只由用户指令或 LLM 深思起。联网边沿仅
         * 显著性充值(深思素材), 不直接行动。 */
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
    /* B-6a 喂点: WAKE(U=1.0 最高突发, ku 余辉 0.6) / TOUCH(U=0.35,
     * ku 余辉 0.2; docs/24 §14.2-6 板上未活两源接通) */
    if (s_evt_wake) {
        s_evt_wake = false;
        sm_event(SM_EVT_WAKE, (uint32_t)s_ticks);
        soul_bb_salience(0.8f);
    }
    if (s_evt_touch) {
        s_evt_touch = false;
        sm_event(SM_EVT_TOUCH, (uint32_t)s_ticks);
        soul_bb_set(SOUL_BB_TOUCH, 1.0f);   /* 黑板触摸因子(τ60s 衰减) */
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
        sm_event(SM_EVT_PLAY_CHG, (uint32_t)s_ticks);
    }
    s_prev_playing = playing;
    soul_bb_set(SOUL_BB_PLAYING, (float)playing);
}

/* 用户显式停止归因(T13 续): 停止入口(CLI/触摸/语音)调用 —— 替代旧
 * "灵魂动作窗口 + 时间猜测"归因(docs/15 P0-3/4 批评的误归因根因):
 * 自然结束/电台断流不再被误判为用户拒绝。
 * S3(B-2): 旧 radio_start 退避随 L2 表退役, 换为音频抑制窗——窗口内
 * 深思 music 行动被硬门拒绝(docs/17 §5.5 用户停止合同; 跨重启锁存
 * 挂账, 见 docs/24 §13 偏差)。 */
void soul_note_user_stop(void)
{
    s_audio_stop_until = s_ticks + SOUL_USERSTOP_DEFER;
    ESP_LOGI(TAG, "user stop noted -> audio suppressed %us",
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

/* ---- 1Hz 灵魂任务(单写者: kd/ke/黑板唯一调用权) ---- */
static void soul_task(void *arg)
{
    (void)arg;
    struct kd_output kd_out;
    struct ke_output ke_out;
    trait_reset();   /* S2.5: 底座中性+调制面应用(建角色后由 persona 接管) */
    kd_reset();                        /* level 全 1(初始零误差) */
    ke_reset(NULL);
    TickType_t next = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SOUL_TICK_MS));
        s_ticks++;

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
            sm_event(SM_EVT_BARGE, (uint32_t)s_ticks);
        } else if (p & PEND_ROUND_OK) {
            s_pend &= ~PEND_ROUND_OK;
            evt = KD_EVT_ROUND_OK;
            face_evt_transient(KD_EVT_ROUND_OK);
            sm_event(SM_EVT_ROUND_OK, (uint32_t)s_ticks);
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
            soul_think_round_answered();   /* B-6a: 轮答=深思遗留问句闭合 */
#endif
            round_now = round_ok = true;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_ROUND_FAIL) {
            s_pend &= ~PEND_ROUND_FAIL;
            evt = KD_EVT_ROUND_FAIL;
            face_evt_transient(KD_EVT_ROUND_FAIL);
            sm_event(SM_EVT_ROUND_FAIL, (uint32_t)s_ticks);
            round_now = true;
            round_ok = false;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_VOICE) {
            s_pend &= ~PEND_VOICE;
            evt = KD_EVT_VOICE;
        }

        /* 2.5) 深思结算漏斗消费(B-6c: sm_settle 同任务执法, §14.2-1
         * 收口——kf/ku 回灌不再跨任务直写) */
        if (s_settle_pend) {
            sm_settle((enum sm_settle)(s_settle_pend - 1), s_settle_s);
            s_settle_pend = 0;
        }

        /* 3) 驱力: quiet = AI idle 且播放器停/电台待机(rest 恢复口径) */
        bool quiet = (s_ai_state == AI_IDLE) && (s_player_state == 0);
        if (s_ai_state != AI_IDLE) s_last_active_tick = s_ticks;
        struct kd_inputs din = {
            .tick = s_ticks,
            .quiet = quiet,
            .charging = false,
            .stimulating = (!quiet || s_ai_state != AI_IDLE),
            .event = evt,
        };
        kd_step(s_ticks, &din, &kd_out);

        /* 4) 快层重评价汇流(mood 标签 D4 通道 ∪ B-6c 深思 appraisal;
         * ke_reappraisal 单槽——同拍两源合并单请求, 防后写覆盖;
         * 先于 ke_step: 本拍情绪即受影响) */
        float reap = 0;
        uint64_t reap_id = 0;
        bool mood_hit = false;
        if ((s_mood_seq & 1) && s_mood_req != 0) {
            reap = s_mood_req;
            reap_id = s_mood_seq;
            mood_hit = true;
        }
        if (s_appr_seq & 1) {
            /* B-6c appraisal 剂量(§11.6 保守形态: 规则表=主通道, LLM
             * 评价=hint, 经 soul_dose 固定映射走官方注入口) */
            float soc, eth, val;
            soul_dose_compute(&s_appr, &soc, &eth, &val);
            if (soc != 0) kd_inject_social(soc);
            if (eth > 0) ke_ethics_dose(eth);
            if (val != 0) {
                reap += val;
                if (reap > 0.2f) reap = 0.2f;
                if (reap < -0.2f) reap = -0.2f;
                if (reap_id == 0)
                    reap_id = (uint64_t)s_appr_seq | (1ULL << 32);
            }
            s_appr_seq++;             /* 消费完清奇偶(待施槽复位) */
            ESP_LOGI(TAG, "appraisal dose social=%+.3f ethics=%.3f val=%+.2f",
                     soc, eth, val);
        }
        if (reap_id != 0 && reap != 0) {
            struct ke_reappraisal r = { .delta = reap, .receipt_id = reap_id };
            if (ke_reappraisal_request(&r) == 0 && mood_hit) {
                ESP_LOGI(TAG, "mood reappraisal %+.2f", s_mood_req);
                soul_moodgate_record(s_mood_req);
            }
        }
        if (mood_hit)
            s_mood_seq++;             /* 消费完清奇偶(待施槽复位) */

        /* 5) 感受态(S2: 增 mood 慢层输入 drive_sat 与安抚 soothe) */
        struct ke_inputs kin = {
            .have_drive = true,
            .drive_error_sum = kd_out.error_sum,
            .drive_error_max = kd_out.error_max,
            .drive_sat = {
                [KD_SOCIAL] = kd_out.level[KD_SOCIAL],
                [KD_STIM] = kd_out.level[KD_STIM],
                [KD_REST] = kd_out.level[KD_REST],
            },
            .receipt = round_now ? (round_ok ? KE_RECEIPT_SUCCESS
                                              : KE_RECEIPT_FAIL)
                                 : KE_RECEIPT_NONE,
            .social_event = (evt != KD_EVT_NONE),
            .soothe_event = (evt == KD_EVT_TOUCH || (round_now && round_ok)),
        };
        ke_step(s_ticks, &kin, &ke_out);

        /* 6) 快照(短临界区写) + 黑板供给 + 消费出口 */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20))) {
            s_snap.drive = kd_out;
            s_snap.emo = ke_out;
            s_snap.tick = s_ticks;
            xSemaphoreGive(s_lock);
        }
        soul_think_day_flip((uint32_t)(s_ticks / 60));   /* S5: 配额日
            翻转每拍检测(旧 gate_ctx 职责; think_task 内仅触发时检查,
            触发间隔>24h 时会漏翻, 审计 P2-12) */
/* 引擎动量方向(上拍 Δvalence 符号; 三断法断三用) */
        {
            static float s_prev_val;
            static bool s_prev_seen;
            float dv = ke_out.state.valence - (s_prev_seen ? s_prev_val
                                                           : 0.0f);
            s_engine_trend = dv > 0.005f ? 1 : (dv < -0.005f ? -1 : 0);
            s_prev_val = ke_out.state.valence;
            s_prev_seen = true;
        }
        bb_feed(&kd_out, &ke_out);
        mood_publish(&s_snap, false);
        face_bg_select(&s_snap);        /* L1 底色(连续坐标+迟滞) */

#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
        /* 8) L4 思维闸(S5 重构): 动机面推进(sm_ctx/event/tick)→ 仲裁
         * sm_gate(显著性×动机偏置×习惯化+令牌桶+kb 礼貌闸, docs/24 §4.2)
         * —— 替换 ki_think_gate 固定 10min 节律(退役) */
        {
            /* kd sev 跃迁沿(任一驱力告警级变化, 调用方边沿检测) */
            static uint8_t s_prev_sev;
            /* 4 值枚举各占 2bit(<<2 拼接曾碰撞漏沿, S5 审计 P1-3) */
            uint8_t sev = (uint8_t)((kd_out.sev[KD_SOCIAL] << 4) |
                                    (kd_out.sev[KD_STIM] << 2) |
                                    kd_out.sev[KD_REST]);
            bool sev_jump = s_prev_sev != sev;
            s_prev_sev = sev;
            /* 长静默事件(每满 2h 一次, U=0.30); 用户活动即重置标记
             * (旧实现 mark 只增不减→交互后首事件延迟无界, 审计 P1-4) */
            static uint32_t s_longsil_mark;
            static uint64_t s_longsil_prev_active;
            if (s_last_active_tick != s_longsil_prev_active) {
                s_longsil_prev_active = s_last_active_tick;
                s_longsil_mark = 0;
            }
            uint32_t sil = (uint32_t)(s_ticks - s_last_active_tick);
            if (sil / 7200u > s_longsil_mark) {
                s_longsil_mark = sil / 7200u;
                sm_event(SM_EVT_LONG_SILENCE, (uint32_t)s_ticks);
            }
            sm_ctx(soul_bb_get(SOUL_BB_QUIET) > 0.5f,
                   s_ai_state != AI_IDLE,
                   s_player_state == PLAYER_ST_CONNECTING ||
                       s_player_state == PLAYER_ST_PLAYING);
            sm_tick((uint32_t)s_ticks);
            struct sm_gate_in gin = {
                .now_s = (uint32_t)s_ticks,
                .now_min = (uint32_t)(s_ticks / 60),
                .arousal = ke_out.state.arousal,
                .e_social = kd_out.error[KD_SOCIAL],
                .e_stim = kd_out.error[KD_STIM],
                .ai_active = (s_ai_state != AI_IDLE),
                .playing = (s_player_state == PLAYER_ST_CONNECTING ||
                            s_player_state == PLAYER_ST_PLAYING),
                .net_up = soul_bb_get(SOUL_BB_NET) > 0.5f,
                .quiet_hours = soul_bb_get(SOUL_BB_QUIET) > 0.5f,
                .kd_sev_jump = sev_jump,
                .mem_hook = false,     /* B-6 用户锚定时刻接线 */
                .thinking = soul_think_busy(),
            };
            struct sm_gate_out g;
            sm_gate(&gin, &g);
            if (g.trigger) {
                soul_think_fire(g.reason_mask, false,
                                g.S > 0.0f ? g.S : 0.6f);
                s_initiative_tick = true;   /* 自发发起(trait EMA 观测) */
            }
        }
#endif

        /* 9) 记忆(轮结束拍; 与快照同拍, 拿的是当拍前情绪——可接受) */
        if (round_now && ru[0]) {
            soul_mem_record(ru, rr, ke_out.state.valence,
                            ke_out.state.arousal);
        }


        /* 9.5) B-6b 反思触发(docs/24 §3.5): Σimp≥60 ∨ surprise≥40,
         * 24h 冷却; fire 忙则下拍重试(不计冷却) */
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
        if ((int32_t)(s_ticks - s_reflect_cd_until) >= 0 &&
            (soul_mem_imp_pending() >= 60 ||
             sm_surprise_pending() >= 40.0f)) {
            if (soul_think_reflect_fire()) {
                s_reflect_cd_until = s_ticks + 86400;
                ESP_LOGI(TAG, "REFLECT fired (imp=%lu sur=%.0f)",
                         (unsigned long)soul_mem_imp_pending(),
                         sm_surprise_pending());
            }
        }
#endif
        /* 10) trait 慢变量(S2.5b 双通道, docs/24 §3.3): 每拍残差观测
         * (initiative=本拍深思行动成立), 日界结算 EMA+熔断 */
        struct trait_obs tobs = {
            .arousal = ke_out.state.arousal,
            .salience = soul_bb_get(SOUL_BB_SALIENCE),
            .urgency = ke_out.state.urgency,
            .social_bond = ke_out.state.social_bond,
            .valence = ke_out.state.valence,
            .initiative = s_initiative_tick,
        };
        s_initiative_tick = false;
        trait_stat_observe(&tobs);
        if (s_ticks % 86400 == 0) {
            float after[TRAIT_DIM];
            trait_ema_settle();
            trait_get(after);
            bool moved = false;
            for (int d = 0; d < TRAIT_DIM; d++) {
                if (trait_last_ema(d) != 0.0f) moved = true;
                if (trait_frozen(d))
                    ESP_LOGW(TAG, "trait dim %d frozen 7d (7日净移熔断)", d);
            }
            if (moved) {
                ESP_LOGI(TAG, "trait evolve C=%.3f W=%.3f E=%.3f A=%.3f "
                         "R=%.3f (day %u)", after[0], after[1], after[2],
                         after[3], after[4], trait_day_count());
                /* 持久化(B-5): persona owner(act_gate)订阅落盘 */
                cube_trait_evo_t evo;
                for (int d = 0; d < TRAIT_DIM; d++) {
                    evo.trait[d] = after[d];
                    evo.applied[d] = trait_last_ema(d);
                }
                snprintf(evo.reason, sizeof(evo.reason),
                         "EMA day %lu",
                         (unsigned long)trait_day_count());
                cube_event_publish(CUBE_EVT_TRAIT_EVO, &evo, sizeof(evo));
            }
        }
    }
}

esp_err_t soul_start(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;

    soul_mem_boot();
    sb_reset();                    /* S3: 行动预算分账(R6 零生成, 只执法) */
    sm_reset();                    /* S5: 四维动机+仲裁器+令牌桶复位
                                      (缺调用=s_cfg 全零→门全开, 板上
                                      THINK reason=1 score=0.35 实证) */
    soul_moodgate_reset();         /* S6: 回写三断法状态清零 */
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(cube_event_subscribe(soul_evt_cb, NULL),
                        TAG, "evt subscribe");
    /* 栈预分配(docs/22 §3B): RTCRAM 定置(1Hz 低频任务, 非 DMA/非 PIE,
     * v6.1 释放 HP DRAM)。4KB: 3KB 时 panic 转储实证帧底距栈界仅 ~20B,
     * 向下溢出砸穿相邻 cli 栈上下文区(2026-11-18 Instruction access
     * fault MEPC=0, 静态相邻摆放的固有害); v5 动态期同深度砸的是堆 */
    static StaticTask_t s_soul_tcb;
    static StackType_t s_soul_stack[4096 / sizeof(StackType_t)]
        __attribute__((section(".rtc.bss")));
    if (xTaskCreateStatic(soul_task, "soul", 4096, NULL, 4,
                          s_soul_stack, &s_soul_tcb) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "soul up (1Hz, percep→kd→ke→bb→budget/think)");
    return ESP_OK;
}

#endif /* CONFIG_ESP_CUBE_ENABLE_SOUL */
