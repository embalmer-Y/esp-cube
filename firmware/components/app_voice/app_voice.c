/*
 * app_voice.c —— 语音前端(esp-sr WakeNet 唤醒)
 *
 * 移植自 Kaleido V-04 唤醒真机实测工程(S31+IDF6.1+esp-sr 2.6.0,
 * 检出率 100%/误触发 0, docs/gates/V-04)。S31 关键事实:
 *   - 唯一可用模型 = wn10_hilexin_int16("Hi,乐鑫"; wn9s 挂载缺失,
 *     wn10 q8p2 缺量化内核) → CONFIG_SR_WN_WN10_HILEXIN_INT16=y
 *   - esp-sr 自动挂载缺失 → add_wakenet_model 显式挂载
 *   - 阈值 0.60 实测无虚警(正式参数量产标定)
 *   - 采集分片续读 + DMA 自愈(int16 推理负载下 I2S 会部分超时)
 *
 * 管线: audio_mic_read_16k(L2 麦克风服务) → AFE("M") → WakeNet
 *       → 唤醒事件 CUBE_EVT_WAKE_DETECTED → ai_session(自订阅)
 * 麦克风所有权(docs/06 §3): AI_LISTENING 期间暂停读取让渡给 ASR,
 * 其余状态常开(保 SPEAKING 期 barge-in 再唤醒打断)。
 * AEC 参考(放歌打断场景)与 MultiNet 命令词留 M2.2+; 单麦无 BSS。
 */
#include "app_voice.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "audio_mic.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

#if CONFIG_ESP_CUBE_ENABLE_VOICE

static const char *TAG = "voice";

/* WakeNet 模型随目标选择(Kconfig SR_WN_*):
 *   S31 = wn10_hilexin("Hi,乐鑫", V-04 唯一可用)
 *   P4  = wn9_nihaoxiaozhi_tts("你好小智", Kaleido voice-loop-p4 真机实证) */
#if CONFIG_SR_WN_WN10_HILEXIN_INT16
#define VOICE_WN_MODEL     "wn10_hilexin"
#define VOICE_WAKE_PHRASE  "Hi,乐鑫"
#elif CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS
#define VOICE_WN_MODEL     "wn9_nihaoxiaozhi_tts"
#define VOICE_WAKE_PHRASE  "你好小智"
#else
#error "未选择 WakeNet 模型(menuconfig: Component config → esp-sr)"
#endif

static TaskHandle_t s_task;
static volatile bool s_running;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static volatile bool s_mic_yield;          /* AI_LISTENING 期让渡麦克风 */

/* ---- 麦克风所有权切换: 订阅 AI 会话状态(docs/06 §3) ---- */
static void voice_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                         void *ctx)
{
    (void)len; (void)ctx;
    if (id == CUBE_EVT_AI_STATE && data != NULL) {
        s_mic_yield = (*(const int *)data == AI_LISTENING);
        ESP_LOGI(TAG, "mic %s", s_mic_yield ? "yield → asr" : "resume");
    }
}

