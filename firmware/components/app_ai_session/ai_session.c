/*
 * ai_session.c —— AI 云端会话编排(百炼组合 C: ASR→LLM→TTS)
 *
 * 移植自 Kaleido kaleido_speech 的编排经验(V-33 真机实测):
 *   - ASR: ks_asr_open/feed/finish(WS 双向流, 100ms 分片上行);
 *     sentence_end 聚合定稿, 定稿即转 LLM
 *   - LLM: ks_llm_chat 阻塞流式, 句读(，。？！)即回调——回调与
 *     LLM 同线程, 直接 ks_tts_feed(DashScope continue-task 续喂),
 *     服务端自动分句 → 首句即播, 端到端预算 ~1.6-2.1s(V-33 §5)
 *   - TTS: ks_tts_open/feed(last=true 收口), PCM 下行 → 重采样
 *     48k stereo → mixer "tts" 源(ducking 发起者)
 *
 * 线程模型:
 *   session_task —— 会话编排(LLM 阻塞调用也在此任务)
 *   mic_task     —— LISTENING 期采集(audio_mic_read_16k → ASR feed)
 *   tts_task     —— TTS PCM 环形缓冲 → 重采样 → mixer(阻塞 pacing);
 *                   兼任排空检测: 缓冲空 && TTS 已收口 → EVT_TTS_DONE
 *   ks_ 回调在网络任务上下文, 一律投队列/写环形缓冲, 不持锁不阻塞
 *
 * 触发: 自订阅 CUBE_EVT_WAKE_DETECTED(app_voice 唤醒词)或直调
 *       ai_session_on_wake()。
 * 打断: SPEAKING/LISTENING 中再次唤醒 → abort → 重开 LISTENING。
 * 麦克风所有权(docs/06 §3): 本组件只在 LISTENING 读麦克风,
 * 其余时间由 app_voice 常开(WakeNet barge-in)。
 */
#include "ai_session.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "audio_mic.h"
#include "speech_provider.h"
#include "ai_skills.h"
#include "soul.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CUBE_ENABLE_AI

