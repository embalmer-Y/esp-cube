# app_bt [L3 应用]: 经典蓝牙 A2DP Sink + AVRCP(手机蓝牙音乐)
idf_component_register(
    SRCS "app_bt.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore app_audio bt
        esp_common nvs_flash esp_timer
)
