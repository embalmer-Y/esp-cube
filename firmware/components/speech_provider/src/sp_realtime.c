/*
 * sp_realtime.c — 快线 provider: qwen-omni realtime(OpenAI Realtime 风格 WS)
 *
 * B3 快慢路径的"嘴和耳"(Kaleido realtime 快线方案, 2026-11-12 PC 探针
 * 全环验证): 一条常驻连接承载 语音上行/转写/即兴语音下行/文本注入播报。
 * 慢线(paraformer+文本 LLM FC)保工具可靠性, 本线只负责自然交互。
 *
 * 协议事实(PC 探针 /tmp/rt_probe.mjs, 2026-11-12):
 *   - 端点 wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=...
 *     (3.8-omni 旧域名即可, 0.2s 建连), Bearer 鉴权
 *   - session.created → session.update(voice/instructions/server_vad/
 *     tool_choice=none) → 常驻
 *   - 上行 input_audio_buffer.append(base64 PCM16 16k, 100ms/3200B)
 *   - server_vad 自动判停应答; 转写事件流式(可靠性弱, 仅 UI 参考)
 *   - 下行 response.audio.delta(base64 PCM16 24k mono) → 解码回调
 *   - 注入播报 response.create{response:{instructions}}; 探针实测
 *     "Cannot create response while another response is in progress"
 *     → say() 前置 response.cancel
 *   - 音色: 默认 Tina; 支持复刻(qwen-omni-voice-cloning), 填克隆 ID
 */
#include "speech_provider.h"
#include "sp_internal.h"

#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#define RT_WS_BUF       16384

/* GlobalSign R46 根(与 DashScope 同锚定, 符号定义见 sp_dashscope.c) */
extern const char gs_r46_pem_start[] asm("_binary_gs_r46_pem_start");
#define RT_TX_TIMEOUT   8000
#define RT_PCM_CHUNK    3200    /* 100ms@16k s16le 上行分片 */

static const char *TAG = "ks-rt";

struct ks_rt_sess {
	char *uri;
	char *auth;
	esp_websocket_client_handle_t ws;
	SemaphoreHandle_t created;   /* session.created */
	struct ks_rt_events ev;
	uint8_t pend[RT_PCM_CHUNK];  /* 上行音频攒片 */
	size_t pend_len;
	char b64[RT_PCM_CHUNK * 4 / 3 + 8];     /* 会话持有: 曾用 8.6KB 栈
	                                         * 缓冲打爆 mic_task(板上
	                                         * Stack protection fault) */
	char frame[RT_PCM_CHUNK * 4 / 3 + 128];
	uint8_t dec[4096];  /* 音频帧解码缓冲: 去 WS 任务 malloc/free */
	volatile bool resp_busy;     /* response 进行中(say 前需 cancel) */
	volatile bool up;            /* session.created 后在位; 断线即失效,
	                              * 由上层下次唤醒时 close+open 重建 */
	volatile bool closing;
};

static struct ks_rt_sess *s_rt;

/* ---- 会话配置帧 ---- */
static char *session_update_build(const struct ks_rt_cfg *c)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *ss = cJSON_CreateObject();
	cJSON *td = cJSON_CreateObject();
	char *s;

	cJSON_AddStringToObject(root, "type", "session.update");
	cJSON_AddItemToObject(root, "session", ss);
	{
		cJSON *mods = cJSON_CreateArray();
		cJSON_AddItemToArray(mods, cJSON_CreateString("text"));
		cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));
		cJSON_AddItemToObject(ss, "modalities", mods);
	}
	cJSON_AddStringToObject(ss, "input_audio_format", "pcm16");
	cJSON_AddStringToObject(ss, "output_audio_format", "pcm16");
	cJSON_AddStringToObject(ss, "voice", c->voice);
	cJSON_AddStringToObject(ss, "instructions", c->instructions);
	/* 转写订阅省略(v1): bool 形式会被服务端拒(板上实测 Invalid
	 * session.update payload); 对象形 schema 未验证, 要字幕再开 */
	cJSON_AddStringToObject(ss, "tool_choice", "none");
	/* server_vad: 用户停说自动应答(快线即兴的前提) */
	cJSON_AddStringToObject(td, "type", "server_vad");
	cJSON_AddItemToObject(ss, "turn_detection", td);

	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

