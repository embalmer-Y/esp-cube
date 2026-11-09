# app_audio: 音频引擎(混音器 + ducking + AEC 参考抽头)
idf_component_register(
    SRCS "audio_mixer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg drv_audio
        esp_common freertos
)
