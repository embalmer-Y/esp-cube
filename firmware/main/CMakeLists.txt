# main [L5 装配]: 只负责模块初始化顺序/失败域策略/ui_port_esp 后端; 唯一可横跨全层(依赖规则见 docs/06)
set(main_priv_reqs
    cubecore boardcfg
    drv_display drv_audio drv_touch
    app_audio app_player app_net app_ncm app_ui
    app_bt app_voice app_ai_session ai_skills app_assist app_soul app_storage
    cube_act persona pair credstore
    espressif__esp_lvgl_port
    nvs_flash esp_wifi esp_event esp_netif esp_timer esp_driver_uart esp_http_client
    esp_https_server esp_driver_usb_serial_jtag
)
# app_bt 组件双目标存在(P4 上空编译)保留依赖供头文件路径;
# esp_coex 仅 S31(P4 无 BT 控制器)
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32s31")
    list(APPEND main_priv_reqs esp_coex)
endif()

# 表情观测(faceperf): 只在 CONFIG_ESP_CUBE_FACE_PERF 下编译
set(FACE_PERF_SRCS)
if(CONFIG_ESP_CUBE_FACE_PERF)
    list(APPEND FACE_PERF_SRCS "face_perf.c" "face_perf_stats.c")
endif()

idf_component_register(
    SRCS "main.c" "ui_port_esp.c" "cli.c" "ai_skills_setup.c" "act_gate.c" "pair_server.c"
         "mem_budget.c" ${FACE_PERF_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${main_priv_reqs}
)

# esp_efc_codec 仅随表情开关引入(ui_port_esp.c 直接调用解码器 API, face_perf.c 包含 lv_efc.h)。
# 与 app_ui 相同用 optional 形式: 写在 if 内的 PRIV_REQUIRES 会被需求预扫描忽略。
if(CONFIG_ESP_CUBE_ENABLE_FACE_ANIM)
    idf_component_optional_requires(PRIVATE espressif__esp_efc_codec)
endif()
