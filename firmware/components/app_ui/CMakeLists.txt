# app_ui: LVGL9 UI(60Hz 刷新/频谱/3D立方体)
idf_component_register(
    SRCS "app_ui.c" "ui_pages.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg drv_display app_audio app_player app_bt
        espressif__esp_lvgl_port lvgl__lvgl
)
