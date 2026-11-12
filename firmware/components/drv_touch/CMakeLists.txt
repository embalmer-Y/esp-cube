# drv_touch [L1.5 驱动]: CST820 in-cell 触摸(P4C5 屏模组)
# vendor 驱动 esp_lcd_touch_cst820.c 来自 kodediy/esp_lcd_touch_cst820
# (Apache-2.0, 见 LICENSE.txt); esp_lcd_touch 组件经 registry 拉取
idf_component_register(
    SRCS "drv_touch.c" "esp_lcd_touch_cst820.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES boardcfg esp_lcd_touch driver
)
