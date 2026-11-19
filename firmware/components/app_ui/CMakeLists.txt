# app_ui [L4 表现]: LVGL9 氛围 UI(只依赖 ui_model.h 契约, 依赖规则见 docs/06)
# ui/ 子树平台无关: 固件与 sim/ 同源编译, UI 代码零分叉;
# 设备后端 ui_port_esp.c 在 main(L5), 模拟器后端 mock_app.c 在 sim/。
# M4: 音频可视化(频谱/歌词)退役, 表情常驻 UI。
# 表情素材(esp_efc_codec 1.1.0, lv_efc 控件; 2026-11-15 由 EAF 改为 EFC):
# 测试阶段内置 4 段 .efc 进 flash rodata(符号 _binary_<name>_efc_start/end)。
# 候选池 23 段保留在 ui/assets/face/, 不编入固件。
# 以上全部由 CONFIG_ESP_CUBE_ENABLE_FACE_ANIM 门控: S31(表情关闭)不嵌入素材、
# 不要求 esp_efc_codec。
if(CONFIG_ESP_CUBE_ENABLE_FACE_ANIM)
    # 素材出库(§55.20 用户裁决): .efc 不再随仓库分发(来源许可未定,
    # 见 vendor-licenses.md §3); 本地存在才嵌入并给对应编译定义,
    # 缺失时构建照绿(表情走降级: builtin 查找失败=无兜底脸)。
    # CONFIGURE_DEPEND: 素材增删自动触发重configure(ninja 对文件存在性
    # 不建依赖, 纯 EXISTS 判断在文件消失时会用旧清单报 missing)
    file(GLOB FACE_GOT CONFIGURE_DEPEND "${CMAKE_CURRENT_LIST_DIR}/ui/assets/face/*.efc")
    set(FACE_EMBED "")
    set(FACE_DEFS "")
    foreach(f smile_static laugh investigate shocked)
        list(FIND FACE_GOT "${CMAKE_CURRENT_LIST_DIR}/ui/assets/face/${f}.efc" IDX)
        if(IDX GREATER -1)
            list(APPEND FACE_EMBED "ui/assets/face/${f}.efc")
            string(TOUPPER ${f} FU)
            list(APPEND FACE_DEFS ESP_CUBE_FACE_EMBED_${FU}=1)
        endif()
    endforeach()
endif()
idf_component_register(
    SRCS "app_ui.c" "ui/ui_pages.c" "ui/ui_widget.c"
         "ui/ui_font_cjk18.c" "ui/ui_font_cjk24.c"
    INCLUDE_DIRS "include" "ui"
    EMBED_FILES ${FACE_EMBED}
    PRIV_REQUIRES cubecore espressif__esp_lvgl_port lvgl__lvgl
)
if(CONFIG_ESP_CUBE_ENABLE_FACE_ANIM AND FACE_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${FACE_DEFS})
endif()
# esp_efc_codec 仅随表情开关引入。写在 if(CONFIG_*) 内的 PRIV_REQUIRES 会被 IDF 的
# 需求预扫描忽略(该扫描不加载 sdkconfig), 故用 optional 形式: 组件在构建中时才链接,
# S31(表情关闭, 组件不在构建中)不受影响。
if(CONFIG_ESP_CUBE_ENABLE_FACE_ANIM)
    idf_component_optional_requires(PRIVATE espressif__esp_efc_codec)
endif()
