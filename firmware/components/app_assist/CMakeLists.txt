# app_assist [L3 应用]: 语音助理本地服务(M2 琐事/提醒 + SNTP 时间底座)
# 后续 M3 soul/记忆的设备侧服务同样落此层
idf_component_register(
    SRCS "app_assist.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore nvs_flash esp_netif esp_timer
)
