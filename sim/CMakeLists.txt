# sim/CMakeLists.txt —— EspCube PC 模拟器(LVGL9 + SDL2)
#
# 与固件共享的源码(同源编译, UI 代码零分叉):
#   - firmware/components/app_ui/ui/      页面/动画/模型边界
#   - firmware/components/cubecore/cube_fft.c  频谱 FFT
#   - firmware/managed_components/lvgl__lvgl    与设备同版本 LVGL(9.6.0)
#
# 构建前置: tools/install_sdl2.sh(apt-get download + dpkg -x 解到 ~/.local/sdl2)
cmake_minimum_required(VERSION 3.16)
project(espcube_sim C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall -Wno-unused-parameter)

# ---- LVGL(与固件同源同版本) ----
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/managed_components/lvgl__lvgl"
    CACHE PATH "LVGL 源码目录")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "LVGL 未就位: ${LVGL_DIR}\n(先在 firmware/ 下跑一次 idf.py build)")
endif()
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)   # 不编 LVGL examples, 提速
add_subdirectory("${LVGL_DIR}" lvgl)

# ---- SDL2(本地解包前缀, 无需 root) ----
set(SDL2_ROOT "$ENV{HOME}/.local/sdl2" CACHE PATH "SDL2 解包前缀")
if(NOT EXISTS "${SDL2_ROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR "SDL2 未就位: ${SDL2_ROOT}\n(运行 tools/install_sdl2.sh)")
endif()

add_executable(espcube_sim
    src/main.c
    src/mock_app.c
    ../firmware/components/app_ui/ui/ui_pages.c
    ../firmware/components/app_ui/ui/ui_widget.c
    ../firmware/components/app_ui/ui/ui_font_cjk18.c
    ../firmware/components/app_ui/ui/ui_font_cjk24.c
    ../firmware/components/cubecore/cube_fft.c
)

target_include_directories(espcube_sim PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/app_ui/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/cubecore/include
    shim
    ${SDL2_ROOT}/usr/include
    ${SDL2_ROOT}/usr/include/SDL2
    ${SDL2_ROOT}/usr/include/x86_64-linux-gnu   # _real_SDL_config.h(multiarch)
)
target_link_directories(espcube_sim PRIVATE
    ${SDL2_ROOT}/usr/lib/x86_64-linux-gnu
)
target_link_libraries(espcube_sim lvgl SDL2 m pthread dl)
# SDL2.so 的间接依赖(libdrm/libwayland)系统已装但不在链接路径, 运行时由
# DT_NEEDED 解析 —— 放宽链接期检查
target_link_options(espcube_sim PRIVATE -Wl,--allow-shlib-undefined)
set_target_properties(espcube_sim PROPERTIES
    BUILD_RPATH "${SDL2_ROOT}/usr/lib/x86_64-linux-gnu"
)