/* VOICE/AI 关闭: 空实现(仅保启动接口可用) */
esp_err_t ai_session_start(void) { return ESP_OK; }
void ai_session_on_wake(void) {}
esp_err_t ai_session_submit_text(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ai_session_announce(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

#else

static const char *TAG = "ai_session";

/* 前向声明(TTS 配置单例先于回调定义使用) */
static void on_tts_pcm(void *user, const uint8_t *pcm, size_t len);
static void on_tts_err(void *user, int stage, int code);

/* ---- 云端配置(百炼, V-33 全功能真机验证) ----
 * ⚠ 只填主机名: provider(sp_dashscope)会自动拼 /api-ws/v1/inference。
 * 曾误填完整端点 → 双路径 URL → 网关多数节点容忍、少数严格节点报
 * InvalidParameter: url error(2026-11-12 harness 定位, ASR/TTS 腿
 * 间歇失败的总根源) */
#define AI_DASHSCOPE_URL   "wss://dashscope.aliyuncs.com"
/* LLM 端点可配置(Kconfig): 任意 OpenAI 兼容厂家(DeepSeek/Kimi/智谱/
 * MiniMax/火山方舟), 五家链根均在 IDF 默认证书束(2026-11-12 验证) */
#define AI_LLM_URL          CONFIG_ESP_CUBE_AI_LLM_URL
#define AI_MIC_RATE        16000
#define AI_TTS_RATE        CONFIG_ESP_CUBE_AI_TTS_RATE

/* ---- 能量 VAD 断句 ----
 * 尾静音 500ms(2026-11-12 由 900 压缩, LiveKit/Picovoice 端点口径
 * 300-500ms; 首响预算 -0.4s)。误断保护: 短促人声+1.8s 长静音兜底仍在 */
#define VAD_SILENCE_MS     500
#define VAD_MIN_SPEECH_MS  400
#define VAD_RMS_THRESH     350        /* ES8311 增益标定后调 */
#define LISTEN_MAX_MS      15000

/* ---- 状态 ---- */
static ai_state_t s_state = AI_IDLE;
static QueueHandle_t s_evtq;
static TaskHandle_t s_task, s_mic_task, s_tts_task;
/* ASR 定稿暂存: 512B + UTF-8 边界截断(串台/长句超 128B 时截在中文
 * 中间 → LLM 请求体非法 UTF-8 → 400, 2026-11-11 板上实测) */
static char s_last_asr[512];

/* TTS 播放桥: 网络回调 → 环形缓冲(mono 16k) → 重采样 → mixer。
 * 二次幂容量, 索引运算走掩码(免逐样本取模)。 */
#define TTS_RING_SHIFT     16
#define TTS_RING_FRAMES    (1 << TTS_RING_SHIFT)   /* 65536 ≈ 4.1s@16k */
#define TTS_RING_MASK      (TTS_RING_FRAMES - 1)
static int16_t *s_ring;
static volatile size_t s_ring_r, s_ring_w;    /* 单写单读 */
static cube_resampler_t s_rs;
static audio_source_handle_t s_tts_src;
static volatile bool s_abort_req;
static volatile bool s_tts_closed;            /* TTS 会话已收口(排空判定) */
static bool s_tts_opened;                     /* TTS 懒开(见 on_llm_sent) */
static volatile bool s_tts_dead;              /* 本轮 TTS 已失败(不再喂/不再等) */
static volatile bool s_round_spoken;          /* 本轮已有句子成功进 TTS */
static char s_round_reply1st[96];             /* 回复首句(soul 记忆/回执) */
static volatile bool s_tts_armed;             /* 排空上报武装(run_round 收口置位) */
static int64_t  s_tts_armed_us;               /* 武装时刻(超时兜底) */
static int64_t s_listen_start_us;             /* 唤醒防抖基准 */
static volatile bool s_filler_req;            /* tts_task 待播 filler */
static bool s_announce_pend;                  /* 会话忙时挂起的带外播报 */
static char s_announce_text[176];
/* selftest 采集(见文件尾 st_*): on_tts_pcm 采集优先, TTS→注入素材 */
static int16_t *s_st_buf;                      /* PSRAM, 首用分配 */
static volatile bool s_st_capture;
static size_t s_st_w, s_st_cap;
static volatile bool s_st_pending;             /* 注入待武装(见 EVT_WAKE) */
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
/* ---- 快线(qwen-omni realtime): 嘴和耳, 工具可靠性在慢线 ----
 * 双路上行: 同一麦克流同时喂 paraformer(慢线精确转写)与 omni(快线
 * 即兴应答); omni 音频(24k)入专用环形, 由 tts_task 泵出(24k→48k);
 * 慢线技能回执经 ks_rt_say 注入快线出声(单一 omni 音色)。 */
static volatile bool s_rt_spoken;        /* 本轮快线已出声 */
static int16_t *s_rt_ring;               /* 24k mono(PSRAM) */
#define RT_RING_SHIFT   17               /* 131072 帧 ≈ 5.5s@24k */
#define RT_RING_FRAMES  (1 << RT_RING_SHIFT)
#define RT_RING_MASK    (RT_RING_FRAMES - 1)
static volatile size_t s_rt_r, s_rt_w;
static uint32_t s_rt_drops;
static cube_resampler_t s_rt_rs;         /* 24k mono → 48k stereo */
static bool s_rt_rs_init;
static struct ks_rt_cfg s_rt_cfg = {
    .url = CONFIG_ESP_CUBE_AI_RT_URL "?model=" CONFIG_ESP_CUBE_AI_RT_MODEL,
    .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
    .voice = CONFIG_ESP_CUBE_AI_RT_VOICE,
    .instructions = "你是音箱的语音伙伴, 回应热情简短自然。用户想听歌/"
                    "点歌时就说马上去挑之类的热情话, 但不许编造具体歌名,"
                    "也不许说已经放好了(你不负责放歌)。闲聊就自然接话。",
    .transcript = true,
};
static void on_rt_audio(void *user, const uint8_t *pcm24k, size_t len);
static void on_rt_transcript(void *user, const char *text);
static struct ks_rt_events s_rt_ev = {
    .user = NULL, .on_audio = on_rt_audio,
    .on_transcript = on_rt_transcript,
};
#endif /* RT */

/* 本地 filler("好的，正在为您找歌", cosyvoice 预合成 16k mono, 构建期
 * EMBED 嵌入 flash): 工具调用确定即播, 遮蔽搜歌网络时延 —— 用户感知
 * 零等待(GetStream filler 模式, 2026-11-12) */
extern const uint8_t filler_16k_pcm_start[] asm("_binary_filler_16k_pcm_start");
extern const uint8_t filler_16k_pcm_end[]   asm("_binary_filler_16k_pcm_end");
/* TTS 音色热切缓冲(音色甄选台调/persona 配置热更新): ks_tts_cfg.voice
 * 是指针, 指向本可写数组即可运行时切换 */
static char s_tts_voice_buf[48] = CONFIG_ESP_CUBE_AI_TTS_VOICE;
static struct ks_tts_cfg s_tts_cfg = {        /* TTS 配置单例(懒开复用) */
    .url = AI_DASHSCOPE_URL,
    .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
    .model = CONFIG_ESP_CUBE_AI_TTS_MODEL,
    .voice = s_tts_voice_buf,
    .sample_rate = AI_TTS_RATE,
    .speech_rate = 0, .pitch = 0,
};
static struct ks_tts_events s_tts_ev = {
    .user = NULL, .on_pcm = on_tts_pcm, .on_error = on_tts_err,
};

/* 事件 */
enum {
    EVT_WAKE = 1,
    EVT_ASR_TEXT,          /* payload: char*(堆, 消费方释放) */
    EVT_MIC_DONE,
    EVT_TTS_DONE,          /* TTS 缓冲排空且已收口(tts_task 发) */
    EVT_TEXT_IN,           /* payload: char*(文本注入, 跳过 ASR 直进 LLM) */
    EVT_ANNOUNCE,          /* payload: char*(带外播报, M2 提醒/主动交互) */
};

/* 队列元素(带指针的事件) */
typedef struct {
    int id;
    void *payload;
} session_evt_t;

static void set_state(ai_state_t st)
{
    if (s_state == st) return;
    /* ducking 接线(mixer 现成能力, 此前无人调用=死代码): AI 说话期
     * NET/BT 源压 -14dB, 否则点歌后音乐全音量盖住人声 */
    audio_mixer_set_ducking(st == AI_SPEAKING);
    s_state = st;
    cube_event_publish(CUBE_EVT_AI_STATE, &st, sizeof(st));
}

/* TTS 音色热切(音色甄选台调 CLI / persona 配置): 断当前会话令下轮
 * 懒开握手新音色; announce 带外播报同走 s_tts_cfg, 一并生效 */
void ai_session_set_tts_voice(const char *voice)
{
    if (voice == NULL || voice[0] == '\0') return;
    strlcpy(s_tts_voice_buf, voice, sizeof(s_tts_voice_buf));
    ks_tts_close();               /* 懒开: 下轮重连用新音色 */
    ESP_LOGI(TAG, "tts voice -> %s", s_tts_voice_buf);
}

const char *ai_session_tts_voice(void)
{
    return s_tts_voice_buf;
}

/* TTS open 带重试: ASR+TTS 双 WS 并发握手偶发抢不到 esp-aes 长度描述符
 * (2026-11-11 板上实测 Failed to allocate memory for len descriptor →
 * TTS 连接超时); 间隔 2s 重试一次即可让路 ASR 握手完成 */
static int tts_open_retry(const struct ks_tts_cfg *cfg,
                          const struct ks_tts_events *ev)
{
    int rc = ks_tts_open(cfg, ev);
    if (rc != 0) {
        ESP_LOGW(TAG, "tts open rc=%d, retry in 2s", rc);
        vTaskDelay(pdMS_TO_TICKS(2000));
        rc = ks_tts_open(cfg, ev);
    }
    return rc;
}

/* ASR open 带重试: DashScope 网关偶发拒绝(见 AI_DASHSCOPE_URL 注释),
 * 与 TTS 同款间隔重试 */
static int asr_open_retry(const struct ks_asr_cfg *cfg,
                          const struct ks_asr_events *ev)
{
    int rc = ks_asr_open(cfg, ev);
    if (rc != 0) {
        ESP_LOGW(TAG, "asr open rc=%d, retry in 2s", rc);
        vTaskDelay(pdMS_TO_TICKS(2000));
        rc = ks_asr_open(cfg, ev);
    }
    return rc;
}

/* ---- ASR 定稿回调(网络任务) ---- */
static void on_asr_text(void *user, const char *sentence, bool final)
{
    (void)user;
    if (!final || s_abort_req) return;
    session_evt_t e = { .id = EVT_ASR_TEXT,
                        .payload = strdup(sentence) };
    if (e.payload == NULL ||
        xQueueSend(s_evtq, &e, 0) != pdPASS) {
        free(e.payload);
    }
}

/* ---- TTS 文本清洗: cosyvoice 拒绝含 URL 的文本(task-failed:
 * InvalidParameter: url error, 2026-11-11 板上实测) —— LLM 回复
 * 可能复述技能回执里的链接, 喂 TTS 前剥掉 http(s)://… 段 ---- */
static void tts_sanitize(char *s)
{
    char *p = strstr(s, "http");
    while (p != NULL) {
        char *q = p;
        /* URL 终点 = 空白/控制符/多字节首字节(中文标点) */
        while (*q != '\0' && (uint8_t)*q > 0x20 && (uint8_t)*q < 0x80) q++;
        memmove(p, q, strlen(q) + 1);
        p = strstr(p, "http");
    }
}

/* ---- LLM 句读回调(与 ks_llm_chat 同线程): 直接喂 TTS ---- */
static bool on_llm_sent(void *user, const char *sentence, bool final)
{
    (void)user;
    if (s_abort_req || s_tts_dead) return false;
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    if (ks_rt_ok()) {
        /* 快线在位: 闲聊由 omni 即兴自答, 慢线文本只上屏不重复出声 */
        if (sentence[0]) {
            cube_event_publish(CUBE_EVT_AI_TEXT, sentence,
                               strlen(sentence) + 1);
        }
        return true;
    }
#endif
    /* TTS 懒开: 唤醒路径下 ASR 已收线, 此刻开 TTS 免双 TLS 并发
     * (esp-aes DMA 描述符争抢, 2026-11-11 板上实测互撞超时) */
    if (!s_tts_opened) {
        if (tts_open_retry(&s_tts_cfg, &s_tts_ev) != 0) {
            ESP_LOGE(TAG, "tts lazy open failed");
            s_tts_dead = true;                  /* 本轮放弃语音回应 */
            return false;
        }
        s_tts_opened = true;
    }
    if (sentence[0] != '\0') {
        char clean[512];
        strlcpy(clean, sentence, sizeof(clean));
        soul_strip_mood_tag(clean);           /* mood 标签: 回写灵魂, 不上屏不出声 */
        tts_sanitize(clean);
        cube_event_publish(CUBE_EVT_AI_TEXT, clean, strlen(clean) + 1);
        if (s_round_reply1st[0] == '\0') {
            strlcpy(s_round_reply1st, clean, sizeof(s_round_reply1st));
        }
        ESP_LOGI(TAG, "llm sentence(final=%d): %s", final, clean);  /* DIAG */
        /* 永远只发 continue-task(last=false): v2 的 final=true 表示
         * "本次 chat 调用结束", 带工具的轮后面还有第二轮 LLM ——
         * 若在此发 finish-task, 第二轮文本将喂给已结束的任务,
         * TTS 整轮死亡(2026-11-12 定位: 语音路径必挂/ask 路径偶通的
         * 差异根因)。全轮唯一收口在 run_round 尾部 */
        int rc = clean[0] ? ks_tts_feed(clean, false, 8000) : 0;
        if (rc != 0) {
            /* 会话已死(如服务端 task-failed): 标记本轮, 后续句不再重试 */
            ESP_LOGW(TAG, "tts feed rc=%d (dead this round)", rc);
            s_tts_dead = true;
        } else {
            s_round_spoken = true;
        }
        if (s_state != AI_SPEAKING) set_state(AI_SPEAKING);
    }
    return true;
}

/* ---- 工具调用回调(v2, 与调用同线程): 暂存到 user 指向的结构 ---- */
typedef struct {
    char id[96];
    char name[96];
    char args[512];
} tool_call_t;

static void on_llm_tool(void *user, const char *id, const char *name,
                        const char *arguments)
{
    tool_call_t *t = user;
    strlcpy(t->id, id ? id : "", sizeof(t->id));
    strlcpy(t->name, name ? name : "", sizeof(t->name));
    strlcpy(t->args, arguments ? arguments : "{}", sizeof(t->args));
}

/* ---- 工具参数流式回调(SSE 读循环线程! 只许置标志+发事件) ----
 * filler: 点歌类长动作提示音即刻起播(tts_task 泵出);
 * TOOL_HINT 事件: 技能层 speculative 预取(L5 订阅) */
static void on_llm_args(void *user, const char *name, const char *args)
{
    (void)user;
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    if (!ks_rt_ok())                /* 快线即兴应答替代本地 filler */
#endif
    if (strcmp(name, "music.search_play") == 0) {
        s_filler_req = true;
    }
    cube_ai_tool_hint_t h = { 0 };
    strlcpy(h.name, name, sizeof(h.name));
    strlcpy(h.args, args, sizeof(h.args));
    cube_event_publish(CUBE_EVT_AI_TOOL_HINT, &h, sizeof(h));
    ESP_LOGI(TAG, "tool hint: %s %s", name, args);
}

/* ---- TTS PCM 回调(网络任务) → 环形缓冲。
 * 环满即丢弃并计数: 播放跟不上时旧数据已无意义, 且网络回调里
 * 自旋等待会卡死 WS 客户端任务(旧实现最多阻塞 2s, 已废除)。 ---- */
static void on_tts_pcm(void *user, const uint8_t *pcm, size_t len)
{
    (void)user;
    const int16_t *in = (const int16_t *)pcm;
    if (s_st_capture) {                       /* selftest 合成采集 */
        size_t frames = len / 2;
        if (frames > s_st_cap - s_st_w) frames = s_st_cap - s_st_w;
        memcpy(s_st_buf + s_st_w, in, frames * 2);
        s_st_w += frames;
        return;
    }
    for (size_t i = 0; i < len / 2; i++) {
        size_t next = (s_ring_w + 1) & TTS_RING_MASK;
        if (next == s_ring_r) {
            static uint32_t drop_cnt;
            if ((drop_cnt++ & 0x3F) == 0) {
                ESP_LOGW(TAG, "tts ring full, dropped x%u",
                         (unsigned)(drop_cnt + 63));
            }
            return;                       /* 丢弃剩余整段 */
        }
        s_ring[s_ring_w] = in[i];
        s_ring_w = next;
    }
}

static void on_tts_err(void *user, int stage, int code)
{
    (void)user;
    ESP_LOGW(TAG, "tts err stage=%d code=%d", stage, code);
    s_tts_dead = true;    /* WS 任务上下文: 掐断本轮续喂, 收尾走兜底 */
}

#if CONFIG_ESP_CUBE_AI_RT_ENABLE
/* 快线音频(WS 任务上下文): 只写环形 + 状态推进, 不做重活 */
static void on_rt_audio(void *user, const uint8_t *pcm24k, size_t len)
{
    (void)user;
    if (s_rt_ring == NULL) return;
    const int16_t *in = (const int16_t *)pcm24k;
    size_t frames = len / 2;
    for (size_t i = 0; i < frames; i++) {
        size_t next = (s_rt_w + 1) & RT_RING_MASK;
        if (next == s_rt_r) {            /* 满: 丢整段保最新节奏 */
            if ((s_rt_drops++ & 0x3F) == 0) {
                ESP_LOGW(TAG, "rt ring full, dropped x%u",
                         (unsigned)(s_rt_drops + 63));
            }
            return;
        }
        s_rt_ring[s_rt_w] = in[i];
        s_rt_w = next;
    }
    if (!s_rt_spoken) {
        ESP_LOGI(TAG, "rt audio first frame");
    }
    s_rt_spoken = true;
    /* 状态推进交给 tts_task(泵出时): WS 任务上下文不做总线发布
     * (板上 IWDT 嫌疑排除项, 2026-11-12) */
}

static void on_rt_transcript(void *user, const char *text)
{
    (void)user;
    /* 快线转写可靠性弱(PC 探针实测), 仅作 UI/日志参考 */
    ESP_LOGI(TAG, "rt transcript: %s", text);
}

/* 快线保障: 失联即整体重建(半死会话不如重开); 失败回落纯级联 */
static void rt_ensure(void)
{
    if (ks_rt_ok()) return;
    ks_rt_close();
    if (s_rt_ring == NULL) return;
    if (ks_rt_open(&s_rt_cfg, &s_rt_ev) == 0) {
        ESP_LOGI(TAG, "rt fast-line up (%s)", CONFIG_ESP_CUBE_AI_RT_VOICE);
    } else {
        ESP_LOGW(TAG, "rt fast-line down -> cascade fallback");
    }
}
#endif /* RT */

/* ---- TTS 播放任务: 环形缓冲 → 重采样 48k stereo → mixer;
 * 兼任排空检测: run_round 收口武装(s_tts_armed)后, 缓冲排空即上报
 * EVT_TTS_DONE。原实现要求"缓冲曾有过数据"才上报 —— TTS 服务端
 * task-failed 零 PCM 时 EVT_TTS_DONE 永不触发, 会话卡死在
 * SPEAKING(2026-11-11 实测), 故改为武装制 + 10s 超时兜底。 ---- */
static void tts_task(void *arg)
{
    (void)arg;
    int16_t in_mono[320];                        /* 20ms@16k */
    int16_t out48[320 * 3 * 2];
    static cube_resampler_t filler_rs;           /* filler 独立重采样器 */
    static bool filler_rs_init;
    while (1) {
        /* filler 泵出: 嵌入 PCM 16k mono → 48k stereo → mixer(tts 源,
         * ducking 让位)。abort 即断 */
        if (s_filler_req) {
            if (!filler_rs_init) {
                cube_resampler_init(&filler_rs, 16000, 48000);
                filler_rs_init = true;
            }
            const int16_t *f = (const int16_t *)filler_16k_pcm_start;
            size_t total = (filler_16k_pcm_end - filler_16k_pcm_start) / 2;
            size_t pos = 0;
            ESP_LOGI(TAG, "filler play (%.2fs)",
                     (double)total / 16000.0);
            while (pos < total && !s_abort_req) {
                size_t n = total - pos;
                if (n > 320) n = 320;
                size_t of = cube_resampler_process_mono_to_stereo(
                        &filler_rs, f + pos, n, out48,
                        sizeof(out48) / sizeof(out48[0]) / 2);
                if (of > 0) {
                    audio_mixer_source_write(s_tts_src, out48,
                                             of * 2 * sizeof(int16_t),
                                             pdMS_TO_TICKS(100));
                }
                pos += n;
            }
            s_filler_req = false;
            continue;
        }
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
        /* 快线音频泵出: 24k mono → 48k stereo(独立重采样器)。
         * 不按 SPEAKING 门控 —— omni 可能在聆听尾即开始应答 */
        if (s_rt_ring != NULL && s_rt_r != s_rt_w) {
            if (s_state == AI_THINKING) {
                set_state(AI_SPEAKING);   /* WS 侧只置标志, 此处推进 */
            }
            if (!s_rt_rs_init) {
                cube_resampler_init(&s_rt_rs, 24000, 48000);
                s_rt_rs_init = true;
            }
            /* 复用本任务的 in_mono/out48 栈缓冲(320 帧一块):
             * 320@24k → 640stereo@48k, 输出容量 960stereo 富余;
             * 曾另开 2.8KB 缓冲直接打爆 aitts 栈(2026-11-12 板上
             * Stack protection fault 启动循环) */
            int n = 0;
            while (n < 320 && s_rt_r != s_rt_w) {
                in_mono[n++] = s_rt_ring[s_rt_r];
                s_rt_r = (s_rt_r + 1) & RT_RING_MASK;
            }
            size_t of = cube_resampler_process_mono_to_stereo(
                    &s_rt_rs, in_mono, n, out48,
                    sizeof(out48) / sizeof(out48[0]) / 2);
            if (of > 0 && !s_abort_req) {
                audio_mixer_source_write(s_tts_src, out48,
                                         of * 2 * sizeof(int16_t),
                                         pdMS_TO_TICKS(500));
            }
            continue;      /* 快线优先, 排空再回 tts/filler */
        }
#endif /* RT */
        /* 排空上报: 已收口且缓冲排空 → 本轮语音播完; 若 15s 仍未排空
         * (PCM 流中途停滞) 也强制收口, 不让会话卡死在 SPEAKING。
         * ⚠ 双层水位: 生产者环(s_ring)排空 ≠ 播完 —— mixer 源环里还有
         * 存量(网络 PCM 快于实时时积压数秒), 必须等混音环也近排空再报
         * DONE, 否则 ducking 提前解除 → 电台叠语音尾巴 + 突变噪音
         * (2026-11-12 用户实测)。余量 4KB≈20ms@48kstereo, 人耳无感 */
        if (s_tts_armed && s_tts_closed && !s_abort_req &&
            s_state == AI_SPEAKING &&
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            (s_rt_ring == NULL || s_rt_r == s_rt_w) &&   /* 快线也排空 */
#endif
            (s_ring_r == s_ring_w ||
             esp_timer_get_time() - s_tts_armed_us > 15000000) &&
            audio_mixer_source_buffered(s_tts_src) <= 4096) {
            if (s_ring_r != s_ring_w) {
                ESP_LOGW(TAG, "tts drain timeout, force done");
                s_ring_r = s_ring_w;
            }
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            if (s_rt_ring != NULL && s_rt_r != s_rt_w) {
                s_rt_r = s_rt_w;
            }
#endif
            session_evt_t e = { .id = EVT_TTS_DONE };
            xQueueSend(s_evtq, &e, 0);
            s_tts_armed = false;
        }
        if (s_ring_r == s_ring_w) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_state != AI_SPEAKING || s_abort_req) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int n = 0;
        while (n < 320 && s_ring_r != s_ring_w) {
            in_mono[n++] = s_ring[s_ring_r];
            s_ring_r = (s_ring_r + 1) & TTS_RING_MASK;
        }
        size_t of = cube_resampler_process_mono_to_stereo(
            &s_rs, in_mono, n, out48, sizeof(out48) / sizeof(out48[0]) / 2);
        if (of > 0 && !s_abort_req) {
            audio_mixer_source_write(s_tts_src, out48,
                                     of * 2 * sizeof(int16_t),
                                     pdMS_TO_TICKS(500));
        }
    }
}

/* ---- 录音任务: mic 16k 单声道(L2 服务) → ASR feed; 能量 VAD 断句 ----
 * 阈值自适应(2026-11-11): 麦克增益提到 36dB 后固定阈值 350 会被底噪/
 * 喇叭漏声单块击穿 → had_speech 误置 → 假断句送空文本。改为开场 300ms
 * 采噪声底, 阈值 = max(固定, 噪声底×3); 且 had_speech 要求连续语音
 * ≥VAD_MIN_SPEECH_MS 才置位(单块毛刺不算"说过话"), 全程无人声 6s
 * 放弃本轮(MIC_DONE + 空 s_last_asr → 会话作废回 IDLE)。 ---- */
static void mic_task(void *arg)
{
    (void)arg;
    int16_t pcm[320];                            /* 20ms@16k 单声道 */
    while (1) {
        vTaskSuspend(NULL);                      /* 由 session 唤醒 */
        int silence_ms = 0, speech_ms = 0, total_ms = 0;
        int64_t noise_acc = 0;
        int noise_n = 0;
        int thresh = VAD_RMS_THRESH;
        while (s_state == AI_LISTENING && !s_abort_req) {
            int n = audio_mic_read_16k(pcm, 320);
            if (n <= 0) continue;
            int64_t sq = 0;
            for (int i = 0; i < n; i++) sq += (int64_t)pcm[i] * pcm[i];
            int rms = (int)(sq / n);
            ks_asr_feed(pcm, n);
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            if (ks_rt_ok()) ks_rt_feed(pcm, n);  /* 快线同流上行 */
#endif
            total_ms += 20;

            if (noise_n < 15) {                  /* 前 300ms: 噪声底标定 */
                noise_acc += rms;
                noise_n++;
                if (noise_n == 15) {
                    int floor_rms = (int)(noise_acc / noise_n);
                    if (floor_rms * 3 > thresh) {
                        thresh = floor_rms * 3;
                        ESP_LOGI(TAG, "vad thresh=%d (floor=%d)",
                                 thresh, floor_rms);
                    }
                }
                continue;
            }
            if (rms > thresh) {
                speech_ms += 20;
                silence_ms = 0;
            } else if (speech_ms > 0) {
                silence_ms += 20;
            }
            /* 断句: 完整语音(≥400ms)+尾静音900ms; 短促人声+1.8s 静音;
             * 全程无人声 6s 放弃; 15s 硬上限 */
            if ((speech_ms >= VAD_MIN_SPEECH_MS && silence_ms > VAD_SILENCE_MS) ||
                (speech_ms > 0 && silence_ms > VAD_SILENCE_MS * 2) ||
                total_ms > LISTEN_MAX_MS ||
                (speech_ms == 0 && total_ms > 6000)) {
                if (speech_ms == 0) {
                    ESP_LOGW(TAG, "vad: no speech in %dms, drop round",
                             total_ms);
                }
                ks_asr_finish(3000);             /* flush 尾句(阻塞) */
                ks_asr_close();
                if (!s_abort_req) {
                    session_evt_t e = { .id = EVT_MIC_DONE };
                    xQueueSend(s_evtq, &e, 0);
                }
                break;                           /* 回挂起, 等下轮 */
            }
        }
    }
}

/* ---- LLM→(技能)→TTS 一轮编排(session_task 内执行; 语音/文本注入共用) ----
 * function calling 循环(≤2 轮工具): 文本照旧流式进 TTS; 模型发起
 * tool_calls → ai_skill_exec 执行 → role:"tool" 回传 → 模型用结果
 * 生成最终答复(仍流式进 TTS)。 */
static void run_round(const char *user_text)
{
    set_state(AI_THINKING);
    s_round_spoken = false;
    s_round_reply1st[0] = '\0';
    s_tts_dead = false;
    s_tts_armed = false;

    /* 灵魂 TTS 调制: 情绪 → 语速/音高(cosyvoice 每会话参数) */
    {
        soul_tts_hint_t h;
        soul_tts_hint(&h);
        s_tts_cfg.speech_rate = h.rate;
        s_tts_cfg.pitch = (int)(h.pitch > 0 ? h.pitch + 0.5f
                                            : h.pitch - 0.5f);
    }
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    s_rt_spoken = false;
#endif

    /* system 追加技能引导(tools 描述已含语义, 此处强化"点播必须用工具";
     * 禁 URL: LLM 回复带链接会被 cosyvoice 拒绝合成(url error))。
     * "先调用再回应"硬约束: 实测 qwen 对"我想听XX"类委婉表达会只口头
     * 答应而不调工具(幻觉顺从, 用户听到的就是"没反应"), 2026-11-12 */
    char system_full[1408];   /* 人格+时间+硬约束+soul 内在状态+记忆块 */
    /* 当前时间注入: reminder.add 的口语时间换算(明天早上八点)必须有
     * "现在"作参照, 否则 LLM 只能瞎猜(M2); 未同步时提示模型说明 */
    char now_str[40] = "时间未同步";
    {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        if (t > 1000000000LL) {
            static const char *wd[] = { "日","一","二","三","四","五","六" };
            snprintf(now_str, sizeof(now_str), "%d-%02d-%02d %02d:%02d 周%s",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, wd[tmv.tm_wday]);
        }
    }
    snprintf(system_full, sizeof(system_full),
             "%s 现在是%s。%s %s 凡用户表达任何播放意图(想听/放/播/切歌/"
             "换台/调音量/停止), 你必须先调用相应工具, 工具执行成功后才能"
             "回应; 用户要记事或定提醒时也必须调用 memo/reminder 工具; "
             "严禁不调用工具就口头答应。回应一句话口语化简短, 不要出现任何"
             "网址或链接。",
             CONFIG_ESP_CUBE_AI_LLM_SYSTEM, now_str,
             soul_prompt_suffix(), soul_mem_prompt_block());

    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON *tools = ai_skills_tools_json();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content",
                             (user_text && user_text[0]) ? user_text : "你好");
    cJSON_AddItemToArray(messages, um);
    if (tools == NULL) {
        ESP_LOGW(TAG, "no skills registered, plain chat");
    }

    tool_call_t tc;
    for (int round = 0; round < 3; round++) {   /* 首轮 + ≤2 工具轮回 */
        struct ks_llm_cfg_v2 lcfg = {
            .url = AI_LLM_URL,
            .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
            .model = CONFIG_ESP_CUBE_AI_LLM_MODEL,
            .system = system_full,
            .messages = messages,
            .tools = tools,
            .args_cb = on_llm_args,             /* 流式参数: filler+预取 */
            .cut_clauses = true,
            #if CONFIG_ESP_CUBE_AI_LLM_DISABLE_THINKING
            .no_think = true,
#else
            .no_think = false,
#endif
        };
        int rc = ks_llm_chat_v2(&lcfg, on_llm_sent, on_llm_tool, &tc);
        if (s_abort_req) break;
        if (rc == 1 && tc.name[0] != 0 && round < 2) {
            /* 执行技能 → 结果回传(role:"tool") → 下一轮 LLM */
            char reply[256];   /* 技能回执(与 ai_skills.h AI_SKILL_REPLY_MAX 对齐, 免头间耦合) */
            cJSON *args_obj = cJSON_Parse(tc.args);   /* 失败→NULL(无参) */
            esp_err_t err = ai_skill_exec(tc.name, args_obj, reply,
                                          sizeof(reply));
            cJSON_Delete(args_obj);
            ESP_LOGI(TAG, "skill %s(%s) %s: %s", tc.name, tc.args,
                     err == ESP_OK ? "ok" : esp_err_to_name(err), reply);
#if CONFIG_ESP_CUBE_AI_TOOL_FAST_ACK
            /* 快速应答: 成功回执直喂 TTS 跳过第二轮 LLM(首响 -1s);
             * 失败仍走 LLM 轮生成自然道歉 */
            if (err == ESP_OK && reply[0] != '\0') {
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
                int src = ks_rt_ok() ? ks_rt_say(reply) : -100;
                if (src == 0) {
                    cube_event_publish(CUBE_EVT_AI_TEXT, reply,
                                       strlen(reply) + 1);
                    ESP_LOGI(TAG, "llm sentence(rt-say): %s", reply);
                    s_round_spoken = true;    /* 等快线音频排空 */
                } else if (src != -100) {
                    /* 快线半死(say 失败): 回落 cosyvoice 直喂 */
                    ESP_LOGW(TAG, "rt_say rc=%d -> tts fallback", src);
                    if (!s_tts_opened &&
                        tts_open_retry(&s_tts_cfg, &s_tts_ev) == 0) {
                        s_tts_opened = true;
                    }
                    if (s_tts_opened && ks_tts_feed(reply, false, 8000) == 0) {
                        cube_event_publish(CUBE_EVT_AI_TEXT, reply,
                                           strlen(reply) + 1);
                        s_round_spoken = true;
                        if (s_state != AI_SPEAKING) set_state(AI_SPEAKING);
                    }
                } else
#endif
                {
                    on_llm_sent(NULL, reply, false);
                }
                break;
            }
#endif

            cJSON *am = cJSON_CreateObject();
            cJSON_AddStringToObject(am, "role", "assistant");
            cJSON_AddNullToObject(am, "content");
            cJSON *tcs = cJSON_AddArrayToObject(am, "tool_calls");
            cJSON *tcj = cJSON_CreateObject();
            cJSON_AddStringToObject(tcj, "id", tc.id);
            cJSON_AddStringToObject(tcj, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", tc.name);
            cJSON_AddStringToObject(fn, "arguments", tc.args);
            cJSON_AddItemToObject(tcj, "function", fn);
            cJSON_AddItemToArray(tcs, tcj);
            cJSON_AddItemToArray(messages, am);

            cJSON *tm = cJSON_CreateObject();
            cJSON_AddStringToObject(tm, "role", "tool");
            cJSON_AddStringToObject(tm, "tool_call_id", tc.id);
            cJSON_AddStringToObject(tm, "content", reply);
            cJSON_AddItemToArray(messages, tm);
            if (s_abort_req) break;             /* barge-in: 不再进下一轮 */
            continue;
        }
        if (rc < 0) {
            ESP_LOGE(TAG, "llm v2 rc=%d", rc);
        }
        break;                                   /* 纯文本轮完成 */
    }

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    /* 灵魂回执: 有声且健康 = 圆满轮(驱力大恢复); 记忆层顺带记录 */
    soul_on_round((s_round_spoken
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
                   || s_rt_spoken
#endif
                  ) && !s_tts_dead,
                  user_text ? user_text : "", s_round_reply1st);
    /* ---- 轮收尾: 三种情况都不允许卡死在 THINKING/SPEAKING ----
     * a) 本轮有语音且 TTS 健康 → 收口 + 武装排空上报(tts_task 报 DONE)
     * b) TTS 失败/无语音 → 直接收口回 IDLE(不等永远不会来的排空) */
    if (s_tts_opened) {
        /* 全轮唯一 finish-task; 本轮一句都没喂过时直接关(零 continue
         * 直接 finish 会被服务端以 task_group 报错, 2026-11-12 实测) */
        int frc = s_round_spoken ? ks_tts_feed("", true, 5000) : 0;
        ks_tts_close();
        s_tts_opened = false;
        s_tts_closed = true;
        if ((frc == 0 && s_round_spoken && !s_tts_dead)
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            || s_rt_spoken                       /* 快线出声也等排空 */
#endif
        ) {
            s_tts_armed = true;                  /* 等 tts_task 排空上报 */
            s_tts_armed_us = esp_timer_get_time();
            return;
        }
        ESP_LOGW(TAG, "round end w/o speech (frc=%d spoken=%d dead=%d)",
                 frc, (int)s_round_spoken, (int)s_tts_dead);
    }
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    else if (ks_rt_ok() && s_rt_spoken) {
        s_tts_closed = true;                     /* 复用排空判定语义 */
        s_tts_armed = true;
        s_tts_armed_us = esp_timer_get_time();
        return;
    }
#endif
    ESP_LOGI(TAG, "round end (silent) -> IDLE");
    set_state(AI_IDLE);                          /* 兜底: 无声轮直接结束 */
    /* TODO(M2.3): 连续对话; 现为单轮, 排空事件驱动回 IDLE */
}

/* ---- 本地唤醒提示音("叮-叮"): 两音阶+指数衰减, 直写混音器 TTS 源。
 * 20ms 粒度生成(栈缓冲 3.8KB), 无网络无 TTS 成本, 唤醒即时反馈 ---- */
static void wake_chime(void)
{
    const int rate = 48000, frames_per_chunk = 960;    /* 20ms */
    int16_t buf[frames_per_chunk * 2];                 /* stereo */
    /* (频率Hz, 时长ms): C6 → G6, 轻快上行 */
    const struct { int hz; int ms; } seg[2] = {
        { 1046, 80 }, { 1568, 130 },
    };
    float phase = 0;
    for (int g = 0; g < 2; g++) {
        int chunks = seg[g].ms / 20;
        float step = 2.0f * (float)M_PI * seg[g].hz / rate;
        for (int c = 0; c < chunks; c++) {
            /* 包络: 段内指数衰减 + 跨段整体渐弱 */
            float env = 0.28f * expf(-(c * 20.0f + (g ? 80.0f : 0)) / 90.0f);
            int16_t amp = (int16_t)(env * 32767);
            for (int i = 0; i < frames_per_chunk; i++) {
                phase += step;
                int16_t v = (int16_t)(sinf(phase) * amp);
                buf[i * 2] = v;
                buf[i * 2 + 1] = v;
            }
            audio_mixer_source_write(s_tts_src, buf, sizeof(buf),
                                     pdMS_TO_TICKS(100));
        }
    }
}

/* ---- 带外播报(M2): 提醒到点/主动交互出口, "站在会话外说话" ----
 * 快线在位 → ks_rt_say(等首帧 ≤3s, omni 音频回流偶发迟滞的保守窗);
 * 回落 cosyvoice: 开→喂→收口→关, 复用轮内排空机制等播完。
 * 供 session_task(EVT_ANNOUNCE/排空补播)调用, 前置条件 s_state==IDLE */
static void announce_start(const char *text)
{
    char clean[192];
    strlcpy(clean, text, sizeof(clean));
    tts_sanitize(clean);
    if (clean[0] == '\0') return;

    set_state(AI_SPEAKING);               /* ducking 让位(音乐压低) */
    audio_mixer_set_source_active(s_tts_src, true);
    cube_event_publish(CUBE_EVT_AI_TEXT, clean, strlen(clean) + 1);
    ESP_LOGI(TAG, "announce: %s", clean);

#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    if (ks_rt_ok()) {
        if (ks_rt_say(clean) == 0) {
            /* 等首帧(否则空环立刻判"排空" → 闪断): 3s 无回流仍武装,
             * 由 15s 排空超时兜底收口 */
            int64_t t0 = esp_timer_get_time();
            while (!s_rt_spoken &&
                   esp_timer_get_time() - t0 < 3000000) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            s_tts_closed = true;
            s_tts_armed = true;
            s_tts_armed_us = esp_timer_get_time();
            return;
        }
        ESP_LOGW(TAG, "rt_say failed -> tts fallback");
    }
#endif
    if (tts_open_retry(&s_tts_cfg, &s_tts_ev) != 0) {
        ESP_LOGE(TAG, "announce tts open failed");
        set_state(AI_IDLE);
        return;
    }
    s_tts_opened = true;
    cube_resampler_init(&s_rs, AI_TTS_RATE, 48000);
    if (ks_tts_feed(clean, false, 8000) == 0) {
        ks_tts_feed("", true, 5000);      /* 收口(零 continue+finish 会被
                                             服务端拒, 直接 close) */
    } else {
        ESP_LOGW(TAG, "announce tts feed failed");
    }
    ks_tts_close();
    s_tts_opened = false;
    s_tts_closed = true;
    s_tts_armed = true;                   /* tts_task 排空后回 IDLE */
    s_tts_armed_us = esp_timer_get_time();
}

