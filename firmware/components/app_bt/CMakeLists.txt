# app_bt [L3 应用]: 经典蓝牙 A2DP Sink + AVRCP(手机蓝牙音乐)
# 源码按 CONFIG_ESP_CUBE_ENABLE_BT 整体守卫(P4 无 BT 控制器时空编译);
# bt/esp_coex 组件仅 S31 目标引入
set(app_bt_reqs cubecore app_audio esp_common nvs_flash esp_timer)
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32s31")
    list(APPEND app_bt_reqs bt)
endif()

idf_component_register(
    SRCS "app_bt.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${app_bt_reqs}
)
