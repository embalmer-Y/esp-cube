# app_ui [L4 表现]: LVGL9 氛围 UI(只依赖 ui_model.h 契约, 依赖规则见 docs/06)
idf_component_register(
    SRCS "app_ui.c" "ui_pages.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg drv_display app_audio app_player app_bt
        espressif__esp_lvgl_port lvgl__lvgl
)