/* ---- 会话任务 ---- */
static void session_task(void *arg)
{
    (void)arg;
    for (;;) {
        session_evt_t e;
        if (xQueueReceive(s_evtq, &e, portMAX_DELAY) != pdPASS) continue;

        switch (e.id) {
        case EVT_WAKE:
            /* 思考期唤醒一律忽略: 此刻尚未出声、无可打断的内容, 且
             * 实测 ASR 定稿后 26~64ms 必来一个 wn9 回声假唤醒
             * (-29~-35dB), 当 barge-in 处理会拦腰杀掉刚识别完的轮
             * (2026-11-11 板上两连测复现, 症状="识别成功但永远没回复") */
            if (s_state == AI_THINKING) {
                ESP_LOGW(TAG, "wake during THINKING ignored (echo?)");
                break;
            }
            /* 防抖: 聆听刚开始 3s 内的重复唤醒多为 wn9 同句双触发
             * (实测 100ms 连击, 2026-11-11), 不当 barge-in 处理 */
            if (s_state == AI_LISTENING &&
                esp_timer_get_time() - s_listen_start_us < 3000000) {
                ESP_LOGW(TAG, "wake debounce (round just started)");
                break;
            }
            if (s_state == AI_SPEAKING) {
                ESP_LOGW(TAG, "barge-in: abort current round");
            }
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            rt_ensure();                     /* 失联重建(≤2s) */
#endif
            s_abort_req = true;
            ks_tts_close();
            ks_asr_close();
            s_ring_r = s_ring_w;
            s_abort_req = false;
            s_tts_closed = false;
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            ks_rt_cancel();                  /* 快线在播应答即停 */
            s_rt_r = s_rt_w;
            s_rt_spoken = false;
#endif
            s_tts_armed = false;
            s_filler_req = false;               /* 打断: filler 即停 */
            s_last_asr[0] = '\0';                /* 防串轮: 旧文本不作本轮输入 */
            cube_resampler_init(&s_rs, AI_TTS_RATE, 48000);
            audio_mixer_set_source_active(s_tts_src, true);

            /* 本地"叮"提示音: 用户可感知已唤醒(此前无声音反馈, 实测
             * 用户不确定是否唤醒成功); PCM 直写混音器, 零成本零延迟 */
            wake_chime();
            s_listen_start_us = esp_timer_get_time();
            set_state(AI_LISTENING);
            struct ks_asr_cfg acfg = {
                .url = AI_DASHSCOPE_URL,
                .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
                .model = CONFIG_ESP_CUBE_AI_ASR_MODEL,
                .sample_rate = AI_MIC_RATE,
            };
            struct ks_asr_events aev = {
                .user = NULL, .on_text = on_asr_text,
            };
            if (asr_open_retry(&acfg, &aev) != 0) {
                ESP_LOGE(TAG, "asr open failed");
                set_state(AI_IDLE);
                break;
            }
            /* TTS 不预开(懒开, 首句时): 唤醒期 ASR+TTS 双 TLS 并发会
             * 争抢 esp-aes DMA 描述符(P4 内部 DMA 内存紧张, 2026-11-11
             * 板上实测互撞超时); 代价 = 首句语音回应延迟 ~1s */
            /* 自测注入此刻才武装: 提前武装会被 voice_task 在 chime+
             * ASR 开握手期吃掉开头(含静音垫), 校准窗吃到语音把阈值
             * 抬到天文数字 → 6s 假"无语音"(2026-11-12 板上定位) */
            if (s_st_pending) {
                audio_mic_test_inject(s_st_buf, s_st_w);
                s_st_pending = false;
            }
            vTaskResume(s_mic_task);             /* 开始录音 */
            break;

        case EVT_TEXT_IN: {
            /* 文本注入(CLI ask / 调试): 复位轮状态 → 直接 LLM+TTS */
            char *text = (char *)e.payload;
            s_abort_req = true;
            ks_tts_close();
            ks_asr_close();
            s_ring_r = s_ring_w;
            s_abort_req = false;
            s_tts_closed = false;
            s_tts_armed = false;
            s_last_asr[0] = '\0';
            cube_resampler_init(&s_rs, AI_TTS_RATE, 48000);
            audio_mixer_set_source_active(s_tts_src, true);

            /* 文本注入路径无 ASR 并发, 预开安全(首音低延迟);
             * 仅成功才登记 s_tts_opened —— 失败也置 true 会让懒开
             * 路径跳过重试, 全轮 feed -2 无声(2026-11-11 排查)。
             * 快线在位: 出声走 omni, 不预开 */
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            bool skip_pre = ks_rt_ok();
#else
            bool skip_pre = false;
#endif
            if (!skip_pre &&
                tts_open_retry(&s_tts_cfg, &s_tts_ev) == 0) {
                s_tts_opened = true;
            } else if (!skip_pre) {
                ESP_LOGE(TAG, "tts open failed(text-in)");
            }
            ESP_LOGI(TAG, "text-in: %s", text);
            cube_event_publish(CUBE_EVT_AI_TEXT, text, strlen(text) + 1);
            run_round(text);
            free(text);
            break;
        }

        case EVT_MIC_DONE:
            /* 定稿竞态防御: finish 超时(3s)内 final 未回时 MIC_DONE 会
             * 抢在 ASR_TEXT 前 → 拿旧文本答非所问。空文本 = 本轮作废 */
            if (s_last_asr[0] == '\0') {
                ESP_LOGW(TAG, "mic done w/o final text, drop round");
                set_state(AI_IDLE);
                break;
            }
            /* TTS 预热(延迟优化): 此刻 ASR WS 已收线, 无 esp-aes 描述符
             * 争抢, 与 LLM 第 1 趟并行握手 —— 懒开路径保留为失败回退。
             * 快线在位时出声走 omni, 不预开 cosyvoice(省一条连接) */
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
            if (!ks_rt_ok())
#endif
            {
                if (!s_tts_opened &&
                    tts_open_retry(&s_tts_cfg, &s_tts_ev) == 0) {
                    s_tts_opened = true;
                }
            }
            run_round(s_last_asr);
            break;

        case EVT_TTS_DONE:                       /* 播放排空 → 本轮结束 */
            if (s_state == AI_SPEAKING && !s_abort_req) {
                set_state(AI_IDLE);
                ESP_LOGI(TAG, "round complete -> IDLE");
                if (s_announce_pend) {           /* 会话期挂起的播报补上 */
                    s_announce_pend = false;
                    char buf[sizeof(s_announce_text)];
                    strlcpy(buf, s_announce_text, sizeof(buf));
                    announce_start(buf);
                    break;
                }
            }
            break;

        case EVT_ANNOUNCE: {                     /* 带外播报(任意任务来源) */
            char *text = (char *)e.payload;
            if (s_state != AI_IDLE) {            /* 忙: 挂起(单槽, 新覆盖旧) */
                strlcpy(s_announce_text, text, sizeof(s_announce_text));
                if (!s_announce_pend) {
                    s_announce_pend = true;
                    ESP_LOGI(TAG, "announce deferred (state=%d)", s_state);
                } else {
                    ESP_LOGW(TAG, "announce slot busy, replaced");
                }
            } else {
                announce_start(text);
            }
            free(text);
            break;
        }

        case EVT_ASR_TEXT: {
            char *text = (char *)e.payload;
            ESP_LOGI(TAG, "asr: %s", text);
            strlcpy(s_last_asr, text, sizeof(s_last_asr));
            /* UTF-8 边界回退(截断保护) */
            size_t al = strlen(s_last_asr);
            if (al > 0 && text[al] != 0) {
                while (al > 0 &&
                       ((uint8_t)s_last_asr[al - 1] & 0xC0) == 0x80) al--;
                if (al > 0 && ((uint8_t)s_last_asr[al - 1] & 0x80)) al--;
                s_last_asr[al] = 0;
            }
            cube_event_publish(CUBE_EVT_AI_TEXT, text, strlen(text) + 1);
            free(text);
            break;
        }
        default:
            break;
        }
    }
}

