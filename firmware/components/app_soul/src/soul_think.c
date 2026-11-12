/*
 * soul_think.c —— 主动交互(点火, M3; Kconfig ESP_CUBE_SOUL_PROACTIVE)
 *
 * Kaleido ignition 防打扰框架最小版(22 号 §5 八条中实装六条):
 *   触发: ki_think_gate(驱力空闲∨低唤起∨显著分∨新事件 + 新意门);
 *   深思: LLM 单发(无 tools)产出 typed 意图 JSON, ki_intent_parse
 *         拒非降级(schema 违例绝不猜, 含数字无证据拒 —— 防幻觉);
 *   出口: announce 带外播报(不经 ASR 轮, "站在会话外说话");
 *   防打扰: 开机静默 3min / 勿扰 23:00-08:00(候选丢弃不排队) /
 *         日配额 4 / 冷静期 30min(被忽略 ×2 封顶) / 5min 内容去重。
 * 30s 轮询(触发密度低, 云钱节流: 思维最小间隔 10min 在 gate 内)。
 */
#include "soul.h"
#include "soul_ignition.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "speech_provider.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CUBE_SOUL_PROACTIVE

static const char *TAG = "soulthink";

#define THINK_POLL_SEC   30
#define THINK_BOOT_SILENCE 180       /* 开机静默( WiFi/校时稳住再说话) */
#define THINK_OBSERVE_SEC 20         /* 播报后观察窗(有无回应) */

static esp_err_t (*s_speak)(const char *text);
static char s_last_content[KI_CONTENT_MAX + 1];
static time_t s_last_fire;
static time_t s_boot_at;
static int s_fired_today = -1;       /* 日配额计数(负=未初始化) */

void soul_think_set_speak(esp_err_t (*fn)(const char *text))
{
    s_speak = fn;
}

/* ---- LLM 整句收集 ---- */
static bool think_sent(void *user, const char *sentence, bool final)
{
    (void)final;
    strlcat((char *)user, sentence, 512);
    return true;
}

static bool llm_think(const char *prompt, char *out, size_t cap)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", prompt);
    cJSON_AddItemToArray(messages, um);
    struct ks_llm_cfg_v2 cfg = {
        .url = CONFIG_ESP_CUBE_AI_LLM_URL,
        .api_key = CONFIG_ESP_CUBE_AI_API_KEY,
        .model = CONFIG_ESP_CUBE_AI_LLM_MODEL,
        .system = "你是桌面语音助理的内心。根据内在状态决定要不要主动"
                  "跟用户说一句话。必须只输出一行 JSON: {\"act\":\"speak\""
                  "或\"silent\",\"content\":\"不超过25字的一句话\","
                  "\"confidence\":0.0到1.0,\"reason\":\"简短理由\"}。"
                  "没有充分理由必须 silent; content 不许出现数字; 语气"
                  "自然, 像家人搭话, 不许说教。",
        .messages = messages,
        .tools = NULL,
        .cut_clauses = false,
#if CONFIG_ESP_CUBE_AI_LLM_DISABLE_THINKING
        .no_think = true,
#else
        .no_think = false,
#endif
    };
    out[0] = '\0';
    int rc = ks_llm_chat_v2(&cfg, think_sent, NULL, out);
    cJSON_Delete(messages);
    return rc >= 0 && out[0] != '\0';
}

