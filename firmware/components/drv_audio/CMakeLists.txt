# drv_audio [L1 驱动]: ES8311 codec + I2S 全双工驱动封装
idf_component_register(
    SRCS "drv_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c           # i2c_master.h 已入公共头(触摸共总线)
    PRIV_REQUIRES boardcfg
        esp_driver_i2s esp_driver_gpio
        espressif__esp_codec_dev
)