/* ---- 唤醒事件桥(总线回调, voice_task 上下文) ----
 * 排队之外还要【立即】掐断当前轮: session_task 单线程, 忙于 run_round
 * (LLM 流式 + TTS 播报可达 10s+)期间队列里的 WAKE 根本得不到处理,
 * barge-in 形同虚设(2026-11-11 用户实测"喊了没反应")。此处只做两件
 * 并发安全的事: 置 abort(LLM 回调断流 / tts_task 停写) + 快进读指针
 * (丢弃已收 TTS PCM, 喇叭 ~20ms 内安静)。会话 close/reopen 仍由
 * session_task 处理 EVT_WAKE 时统一做。 */
static void ai_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                      void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WAKE_DETECTED) {
        /* 即时静音仅限 SPEAKING(有声明内容可打断); THINKING 期的唤醒
         * 检出必为回声, 不掐流(由 EVT_WAKE 处理忽略) */
        if (s_state == AI_SPEAKING) {
            s_abort_req = true;
            s_ring_r = s_ring_w;         /* 快进读指针=tts_task 见空即哑 */
            soul_on_barge();             /* 打断: 强社交在场信号 */
        }
        ai_session_on_wake();
    }
}

/* ---- 对外 ---- */
esp_err_t ai_session_start(void)
{
    if (s_task != NULL) return ESP_OK;
    s_evtq = xQueueCreate(8, sizeof(session_evt_t));
    ESP_RETURN_ON_FALSE(s_evtq != NULL, ESP_ERR_NO_MEM, TAG, "evtq");
    s_ring = heap_caps_malloc(TTS_RING_FRAMES * 2, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring != NULL, ESP_ERR_NO_MEM, TAG, "tts ring");

    audio_source_cfg_t scfg = {
        .name = "tts", .type = AUDIO_SRC_TTS,
        .buffer_ms = 200, .prefill_ms = 120, .blocking = true,
    };
    s_tts_src = audio_mixer_add_source(&scfg);
    ESP_RETURN_ON_FALSE(s_tts_src != NULL, ESP_FAIL, TAG, "tts src");
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
    s_rt_ring = heap_caps_malloc(RT_RING_FRAMES * 2, MALLOC_CAP_SPIRAM);
    if (s_rt_ring == NULL) {
        ESP_LOGW(TAG, "rt ring oom, fast-line disabled");
    }
#endif

    ESP_RETURN_ON_ERROR(cube_event_subscribe(ai_evt_cb, NULL), TAG,
                        "evt subscribe");

    BaseType_t ok = xTaskCreatePinnedToCore(session_task, CUBE_TASK_AISESS_NAME,
                                            CUBE_TASK_AISESS_STACK, NULL,
                                            CUBE_TASK_AISESS_PRIO,
                                            &s_task, CUBE_TASK_AISESS_CORE) == pdPASS &&
                    xTaskCreatePinnedToCore(tts_task, CUBE_TASK_AITTS_NAME,
                                            CUBE_TASK_AITTS_STACK, NULL,
                                            CUBE_TASK_AITTS_PRIO,
                                            &s_tts_task, CUBE_TASK_AITTS_CORE) == pdPASS &&
                    xTaskCreatePinnedToCore(mic_task, CUBE_TASK_AIMIC_NAME,
                                            CUBE_TASK_AIMIC_STACK, NULL,
                                            CUBE_TASK_AIMIC_PRIO,
                                            &s_mic_task, CUBE_TASK_AIMIC_CORE) == pdPASS;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "tasks");
    vTaskSuspend(s_mic_task);
    ESP_LOGI(TAG, "ai session ready (dashscope byok, tts=%dk, fastline=%d)",
             AI_TTS_RATE,
#if CONFIG_ESP_CUBE_AI_RT_ENABLE
             s_rt_ring != NULL
#else
             0
#endif
    );
    return ESP_OK;
}

