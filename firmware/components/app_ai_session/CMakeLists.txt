# app_ai_session: AI 会话客户端(xiaozhi 协议 → 自建server+GLM; 预留 GLM-Realtime)
# 里程碑2启用: Kconfig CONFIG_ESP_CUBE_ENABLE_AI=y
if(CONFIG_ESP_CUBE_ENABLE_AI)
    idf_component_register(
        SRCS "ai_session.c" "xiaozhi_client.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES cubecore app_audio app_voice app_player
            espressif__esp_websocket_client espressif__esp_audio_codec json
    )
else()
    idf_component_register(INCLUDE_DIRS "include")
endif()
