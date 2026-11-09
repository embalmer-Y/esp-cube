# main: 装配层 —— 只负责模块初始化顺序与演示逻辑, 不含具体功能实现
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        cubecore boardcfg
        drv_display drv_audio
        app_audio app_player app_net app_ui
        app_bt app_voice app_ai_session
        nvs_flash esp_wifi esp_event esp_netif esp_timer
)