/* ---- 事件解析 ---- */
static void rt_ws_event(void *arg, esp_event_base_t base, int32_t id,
			void *data)
{
	esp_websocket_event_data_t *e = data;

	if (!s_rt || s_rt->closing)
		return;
	if (id == WEBSOCKET_EVENT_CONNECTED)
		return;
	if (id == WEBSOCKET_EVENT_DISCONNECTED) {
		ESP_LOGW(TAG, "disconnected -> rt down");
		s_rt->up = false;
		return;
	}
	if (id != WEBSOCKET_EVENT_DATA || !e->data_ptr || !e->data_len)
		return;
	if (e->op_code != WS_TRANSPORT_OPCODES_TEXT)
		return;                     /* 快线无二进制帧 */

	cJSON *root = cJSON_ParseWithLength(e->data_ptr, e->data_len);
	if (root == NULL)
		return;
	const cJSON *jt = cJSON_GetObjectItem(root, "type");
	const char *t = cJSON_IsString(jt) ? jt->valuestring : "";

	if (strcmp(t, "session.created") == 0) {
		s_rt->up = true;
		xSemaphoreGive(s_rt->created);
	} else if (strcmp(t, "session.updated") == 0) {
		ESP_LOGI(TAG, "session ready");
	} else if (strcmp(t, "response.created") == 0) {
		s_rt->resp_busy = true;
		ESP_LOGI(TAG, "rt response start");
	} else if (strcmp(t, "response.done") == 0) {
		s_rt->resp_busy = false;
	} else if (strcmp(t, "response.audio.delta") == 0) {
		/* base64 PCM16 24k mono → 解码直交上层(网络任务上下文,
		 * 上层只许写环形缓冲) */
		const cJSON *d = cJSON_GetObjectItem(root, "delta");
		if (cJSON_IsString(d) && d->valuestring[0]) {
			size_t olen = 0;
			if (mbedtls_base64_decode(s_rt->dec,
					sizeof(s_rt->dec), &olen,
					(const uint8_t *)d->valuestring,
					strlen(d->valuestring)) == 0 &&
			    olen > 0 && s_rt->ev.on_audio) {
				s_rt->ev.on_audio(s_rt->ev.user, s_rt->dec,
						  olen);
			}
		}
	} else if (strcmp(t,
			  "conversation.item.input_audio_transcription.completed") == 0) {
		const cJSON *d = cJSON_GetObjectItem(root, "transcript");
		if (cJSON_IsString(d) && s_rt->ev.on_transcript)
			s_rt->ev.on_transcript(s_rt->ev.user, d->valuestring);
	} else if (strcmp(t, "error") == 0) {
		const cJSON *err = cJSON_GetObjectItem(root, "error");
		const cJSON *msg = err ? cJSON_GetObjectItem(err, "message")
				       : NULL;
		ESP_LOGW(TAG, "rt error: %s",
			 cJSON_IsString(msg) ? msg->valuestring : "?");
	}
	cJSON_Delete(root);
}

int ks_rt_open(const struct ks_rt_cfg *cfg, const struct ks_rt_events *ev)
{
	char *upd = NULL;

	if (!cfg || !cfg->url || !cfg->api_key || !cfg->voice || !ev)
		return -1;
	if (s_rt)
		return -1;                  /* 已开(常驻) */
	s_rt = calloc(1, sizeof(*s_rt));
	if (!s_rt)
		return -1;
	s_rt->ev = *ev;
	s_rt->created = xSemaphoreCreateBinary();
	if (!s_rt->created)
		goto fail;

	{
		size_t n = strlen(cfg->url);
		s_rt->uri = malloc(n + 1);
		s_rt->auth = malloc(192);
		if (!s_rt->uri || !s_rt->auth)
			goto fail;
		memcpy(s_rt->uri, cfg->url, n + 1);
		snprintf(s_rt->auth, 192,
			 "Authorization: Bearer %s\r\n", cfg->api_key);
	}

	esp_websocket_client_config_t wc = {
		.buffer_size = RT_WS_BUF,
		.network_timeout_ms = 10000,
		.disable_auto_reconnect = true,  /* 半死会话不如整建 */
		.cert_pem = gs_r46_pem_start,   /* 与 DashScope 同根(GlobalSign R46) */
		.task_stack = 8192,
	};
	wc.uri = s_rt->uri;
	wc.headers = s_rt->auth;
	s_rt->ws = esp_websocket_client_init(&wc);
	if (!s_rt->ws)
		goto fail;
	if (esp_websocket_register_events(s_rt->ws, WEBSOCKET_EVENT_ANY,
					  rt_ws_event, NULL) != ESP_OK)
		goto fail;
	if (esp_websocket_client_start(s_rt->ws) != ESP_OK)
		goto fail;
	if (!xSemaphoreTake(s_rt->created, pdMS_TO_TICKS(10000))) {
		ESP_LOGE(TAG, "rt connect timeout");
		goto fail;
	}

	upd = session_update_build(cfg);
	if (!upd)
		goto fail;
	if (esp_websocket_client_send_text(s_rt->ws, upd, strlen(upd),
					   RT_TX_TIMEOUT) < 0) {
		free(upd);
		goto fail;
	}
	free(upd);
	ESP_LOGI(TAG, "rt open ok");
	return 0;
fail:
	free(upd);
	ks_rt_close();
	return -2;
}

