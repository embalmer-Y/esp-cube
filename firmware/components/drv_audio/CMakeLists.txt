# drv_audio [L1 驱动]: ES8311 codec + I2S 全双工驱动封装
idf_component_register(
    SRCS "drv_audio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES boardcfg cubecore
        esp_driver_i2s esp_driver_i2c esp_driver_gpio
        espressif__esp_codec_dev
)
