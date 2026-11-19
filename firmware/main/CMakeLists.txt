
# ---- A1a: esp_hosted DMA 分配补丁(幂等, configure 期自动应用) ----
# manager 版本不支持 manifest patches 字段, 工程侧实现; 内容=非 aligned
# dma_alloc 补 PREFER_SPIRAM 优先序(hosted 15872 聚合缓冲→EDMA PSRAM,
# docs/22 §3C/§3E; 与 CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM=y 配套)。
set(_hosted_dma ${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_hosted/port/os/idf/src/eh_host_port_dma.c)
if(EXISTS ${_hosted_dma})
    file(READ ${_hosted_dma} _dma_src)
    if(NOT _dma_src MATCHES "EspCube A1a")
        execute_process(
            COMMAND git apply ${CMAKE_CURRENT_LIST_DIR}/patches/esp_hosted_dma_prefer_spiram.patch
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_hosted
            RESULT_VARIABLE _rc)
        if(_rc EQUAL 0)
            message(STATUS "A1a: esp_hosted DMA patch applied")
        else()
            message(WARNING "A1a: esp_hosted DMA patch FAILED rc=${_rc}")
        endif()
    endif()
endif()
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
