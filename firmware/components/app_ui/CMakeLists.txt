# app_ui [L4 表现]: LVGL9 氛围 UI(只依赖 ui_model.h 契约, 依赖规则见 docs/06)
# ui/ 子树平台无关: 固件与 sim/ 同源编译, UI 代码零分叉;
# 设备后端 ui_port_esp.c 在 main(L5), 模拟器后端 mock_app.c 在 sim/。
# M4: 音频可视化(频谱/歌词)退役, 表情常驻 UI。
# 乐鑫官方情绪素材(.eaf, espressif2022/esp_emote_assets 360_360; EMBED 进
# flash rodata; 经 Kconfig ESP_CUBE_ENABLE_FACE_ANIM 启用)
# mood 映射(首批 3 片段): 待机/聆听/其他情绪=neutral, happy/relaxed=Happy,
# 眨眼=wink; 素材重编码管线=tools/eaf_codec.py(460 upscale)
set(FACE_ASSETS
    "ui/assets/face_neutral.eaf"
    "ui/assets/face_happy.eaf"
    "ui/assets/face_wink.eaf")
idf_component_register(
    SRCS "app_ui.c" "ui/ui_pages.c" "ui/ui_widget.c"
         "ui/ui_font_cjk18.c" "ui/ui_font_cjk24.c"
    INCLUDE_DIRS "include" "ui"
    EMBED_FILES ${FACE_ASSETS}
    PRIV_REQUIRES cubecore espressif__esp_lvgl_port lvgl__lvgl espressif__esp_lv_eaf_player
)
