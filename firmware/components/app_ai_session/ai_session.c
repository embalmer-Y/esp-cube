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
 *   mic_task     —— LISTENING 期采集(drv_audio_mic_read→ASR feed)
 *   tts_task     —— TTS PCM 环形缓冲 → 重采样 → mixer(阻塞 pacing)
 *   ks_ 回调在网络任务上下文, 一律投队列/写环形缓冲, 不持锁不阻塞
 *
 * 触发: ai_session_on_wake() —— 唤醒词(app_voice)/调试事件均可调。
 * 打断: SPEAKING/LISTENING 中再次唤醒 → abort → 重开 LISTENING。
 */
#include "ai_session.h"

#include <string.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "drv_audio.h"
#include "speech_provider.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CUBE_ENABLE_AI

/* VOICE/AI 关闭: 空实现(仅保启动接口可用) */
esp_err_t ai_session_start(void) { return ESP_OK; }
void ai_session_on_wake(void) {}

#else

static const char *TAG = "ai_session";

/* ---- 云端配置(百炼旧域名, V-33 全功能真机验证) ---- */
#define AI_DASHSCOPE_URL   "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define AI_LLM_URL \
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define AI_MIC_RATE        16000
#define AI_TTS_RATE        CONFIG_ESP_CUBE_AI_TTS_RATE

/* ---- 能量 VAD 断句 ---- */
#define VAD_SILENCE_MS     900
#define VAD_MIN_SPEECH_MS  400
#define VAD_RMS_THRESH     350        /* ES8311 增益标定后调 */
#define LISTEN_MAX_MS      15000

/* ---- 状态 ---- */
static ai_state_t s_state = AI_IDLE;
static QueueHandle_t s_evtq;
static TaskHandle_t s_task, s_mic_task, s_tts_task;
static char s_last_asr[128];

/* TTS 播放桥: 网络回调 → 环形缓冲(mono 16k) → 重采样 → mixer */
#define TTS_RING_FRAMES    (AI_TTS_RATE * 4)        /* 4s */
static int16_t *s_ring;
static volatile size_t s_ring_r, s_ring_w;    /* 单写单读 */
static cube_resampler_t s_rs;
static audio_source_handle_t s_tts_src;
static volatile bool s_abort_req;

/* 事件 */
enum {
    EVT_WAKE = 1,
    EVT_ASR_TEXT,          /* payload: char*(堆, 消费方释放) */
    EVT_MIC_DONE,
};

/* 队列元素(带指针的事件) */
typedef struct {
    int id;
    void *payload;
} session_evt_t;