void ai_session_on_wake(void)
{
    session_evt_t e = { .id = EVT_WAKE };
    xQueueSend(s_evtq, &e, 0);
}

esp_err_t ai_session_announce(const char *text)
{
    if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;
    session_evt_t e = { .id = EVT_ANNOUNCE,
                        .payload = strdup(text) };
    if (e.payload == NULL) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_evtq, &e, 0) != pdPASS) {
        free(e.payload);
        return ESP_ERR_INVALID_STATE;       /* 队列满: 调用方自行取舍 */
    }
    return ESP_OK;
}

/* ================= selftest: TTS 回环注入(自动化验证) =================
 * 无真人时的全链自测: 云端 TTS 合成"人声" → 注入麦克服务(实时节奏) →
 * 唤醒/ASR/LLM/技能/播放器全链照常运转。CLI: selftest <text> / stwake */
static bool st_synth(const char *text)
{
    if (s_st_buf == NULL) {
        s_st_cap = 160 * 1024 / 2;              /* 160KB ≈ 10s@16k */
        s_st_buf = heap_caps_malloc(s_st_cap * 2, MALLOC_CAP_SPIRAM);
        if (s_st_buf == NULL) return false;
    }
    s_st_w = 0;
    s_st_capture = true;
    /* 头部垫 300ms 静音: mic_task 噪声底标定窗(前 15 块)不应吃到
     * 语音, 否则阈值被抬到语音电平(板上实测 floor=4.5M) */
    memset(s_st_buf, 0, 4800);
    s_st_w = 4800;
    bool ok = tts_open_retry(&s_tts_cfg, &s_tts_ev) == 0;
    if (ok) {
        ok = ks_tts_feed(text, true, 10000) == 0;
        ks_tts_close();
    }
    s_st_capture = false;
    if (!ok || s_st_w == 0) {
        ESP_LOGE(TAG, "ST: synth failed (ok=%d frames=%u)",
                 (int)ok, (unsigned)s_st_w);
        return false;
    }
    ESP_LOGI(TAG, "ST: synth ok (%.2fs)", (double)s_st_w / 16000.0);
    return true;
}

