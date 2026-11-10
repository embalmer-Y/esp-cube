# main [L5 装配]: 只负责模块初始化顺序/失败域策略/ui_port_esp 后端; 唯一可横跨全层(依赖规则见 docs/06)
idf_component_register(
    SRCS "main.c" "ui_port_esp.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        cubecore boardcfg
        drv_display drv_audio
        app_audio app_player app_net app_ui
        app_bt app_voice app_ai_session
        espressif__esp_lvgl_port
        nvs_flash esp_wifi esp_event esp_netif esp_timer esp_coex
)
