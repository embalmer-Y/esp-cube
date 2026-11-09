/*
 * ai_session.c —— AI 会话状态机(里程碑2骨架)
 *
 * 状态迁移(设计文档 §5.2):
 *   IDLE →(唤醒)→ WAKE →(提示音+duck)→ LISTENING →(VAD静音)→ THINKING
 *   THINKING →(TTS到达)→ SPEAKING →(打断/完成)→ LISTENING/IDLE
 * ducking 联动: 进入 SPEAKING → audio_mixer_set_ducking(true); 回 IDLE → false。
 */
#include "ai_session.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "audio_mixer.h"

static const char *TAG = "ai_session";

static struct {
    ai_state_t state;
    const ai_backend_t *backend;
    TaskHandle_t task;
    volatile bool running;
} s_ai;

static void set_state(ai_state_t st)
{
    s_ai.state = st;
    /* ducking 只在 AI 出声时压低音乐 */
    audio_mixer_set_ducking(st == AI_SPEAKING);
    cube_event_publish(CUBE_EVT_AI_STATE, &st, sizeof(int));
    ESP_LOGI(TAG, "state -> %d", st);
}

void ai_session_on_wake(void)
{
    if (s_ai.state == AI_IDLE) {
        set_state(AI_WAKE);
        /* TODO(M2): 播提示音(AUDIO_SRC_TONE); backend->connect(); send_ctrl("listen start") */
        set_state(AI_LISTENING);
    }
}

/* 本地命令词短路(不进云端, 直接执行); 全量派发: 先过滤事件类型 */
static void on_voice_cmd(cube_evt_id_t id, const void *data, size_t len, void *ctx)
{
    (void)len; (void)ctx;
    if (id != CUBE_EVT_VOICE_CMD || data == NULL) return;
    int cmd = *(const int *)data;
    int vol = audio_mixer_get_master_volume();
    switch (cmd) {
    case VOICE_CMD_VOLUME_UP:
        if (vol < 100) audio_mixer_set_master_volume(vol + 10);
        break;
    case VOICE_CMD_VOLUME_DOWN:
        if (vol > 0) audio_mixer_set_master_volume(vol - 10);
        break;
    /* TODO(M2): 暂停/播放/切歌 → player/AVRCP 透传 */
    default:
        break;
    }
}

static void ai_task(void *arg)
{
    ESP_LOGI(TAG, "AI session task (M2 skeleton), backend=%s",
             s_ai.backend ? s_ai.backend->name : "none");
    while (s_ai.running) {
        /* TODO(M2): TTS 下行 Opus 解码 → 重采样 → 混音器 TTS 通道 */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

esp_err_t ai_session_start(void)
{
    if (s_ai.task != NULL) return ESP_OK;
    s_ai.running = true;
    cube_event_subscribe(on_voice_cmd, NULL);
    if (xTaskCreatePinnedToCore(ai_task, "ai", 16384, NULL, 10,
                                &s_ai.task, 1) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
