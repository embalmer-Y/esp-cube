# app_player: 网络音源(ICP/ICY 流 + 解码 → 混音器)
idf_component_register(
    SRCS "app_player.c" "icy_stream.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore app_audio espressif__esp_audio_codec mp3_helix esp_timer
        esp_http_client esp-tls
)