/* ---- 唤醒前端任务 ---- */
static void voice_task(void *arg)
{
    (void)arg;
    int consec_fail = 0;

    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    const int ch_num = s_afe->get_feed_channel_num(s_afe_data);
    ESP_LOGI(TAG, "AFE chunk=%d feed_ch=%d rate=%d", chunk, ch_num,
             s_afe->get_samp_rate(s_afe_data));

    const int frame_frames = chunk * ch_num;     /* feed 需要的 16k 样本数 */
    int16_t *feed_buf = malloc(frame_frames * sizeof(int16_t));
    if (feed_buf == NULL) {
        ESP_LOGE(TAG, "feed buf oom");
        vTaskDelete(NULL);                       /* 任务禁止 return */
        return;
    }

    int detect_count = 0;
    TickType_t last_hb = xTaskGetTickCount();
    int reads = 0;
    for (;;) {
        if (!s_running) break;
        if (s_mic_yield) {                       /* 所有权让渡期 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int got = audio_mic_read_16k(feed_buf, frame_frames);
        if (got < frame_frames) {
            consec_fail++;
            if (consec_fail % 20 == 1) {
                ESP_LOGE(TAG, "mic starved x%d (got=%d)", consec_fail, got);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        consec_fail = 0;

        s_afe->feed(s_afe_data, feed_buf);
        reads++;

        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (res->wakeup_state == WAKENET_DETECTED) {
            detect_count++;
            ESP_LOGW(TAG, "★ WAKE #%d vol=%.1fdBFS heap=%u", detect_count,
                     res->data_volume,
                     (unsigned)esp_get_free_heap_size());
            cube_event_publish(CUBE_EVT_WAKE_DETECTED, NULL, 0);
        }
        if ((xTaskGetTickCount() - last_hb) > pdMS_TO_TICKS(15000)) {
            last_hb = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive reads=%d det=%d heap=%u", reads,
                     detect_count, (unsigned)esp_get_free_heap_size());
        }
    }
    free(feed_buf);
    vTaskDelete(NULL);
}

esp_err_t app_voice_start(void)
{
    if (s_task != NULL) return ESP_OK;

    /* ---- 模型挂载(分区 "model") ---- */
    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(models != NULL && models->num > 0, ESP_FAIL, TAG,
                        "no sr models in partition");
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "model[%d]=%s", i, models->model_name[i]);
    }

    /* ---- AFE: 单麦 SR 高性能模式(逐步打点: 定位 S31 加载期静默复位) ---- */
    ESP_LOGI(TAG, "step1: afe_config_init...");
    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR,
                                            AFE_MODE_HIGH_PERF);
    ESP_RETURN_ON_FALSE(afe_cfg != NULL, ESP_FAIL, TAG, "afe cfg");
    /* 内存模式必须 MORE_PSRAM(Kaleido V-04 真机定参): 默认 internal 模式
     * 在 S31 上 AFE 创建期间损坏 TLSF 堆元数据 → block_trim_free 断言
     * 复位(2026-11-12 实测; 与 convo-stage/wakenet_test 两处参照一致)。
     * 注: 不调 afe_config_check(V-40 实证其破坏唤醒检测) */
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    ESP_LOGI(TAG, "step2: esp_afe_handle_from_config...");
    s_afe = esp_afe_handle_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe != NULL, ESP_FAIL, TAG, "afe handle");
    ESP_LOGI(TAG, "step3: create_from_config...");
    s_afe_data = s_afe->create_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_FAIL, TAG, "afe data");
    ESP_LOGI(TAG, "step4: add_wakenet_model...");

    /* S31 自动挂载缺失——显式添加(V-04 W-5); 模型名随目标(见文件头) */
    int rc = s_afe->add_wakenet_model(s_afe_data, VOICE_WN_MODEL);
    ESP_LOGI(TAG, "step5: add_wakenet_model rc=%d", rc);
    ESP_LOGW(TAG, "add_wakenet_model(%s) → %d", VOICE_WN_MODEL, rc);
    s_afe->print_pipeline(s_afe_data);
    s_afe->set_wakenet_threshold(s_afe_data, 1, 0.60);

    ESP_RETURN_ON_ERROR(cube_event_subscribe(voice_evt_cb, NULL), TAG,
                        "evt subscribe");

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, CUBE_TASK_VOICE_NAME,
                                            CUBE_TASK_VOICE_STACK, NULL,
                                            CUBE_TASK_VOICE_PRIO,
                                            &s_task, CUBE_TASK_VOICE_CORE) == pdPASS;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "voice task");
    ESP_LOGI(TAG, "voice frontend running — 说「%s」唤醒", VOICE_WAKE_PHRASE);
    return ESP_OK;
}

#else /* !VOICE: 空实现(链路仅编译, 语音功能关闭) */

esp_err_t app_voice_start(void)
{
    return ESP_OK;
}

#endif
