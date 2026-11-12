# main [L5 装配]: 只负责模块初始化顺序/失败域策略/ui_port_esp 后端; 唯一可横跨全层(依赖规则见 docs/06)
set(main_priv_reqs
    cubecore boardcfg
    drv_display drv_audio drv_touch
    app_audio app_player app_net app_ncm app_ui
    app_bt app_voice app_ai_session ai_skills app_assist app_soul app_storage
    espressif__esp_lvgl_port
    nvs_flash esp_wifi esp_event esp_netif esp_timer esp_driver_uart esp_http_client
)
# app_bt 组件双目标存在(P4 上空编译)保留依赖供头文件路径;
# esp_coex 仅 S31(P4 无 BT 控制器)
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32s31")
    list(APPEND main_priv_reqs esp_coex)
endif()

idf_component_register(
    SRCS "main.c" "ui_port_esp.c" "cli.c" "ai_skills_setup.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${main_priv_reqs}
)
