# drv_display [L1 驱动]: CO5300 AMOLED 显示封装 —— 接口后端按目标选择
# S31 = QSPI(GP-SPI2, 含 3-wire 单线自检) / P4C5 = MIPI-DSI(片上 DPI),
# 对外 API 同形(drv_display.h); 板级常量在 boardcfg
idf_build_get_property(idf_target IDF_TARGET)
set(drv_display_srcs "")
set(drv_display_reqs boardcfg esp_lcd espressif__esp_lcd_co5300 esp_driver_gpio esp_timer)
if(idf_target STREQUAL "esp32s31")
    list(APPEND drv_display_srcs drv_display_qspi.c display_selftest.c)
else()
    list(APPEND drv_display_srcs drv_display_dsi.c)
    # MIPI PHY 供电(esp_ldo_regulator.h 在 esp_hw_support 公共包含路径)
    list(APPEND drv_display_reqs esp_hw_support)
endif()

idf_component_register(
    SRCS ${drv_display_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${drv_display_reqs}
)
