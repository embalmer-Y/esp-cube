# speech_provider —— 云端语音三通道(ASR/TTS/LLM), 移植自 Kaleido
# kaleido_speech(V-33 百炼链路真机验证, API 前缀 ks_ 保留以便对照上游)
# sp_minimax: MiniMax TTS(HTTPS SSE), sp_dashscope: DashScope ASR+TTS(WS)
# sp_llm: OpenAI 兼容 chat/completions SSE; gs_r46.pem: DashScope 根证书
idf_component_register(EMBED_FILES "gs_r46.pem"
                       SRCS "src/sp_dashscope.c" "src/sp_minimax.c"
                            "src/sp_llm.c" "src/sp_sess.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_websocket_client esp_http_client espressif__cjson
                       PRIV_REQUIRES esp_timer mbedtls espressif__cjson)