void ks_rt_close(void)
{
	if (!s_rt)
		return;
	s_rt->closing = true;
	if (s_rt->ws) {
		esp_websocket_client_close(s_rt->ws, pdMS_TO_TICKS(1500));
		vTaskDelay(pdMS_TO_TICKS(50));
		esp_websocket_client_destroy(s_rt->ws);
	}
	if (s_rt->created)
		vSemaphoreDelete(s_rt->created);
	free(s_rt->uri);
	free(s_rt->auth);
	free(s_rt);
	s_rt = NULL;
}

bool ks_rt_ok(void)
{
	return s_rt != NULL && !s_rt->closing && s_rt->up;
}

int ks_rt_feed(const int16_t *pcm, size_t samples)
{
	if (!s_rt)
		return -2;
	const uint8_t *p = (const uint8_t *)pcm;
	size_t len = samples * 2;

	while (len) {
		size_t n = RT_PCM_CHUNK - s_rt->pend_len;
		if (n > len)
			n = len;
		memcpy(s_rt->pend + s_rt->pend_len, p, n);
		s_rt->pend_len += n;
		p += n;
		len -= n;
		if (s_rt->pend_len == RT_PCM_CHUNK) {
			size_t olen = 0;
			if (mbedtls_base64_encode((uint8_t *)s_rt->b64,
					sizeof(s_rt->b64), &olen, s_rt->pend,
					RT_PCM_CHUNK) != 0)
				return -3;
			int fl = snprintf(s_rt->frame, sizeof(s_rt->frame),
				"{\"type\":\"input_audio_buffer.append\","
				"\"audio\":\"%.*s\"}", (int)olen,
				s_rt->b64);
			if (esp_websocket_client_send_text(s_rt->ws,
					s_rt->frame, fl, RT_TX_TIMEOUT) < 0)
				return -4;
			s_rt->pend_len = 0;
		}
	}
	return (int)samples;
}

int ks_rt_say(const char *text)
{
	if (!s_rt || !text || !text[0])
		return -1;
	/* 探针实测: 前一 response 未完时 create 会被拒 → 先 cancel */
	if (s_rt->resp_busy) {
		static const char cancel[] =
			"{\"type\":\"response.cancel\"}";
		esp_websocket_client_send_text(s_rt->ws, cancel,
			sizeof(cancel) - 1, RT_TX_TIMEOUT);
		s_rt->resp_busy = false;
		vTaskDelay(pdMS_TO_TICKS(60));
	}
	char frame[640];
	/* 批次 44: text 来自技能回执/提醒原文, 含 " 或 \ 时手拼帧非法
	 * JSON(服务端拒)——最小转义后再拼(走 cJSON 的路径已有转义) */
	char esc[448];
	size_t el = 0;
	for (const char *p = text; *p && el + 7 < sizeof(esc); p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			esc[el++] = '\\';
			esc[el++] = (char)c;
		} else if (c == '\n' || c == '\r') {
			esc[el++] = ' ';
		} else {
			esc[el++] = (char)c;
		}
	}
	esc[el] = '\0';
	int fl = snprintf(frame, sizeof(frame),
		"{\"type\":\"response.create\",\"response\":"
		"{\"instructions\":\"用一句口语化中文播报: %s\"}}", esc);
	if (fl <= 0 || (size_t)fl >= sizeof(frame))
		return -3;
	if (esp_websocket_client_send_text(s_rt->ws, frame, fl,
					   RT_TX_TIMEOUT) < 0)
		return -4;
	return 0;
}

int ks_rt_cancel(void)
{
	static const char cancel[] = "{\"type\":\"response.cancel\"}";

	if (!s_rt)
		return -2;
	s_rt->resp_busy = false;
	if (esp_websocket_client_send_text(s_rt->ws, cancel,
			sizeof(cancel) - 1, RT_TX_TIMEOUT) < 0)
		return -4;
	return 0;
}
