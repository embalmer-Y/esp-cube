# drv_display [L1 驱动]: QSPI AMOLED 面板驱动封装(CO5300)
idf_component_register(
    SRCS "drv_display.c" "display_selftest.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES boardcfg esp_lcd espressif__esp_lcd_co5300 esp_driver_gpio esp_timer
)