static void set_state(ai_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    cube_event_publish(CUBE_EVT_AI_STATE, &st, sizeof(st));
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

/* ---- LLM 句读回调(与 ks_llm_chat 同线程): 直接喂 TTS ---- */
static bool on_llm_sent(void *user, const char *sentence, bool final)
{
    (void)user;
    if (s_abort_req) return false;
    if (sentence[0] != '\0') {
        cube_event_publish(CUBE_EVT_AI_TEXT, sentence,
                           strlen(sentence) + 1);
        int rc = ks_tts_feed(sentence, final, 8000);
        if (rc != 0) {
            ESP_LOGW(TAG, "tts feed rc=%d", rc);
        }
        if (s_state != AI_SPEAKING) set_state(AI_SPEAKING);
    }
    return true;
}

/* ---- TTS PCM 回调(网络任务) → 环形缓冲 ---- */
static void on_tts_pcm(void *user, const uint8_t *pcm, size_t len)
{
    (void)user;
    const int16_t *in = (const int16_t *)pcm;
    for (size_t i = 0; i < len / 2; i++) {
        size_t next = (s_ring_w + 1) % TTS_RING_FRAMES;
        int guard = 0;
        while (next == s_ring_r && !s_abort_req && guard++ < 200) {
            vTaskDelay(pdMS_TO_TICKS(10));
            next = (s_ring_w + 1) % TTS_RING_FRAMES;
        }
        if (next == s_ring_r) return;             /* 打断: 丢弃剩余 */
        s_ring[s_ring_w] = in[i];
        s_ring_w = next;
    }
}

static void on_tts_err(void *user, int stage, int code)
{
    (void)user;
    ESP_LOGW(TAG, "tts err stage=%d code=%d", stage, code);
}

/* ---- TTS 播放任务: 环形缓冲 → 重采样 48k stereo → mixer ---- */
static void tts_task(void *arg)
{
    (void)arg;
    int16_t in_mono[320];                        /* 20ms@16k */
    int16_t out48[320 * 3 * 2];
    while (1) {
        if (s_ring_r == s_ring_w || s_state != AI_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int n = 0;
        while (n < 320 && s_ring_r != s_ring_w) {
            in_mono[n++] = s_ring[s_ring_r];
            s_ring_r = (s_ring_r + 1) % TTS_RING_FRAMES;
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

/* ---- 录音任务: mic 16k → ASR feed; 能量 VAD 断句 ---- */
static void mic_task(void *arg)
{
    (void)arg;
    int16_t pcm[320];                            /* 20ms@16k */
    while (1) {
        vTaskSuspend(NULL);                      /* 由 session 唤醒 */
        int silence_ms = 0, speech_ms = 0, total_ms = 0;
        bool had_speech = false;
        while (s_state == AI_LISTENING && !s_abort_req) {
            int n = drv_audio_mic_read(pcm, 320);
            if (n <= 0) continue;
            int64_t sq = 0;
            for (int i = 0; i < n; i++) sq += (int64_t)pcm[i] * pcm[i];
            int rms = (int)(sq / n);
            ks_asr_feed(pcm, n);
            total_ms += 20;

            if (rms > VAD_RMS_THRESH) {
                speech_ms += 20;
                silence_ms = 0;
                had_speech = true;
            } else if (had_speech) {
                silence_ms += 20;
            }
            if ((had_speech && speech_ms > VAD_MIN_SPEECH_MS &&
                 silence_ms > VAD_SILENCE_MS) ||
                total_ms > LISTEN_MAX_MS) {
                ks_asr_finish(1200);             /* flush 尾句(阻塞) */
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

/* ---- LLM→TTS 一轮编排(EVT_MIC_DONE 后 session_task 内执行) ---- */
static void run_round(void)
{
    set_state(AI_THINKING);
    struct ks_llm_cfg lcfg = {
        .url = AI_LLM_URL,
        .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
        .model = CONFIG_ESP_CUBE_AI_LLM_MODEL,
        .system = CONFIG_ESP_CUBE_AI_LLM_SYSTEM,
        .user_msg = s_last_asr[0] ? s_last_asr : "你好",
        .cut_clauses = true,
        .no_think = true,
    };
    int64_t first_tok = 0;
    int rc = ks_llm_chat(&lcfg, on_llm_sent, NULL, &first_tok);
    if (rc != 0) {
        ESP_LOGE(TAG, "llm chat rc=%d", rc);
    }
    ks_tts_feed("", true, 5000);                 /* TTS 收口 */
    ks_tts_close();
    /* TODO(M2.3): TTS 源耗尽检测→连续对话; 现为单轮后回 IDLE */
    vTaskDelay(pdMS_TO_TICKS(800));              /* 尾句播放窗口(粗略) */
    if (!s_abort_req) set_state(AI_IDLE);
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
            if (s_state == AI_SPEAKING || s_state == AI_THINKING) {
                ESP_LOGW(TAG, "barge-in: abort current round");
            }
            s_abort_req = true;
            ks_tts_close();
            ks_asr_close();
            s_ring_r = s_ring_w;
            s_abort_req = false;
            audio_mixer_set_source_active(s_tts_src, true);
            cube_resampler_init(&s_rs, AI_TTS_RATE, 48000);
            audio_mixer_set_source_active(s_tts_src, true);

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
            if (ks_asr_open(&acfg, &aev) != 0) {
                ESP_LOGE(TAG, "asr open failed");
                set_state(AI_IDLE);
                break;
            }
            /* TTS 会话预开(V-33: 连接预热思路——open 与 ASR 并行,
             * 首句 feed 时 TLS 已就绪) */
            struct ks_tts_cfg tcfg = {
                .url = AI_DASHSCOPE_URL,
                .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
                .model = CONFIG_ESP_CUBE_AI_TTS_MODEL,
                .voice = CONFIG_ESP_CUBE_AI_TTS_VOICE,
                .sample_rate = AI_TTS_RATE,
                .speech_rate = 0, .pitch = 0,
            };
            struct ks_tts_events tev = {
                .user = NULL, .on_pcm = on_tts_pcm, .on_error = on_tts_err,
            };
            if (ks_tts_open(&tcfg, &tev) != 0) {
                ESP_LOGE(TAG, "tts open failed");
            }
            vTaskResume(s_mic_task);             /* 开始录音 */
            break;

        case EVT_MIC_DONE:
            run_round();
            break;

        case EVT_ASR_TEXT: {
            char *text = (char *)e.payload;
            ESP_LOGI(TAG, "asr: %s", text);
            strncpy(s_last_asr, text, sizeof(s_last_asr) - 1);
            s_last_asr[sizeof(s_last_asr) - 1] = 0;
            cube_event_publish(CUBE_EVT_AI_TEXT, text, strlen(text) + 1);
            free(text);
            break;
        }
        default:
            break;
        }
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

    BaseType_t ok = xTaskCreatePinnedToCore(session_task, "aisess", 8192,
                                            NULL, 5, &s_task, 0) == pdPASS &&
                    xTaskCreatePinnedToCore(tts_task, "aitts", 6144,
                                            NULL, 5, &s_tts_task, 1) == pdPASS &&
                    xTaskCreatePinnedToCore(mic_task, "aimic", 6144,
                                            NULL, 5, &s_mic_task, 0) == pdPASS;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "tasks");
    vTaskSuspend(s_mic_task);
    ESP_LOGI(TAG, "ai session ready (dashscope byok, tts=%dk)",
             AI_TTS_RATE);
    return ESP_OK;
}

void ai_session_on_wake(void)
{
    session_evt_t e = { .id = EVT_WAKE };
    xQueueSend(s_evtq, &e, 0);
}

#endif /* CONFIG_ESP_CUBE_ENABLE_AI */
