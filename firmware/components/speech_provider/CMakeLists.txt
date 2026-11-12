# speech_provider [L2 服务] —— 云端语音三通道(ASR/TTS/LLM), 移植自 Kaleido
# kaleido_speech(V-33 百炼链路真机验证, API 前缀 ks_ 保留以便对照上游)
# sp_minimax: MiniMax TTS(HTTPS SSE), sp_dashscope: DashScope ASR+TTS(WS)
# sp_llm: OpenAI 兼容 SSE; sp_realtime: omni 快线(Realtime WS); gs_r46.pem: 根证书
idf_component_register(EMBED_FILES "gs_r46.pem"
                       SRCS "src/sp_dashscope.c" "src/sp_minimax.c"
                            "src/sp_llm.c" "src/sp_realtime.c" "src/sp_sess.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_websocket_client esp_http_client espressif__cjson
                       PRIV_REQUIRES esp_timer mbedtls)