esp_err_t ai_session_selftest(const char *text)
{
    if (s_state != AI_IDLE) {
        ESP_LOGW(TAG, "ST: busy (state=%d), try later", s_state);
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == 0) return ESP_ERR_INVALID_ARG;
    if (!st_synth(text)) return ESP_FAIL;
    s_st_pending = true;                 /* 唤醒处理内武装(见 EVT_WAKE) */
    ESP_LOGI(TAG, "ST: round start");
    ai_session_on_wake();                       /* 唤醒轮由注入音频走完 */
    return ESP_OK;
}

esp_err_t ai_session_stwake(void)
{
    if (!st_synth("你好小智")) return ESP_FAIL;
    audio_mic_test_inject(s_st_buf, s_st_w);    /* wn9 应从注入音频检出唤醒 */
    ESP_LOGI(TAG, "ST: wake audio injected (expect ★ WAKE)");
    return ESP_OK;
}

esp_err_t ai_session_submit_text(const char *text)
{
    if (s_evtq == NULL) {
        return ESP_ERR_INVALID_STATE;            /* 会话未启动 */
    }
    if (text == NULL || text[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    session_evt_t e = { .id = EVT_TEXT_IN, .payload = strdup(text) };
    if (e.payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_evtq, &e, 0) != pdPASS) {
        free(e.payload);
        return ESP_FAIL;
    }
    return ESP_OK;
}

#endif /* CONFIG_ESP_CUBE_ENABLE_AI */
