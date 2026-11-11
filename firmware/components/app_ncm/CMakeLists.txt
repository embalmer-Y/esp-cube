# app_ncm [L2 云服务]: 网易云音乐游客客户端(weapi 加密 + 搜索/取播放地址)
# 同步阻塞 API(装配层 CLI/事件编排调用); 播放由 app_player 承载(L5 编排)
idf_component_register(
    SRCS "app_ncm.c" "ncm_weapi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp-tls mbedtls espressif__cjson esp_timer
)
