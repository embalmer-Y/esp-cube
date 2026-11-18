# app_net [L2 服务]: WiFi STA 管理(连接服务) + P3a SoftAP 首配窗口
# P4C5 目标经 C5 共芯(esp_hosted + esp_wifi_remote 透传)
set(app_net_reqs cubecore esp_wifi esp_netif esp_event nvs_flash esp_timer
                 protocomm espressif__network_provisioning)
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32p4")
    list(APPEND app_net_reqs esp_hosted)
endif()

idf_component_register(
    SRCS "app_net.c" "app_prov.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${app_net_reqs}
)
