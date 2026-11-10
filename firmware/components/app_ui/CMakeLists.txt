# app_ui [L4 表现]: LVGL9 氛围 UI(只依赖 ui_model.h 契约, 依赖规则见 docs/06)
# ui/ 子树平台无关: 固件与 sim/ 同源编译, UI 代码零分叉;
# 设备后端 ui_port_esp.c 在 main(L5), 模拟器后端 mock_app.c 在 sim/。
idf_component_register(
    SRCS "app_ui.c" "ui/ui_pages.c" "ui/ui_render.c" "ui/ui_lyrics.c"
         "ui/ui_widget.c" "ui/ui_font_cjk18.c" "ui/ui_font_cjk24.c"
    INCLUDE_DIRS "include" "ui"
    PRIV_REQUIRES cubecore espressif__esp_lvgl_port lvgl__lvgl
)
