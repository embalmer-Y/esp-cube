/*
 * xiaozhi_client.c —— xiaozhi 协议适配器(里程碑2骨架)
 *
 * 协议(78/xiaozhi-esp32 docs/websocket.md):
 *   - 握手: 连接后发 hello JSON(设备信息 + audio_params: opus/16k/1ch/60ms帧)
 *   - 二进制帧: Opus 编码音频(60ms = 960 samples @16k)
 *   - 文本帧: JSON 事件(listen start/stop/detect, stt, tts start/stop,
 *             sentence_start, llm, mcp, abort)
 *   - 鉴权: HTTP 头 Authorization: Bearer <token> + Device-Id
 * 服务端: 自建 xinnan-tech/xiaozhi-esp32-server(LLM=智谱 glm-4-flash 免费)
 */
#include "ai_session.h"

#include <string.h>
#include "esp_log.h"
/* M2 启用: #include "esp_websocket_client.h" */

static const char *TAG = "xiaozhi";

/* ---- hello 握手帧(xiaozhi 协议 v3) ---- */
static const char *HELLO_JSON =
    "{"
    "\"type\":\"hello\","
    "\"version\":3,"
    "\"features\":{"
        "\"mcp\":true,"
        "\"mcp_from_server\":true"
    "},"
    "\"transport\":\"websocket\","
    "\"audio_params\":{"
        "\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60"
    "}"
    "}";

static esp_err_t xz_connect(void)
{
    /* TODO(M2):
     * 1. esp_websocket_client_init(wss 地址 + Authorization 头);
     * 2. 连接后发送 HELLO_JSON;
     * 3. 注册事件: 收到 hello ack 后进入可会话状态。 */
    ESP_LOGI(TAG, "connect (M2 skeleton)");
    return ESP_OK;
}

static int xz_send_audio(const int16_t *pcm, int frames)
{
    /* TODO(M2): Opus 编码(esp_audio_codec opus encoder, 960 samples/帧)
     *           → esp_websocket_client_send_bin */
    (void)pcm; (void)frames;
    return 0;
}

static esp_err_t xz_send_ctrl(const char *json)
{
    /* TODO(M2): esp_websocket_client_send_text */
    (void)json;
    return ESP_OK;
}

static esp_err_t xz_abort(void)
{
    /* 打断: 发 {"type":"abort"} → 停止渲染下行 TTS */
    return xz_send_ctrl("{\"type\":\"abort\"}");
}

const ai_backend_t g_xiaozhi_backend = {
    .name = "xiaozhi",
    .connect = xz_connect,
    .disconnect = NULL,
    .send_audio = xz_send_audio,
    .send_ctrl = xz_send_ctrl,
    .abort = xz_abort,
};
