/* pair_server.h —— 配对 HTTPS 服务装配(T15 续; L5)
 *
 * 职责: 证书灌入(NVS "pairc" 区, base64 分块经串口 CLI)/配对窗口/
 * esp_https_server 生命周期/配对端点(加 /v1/pair 前缀, docs/20 §3/§4)。
 * M1 口径(批次 19): 管理通道(/v1 其余端点, client 证书 REQUIRED)
 * 与 T17 服务凭据端点同批落地; 本批只开配对端点 + /v1/device 只读。
 */
#pragma once

#include <stdbool.h>

/* 证书灌入(串口分块): append 累积 base64, commit 解码入 NVS */
int pair_cert_append(const char *which, const char *b64);
int pair_cert_commit(const char *which);
void pair_cert_status(void);

/* 配对窗口(CLI 触发; T16 换物理触摸) */
int pair_window_open_cmd(void);
/* 最近一次开窗的 PIN(6 位数字, 无窗为空串) */
void pair_window_pin(char out[7]);
/* 已配对客户端数(CLI 展示用) */
int pair_client_count_pub(void);

/* https 服务启停(证书齐备才允许 start) */
int pair_server_start(void);
int pair_server_stop(void);
bool pair_server_running(void);

/* T17: LLM 密钥 provider(ai_session 注入; credstore "llm.api_key"
 * 优先, 未配置返回 NULL → Kconfig 兜底); 串由本组件缓存持有 */
const char *pair_server_llm_key(void);

/* T30: settings blob JSON 文本(llmprofile 来源; 未配置返回 NULL) */
const char *pair_server_profile_settings(void);
