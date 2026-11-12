/*
 * soul.c —— 灵魂编排: 1Hz tick(kd→ke) + 事件漏斗 + prompt/消费出口
 *
 * 移植自 Kaleido tools/hil/convo-stage/main/soul.c(W0-3), 适配 EspCube:
 *   - 会话状态经 CUBE_EVT_AI_STATE 总线缓存(不直接依赖 ai_session);
 *   - 播放器状态经 CUBE_EVT_PLAYER_STATE 缓存(quiet = idle 且无播放);
 *   - 消费出口: prompt suffix / tts hint / CUBE_EVT_SOUL_MOOD(节流)。
 * kd/ke 为纯函数组件(零平台依赖), 本层持有唯一调用权(单写者纪律)。
 */
#include "soul.h"
#include "soul_drive.h"
#include "soul_emotion.h"

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

/* ---- 事件漏斗(置位任意任务, 消费唯 tick) ---- */
#define PEND_ROUND_OK   0x01
#define PEND_ROUND_FAIL 0x02
#define PEND_BARGE      0x04
#define PEND_VOICE      0x08
static volatile uint8_t s_pend;
/* round 附带文本(单槽, 新覆盖旧 —— 轮结束才写, 会话任务上下文) */
static char s_round_user[192];
static char s_round_reply[96];

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

/* soul_mem 弱耦合: 同组件内另一文件(编译期一体, 免头依赖环) */
extern void soul_mem_record(const char *user, const char *reply,
                            float valence, float arousal);
extern void soul_mem_boot(void);

/* ---- 总线缓存 ---- */
static void soul_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                        void *ctx)
{
    (void)len; (void)ctx;
    if (id == CUBE_EVT_AI_STATE && data) {
        s_ai_state = *(const ai_state_t *)data;
        if (s_ai_state == AI_LISTENING) {
            /* 唤醒进入聆听 = 强社交在场信号(等价 Kaleido VOICE 拍) */
            soul_on_voice();
        }
    } else if (id == CUBE_EVT_PLAYER_STATE && data) {
        s_player_state = *(const int *)data;
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

/* ---- 1Hz 灵魂任务(单写者: kd/ke 唯一调用权) ---- */
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

        /* 事件消费(每拍至多一个, 优先级 BARGE > ROUND > VOICE) */
        uint8_t p = s_pend;
        enum kd_event evt = KD_EVT_NONE;
        char ru[192], rr[96];
        ru[0] = rr[0] = '\0';
        bool round_now = false;
        bool round_ok = false;
        if (p & PEND_BARGE) {
            s_pend &= ~PEND_BARGE;
            evt = KD_EVT_BARGE;
        } else if (p & PEND_ROUND_OK) {
            s_pend &= ~PEND_ROUND_OK;
            evt = KD_EVT_ROUND_OK;
            round_now = round_ok = true;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_ROUND_FAIL) {
            s_pend &= ~PEND_ROUND_FAIL;
            evt = KD_EVT_ROUND_FAIL;
            round_now = true;
            round_ok = false;
            strlcpy(ru, s_round_user, sizeof(ru));
            strlcpy(rr, s_round_reply, sizeof(rr));
        } else if (p & PEND_VOICE) {
            s_pend &= ~PEND_VOICE;
            evt = KD_EVT_VOICE;
        }

        /* 驱力: quiet = AI idle 且播放器停/电台待机(rest 恢复口径) */
        bool quiet = (s_ai_state == AI_IDLE) && (s_player_state == 0);
        if (s_ai_state != AI_IDLE) s_last_active_tick = s_ticks;
        struct kd_inputs din = {
            .tick = s_ticks,
            .quiet = quiet,
            .charging = false,
            .event = evt,
        };
        kd_step(s_ticks, &din, &kd_out);

        /* mood 重评价(先于 ke_step: 本拍情绪即受影响) */
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

        /* 感受态 */
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

        /* 快照(短临界区写) */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20))) {
            s_snap.drive = kd_out;
            s_snap.emo = ke_out;
            s_snap.tick = s_ticks;
            xSemaphoreGive(s_lock);
        }
        mood_publish(&s_snap, false);

        /* 记忆(轮结束拍; 与快照同拍, 拿的是当拍前情绪——可接受) */
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
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(cube_event_subscribe(soul_evt_cb, NULL),
                        TAG, "evt subscribe");
    if (xTaskCreate(soul_task, "soul", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "soul up (1Hz, kd→ke)");
    return ESP_OK;
}

#endif /* CONFIG_ESP_CUBE_ENABLE_SOUL */
