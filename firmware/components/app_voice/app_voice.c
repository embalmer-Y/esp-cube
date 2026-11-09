/*
 * app_voice.c —— 语音前端实现骨架(里程碑2)
 *
 * ⚠ 本文件为 M2 骨架: 编译前需启用 esp-sr 依赖与模型分区(见 CMakeLists 注释)。
 * 实现顺序(调试验证步骤):
 *   1. AFE 通道打通: 麦克风 48k→16k 降采样 + 混音器 AEC 参考取流;
 *   2. WakeNet10 唤醒验证(免费词 wn10_nihaoxiaozhi);
 *   3. MultiNet mn7_cn 本地命令词;
 *   4. 放歌场景下 AEC 打断效果调参(全双工 AEC 模式)。
 *
 * esp-sr 关键 API(以 v2.6.x 头文件为准, 编译时核对):
 *   afe_create_config_t / afe_config_t(.aec_mode=AEC_MODE_FD_LOW_COST,
 *     .vad_mode, .wakenet_model_name="wn10_nihaoxiaozhi")
 *   esp_afe_sr_v1.0 接口: esp_afe_sr_from_config() → esp_afe_feed()/fetch()
 */
#include "app_voice.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "drv_audio.h"

static const char *TAG = "voice";

#define SR_RATE         16000       /* esp-sr 输入采样率(固定) */
#define AFE_FRAME_MS    30          /* AFE 处理帧长(默认30ms) */
#define SR_FRAME        (SR_RATE / 1000 * AFE_FRAME_MS)

static struct {
    TaskHandle_t task;
    volatile bool running;
    int16_t mic_ring[SR_FRAME * 4];     /* 麦克风积累缓冲 */
    volatile size_t mic_level;
    int16_t ref_ring[SR_FRAME * 4];
    volatile size_t ref_level;
} s_voice;

/* ---- AEC 参考: 混音器输出(48k立体声) → 单声道16k ----
 * 在混音任务上下文执行, 只做降采样与入队, 不得阻塞。 */
static void on_mixer_ref(const int16_t *stereo, int frames_48k, void *ctx)
{
    (void)ctx;
    /* 每 3 点取均值: 48k → 16k; TODO(M2): 写入 AFE 参考环形缓冲(帧粒度对齐) */
    int16_t mono16k[64];
    int out = 0;
    for (int i = 0; i + 2 < frames_48k && out < (int)(sizeof(mono16k) / sizeof(mono16k[0])); i += 3, out++) {
        mono16k[out] = (int16_t)(((int32_t)stereo[i * 2] + stereo[i * 2 + 1]) >> 1);
    }
    (void)mono16k;
}

/* ---- 语音前端任务 ---- */
static void voice_task(void *arg)
{
    /* TODO(M2): esp_afe_sr 初始化(需要 psram + model 分区) */
    ESP_LOGI(TAG, "voice frontend task (M2 skeleton)");
    while (s_voice.running) {
        /* TODO(M2): AFE feed(mic+ref) → fetch → wakenet detect → multinet */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

esp_err_t app_voice_start(void)
{
    if (s_voice.task != NULL) return ESP_OK;
    /* 注册 AEC 参考抽头: 参考信号=最终输出(ducking后), 与扬声器同源 */
    audio_mixer_set_ref_sink(on_mixer_ref, NULL);
    s_voice.running = true;
    if (xTaskCreatePinnedToCore(voice_task, "voice", 12288, NULL, 18,
                                &s_voice.task, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
