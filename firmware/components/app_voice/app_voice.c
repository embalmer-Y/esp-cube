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
 * 管线: drv_audio_mic_read(48k→16k 单麦) → AFE("M") → WakeNet
 *       → 唤醒事件 CUBE_EVT_WAKE_DETECTED → ai_session_on_wake()
 * AEC 参考(放歌打断场景)与 MultiNet 命令词留 M2.2+; 单麦无 BSS。
 */
#include "app_voice.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "drv_audio.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

#if CONFIG_ESP_CUBE_ENABLE_VOICE

static const char *TAG = "voice";

static TaskHandle_t s_task;
static volatile bool s_running;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;

/* ---- 唤醒前端任务 ---- */
static void voice_task(void *arg)
{
    (void)arg;
    int consec_fail = 0;

    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    const int ch_num = s_afe->get_feed_channel_num(s_afe_data);
    ESP_LOGI(TAG, "AFE chunk=%d feed_ch=%d rate=%d", chunk, ch_num,
             s_afe->get_samp_rate(s_afe_data));

    const int frame_frames = chunk * ch_num;     /* feed 需要的 16k 帧数 */
    int16_t *feed_buf = malloc(frame_frames * 2);
    ESP_RETURN_ON_FALSE(feed_buf != NULL, , TAG, "feed buf");

    int detect_count = 0;
    TickType_t last_hb = xTaskGetTickCount();
    int reads = 0;
    for (;;) {
        if (!s_running) break;
        /* 采集: 分片续读(部分数据不丢); 顽固失败重启 DMA(V-04 补丁) */
        /* drv_audio_mic_read 读到 48k 立体声(与播放同 LRCK);
         * 3:1 均值抽取 → AFE 需要的 16k 单声道 */
        int frames48 = frame_frames * 3;
        int16_t *buf48 = malloc((size_t)frames48 * 2 * 2);
        if (buf48 == NULL) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int got48 = drv_audio_mic_read(buf48, frames48);
        int got = 0;
        if (got48 > 0) {
            for (int i = 0, o = 0; i + 2 < got48; i += 3, o++) {
                int32_t acc = (int32_t)buf48[i * 2] + buf48[(i + 1) * 2] +
                              buf48[(i + 2) * 2];
                if (o < frame_frames) feed_buf[o] = (int16_t)(acc / 3);
                got = o + 1 <= frame_frames ? o + 1 : got;
            }
        }
        free(buf48);
        if (got < frame_frames) {
            consec_fail++;
            if (consec_fail % 20 == 1) {
                ESP_LOGE(TAG, "mic starved x%d (got48=%d)", consec_fail, got48);
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

    /* ---- AFE: 单麦 SR 高性能模式 ---- */
    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR,
                                            AFE_MODE_HIGH_PERF);
    ESP_RETURN_ON_FALSE(afe_cfg != NULL, ESP_FAIL, TAG, "afe cfg");
    s_afe = esp_afe_handle_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe != NULL, ESP_FAIL, TAG, "afe handle");
    s_afe_data = s_afe->create_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_FAIL, TAG, "afe data");

    /* S31 自动挂载缺失——显式添加(V-04 W-5) */
    int rc = s_afe->add_wakenet_model(s_afe_data, "wn10_hilexin");
    ESP_LOGW(TAG, "add_wakenet_model(wn10_hilexin) → %d", rc);
    s_afe->print_pipeline(s_afe_data);
    s_afe->set_wakenet_threshold(s_afe_data, 1, 0.60);

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, "voice", 8192, NULL,
                                            18, &s_task, 0) == pdPASS;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "voice task");
    ESP_LOGI(TAG, "voice frontend running — 说「Hi,乐鑫」唤醒");
    return ESP_OK;
}

#else /* !VOICE: 空实现(链路仅编译, 语音功能关闭) */

esp_err_t app_voice_start(void)
{
    return ESP_OK;
}

#endif
