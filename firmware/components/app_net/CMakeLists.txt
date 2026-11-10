# app_net [L2 服务]: WiFi STA 管理(连接服务)
idf_component_register(
    SRCS "app_net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore esp_wifi esp_netif esp_event nvs_flash esp_timer
)