/* ---- 一次深思维度 ---- */
static void think_once(void)
{
    soul_state_t st;
    if (!soul_state_get(&st) || !st.ai_idle) return;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    uint32_t now_min = tmv.tm_hour * 60 + tmv.tm_min;

    /* 日配额(日界重置) */
    if (s_fired_today < 0 || s_last_fire / 86400 != now / 86400) {
        s_fired_today = 0;
    }

    /* 五因子(简化供给: 作息/情感/事件/在场; mem_hook 未接=0) */
    struct ki_factors f = {
        .time_rhythm = (tmv.tm_hour >= 9 && tmv.tm_hour < 21) ? 0.6f : 0.2f,
        .affect_motive = st.urgency,
        .memory_hook = 0.0f,
        .event_hook = st.idle_sec > 1800 ? 0.8f : 0.1f,
        .presence = st.idle_sec < 300 ? 0.7f : 0.2f,
    };
    float urg_max = 0;
    for (int i = 0; i < 3; i++) {
        float e = 1.0f - st.drive_level[i];
        if (e > urg_max) urg_max = e;
    }
    struct ki_context kctx = {
        .user_speaking = false,
        .quiet_hours = (tmv.tm_hour >= 23 || tmv.tm_hour < 8),
        .now_min = now_min,
        .last_fire_min = s_last_fire ? (uint32_t)(s_last_fire % 86400 / 60) : 0,
        .fired_today = (uint8_t)s_fired_today,
    };
    struct ki_decision dec;
    ki_step(&kctx, ki_config_default(), &f, &dec);

    struct ki_think_ctx tctx = {
        .arousal = st.arousal,
        .urgency_max = urg_max,
        .pending_command = false,
        .quiet_hours = kctx.quiet_hours,
        .new_event = (st.idle_sec > 1800),
        .now_min = now_min,
        .last_think_min = s_last_fire ? (uint32_t)(s_last_fire % 86400 / 60) : 0,
        .thought_today = (uint8_t)s_fired_today,
        .score = dec.score,
        .mem_hook = 0.0f,
    };
    struct ki_think_gate gate;
    ki_think_gate(&tctx, &gate);
    if (!gate.trigger) {
        if (gate.blocked_mask) {
            ESP_LOGI(TAG, "think blocked mask=0x%x", gate.blocked_mask);
        }
        return;
    }

    /* 深思(云钱): 内在状态 + 场景 → 单句意图 */
    char prompt[320];
    snprintf(prompt, sizeof(prompt),
             "现在是%d点%d分, 用户已经%d分钟没理你。你的内在状态: 心情"
             "%.2f(%s), 活力%.2f, 想说话的冲动%.2f, 社交联结%.2f。决定"
             "要不要说句话。",
             tmv.tm_hour, tmv.tm_min, st.idle_sec / 60,
             st.valence, st.anim_tag, st.arousal, st.urgency,
             st.social_bond);
    char raw[512];
    if (!llm_think(prompt, raw, sizeof(raw))) {
        ESP_LOGW(TAG, "think llm failed");
        return;
    }
    struct ki_intent intent;
    if (ki_intent_parse(raw, strlen(raw), &intent) != KI_PARSE_OK ||
        intent.act != KI_SPEAK || intent.confidence < KI_CONF_FLOOR) {
        ESP_LOGI(TAG, "think -> silent (raw=%.60s)", raw);
        return;
    }
    /* 5min 内容去重 */
    if (s_last_fire && now - s_last_fire < 300 &&
        strcmp(intent.content, s_last_content) == 0) {
        return;
    }
    strlcpy(s_last_content, intent.content, sizeof(s_last_content));
    s_last_fire = now;
    s_fired_today++;
    ESP_LOGI(TAG, "ignite: %s (%.2f, %s)",
             intent.content, intent.confidence, intent.reason);

    if (s_speak && s_speak(intent.content) == ESP_OK) {
        /* 观察窗结算: 20s 内有互动(非 idle)=responded, 否则 ignored ×2 */
        bool responded = false;
        for (int i = 0; i < THINK_OBSERVE_SEC; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            soul_state_t s2;
            if (soul_state_get(&s2) && !s2.ai_idle) {
                responded = true;
                break;
            }
        }
        ki_settle2(responded ? KI_SETTLE_RESPONDED : KI_SETTLE_IGNORED);
        ESP_LOGI(TAG, "settle: %s", responded ? "responded" : "ignored");
    }
}

static void soul_think_task(void *arg)
{
    (void)arg;
    s_boot_at = time(NULL);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(THINK_POLL_SEC * 1000));
        if (time(NULL) - s_boot_at < THINK_BOOT_SILENCE) continue;
        think_once();
    }
}

esp_err_t soul_think_start(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;
    if (xTaskCreate(soul_think_task, "soulthink", 6144, NULL, 3, NULL)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "proactive think up (poll %ds)", THINK_POLL_SEC);
    return ESP_OK;
}

#else /* !PROACTIVE */

esp_err_t soul_think_start(void) { return ESP_OK; }
void soul_think_set_speak(esp_err_t (*fn)(const char *text)) { (void)fn; }

#endif
