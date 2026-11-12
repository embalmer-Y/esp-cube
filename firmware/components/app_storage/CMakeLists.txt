# app_storage [L3 应用]: SD 卡存储服务(TF 卡槽 SDMMC+FATFS)
# 记忆系统/表情素材/唤醒词模型/参考音频的统一落盘层, 无卡优雅降级
idf_component_register(
    SRCS "app_storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg sdmmc fatfs vfs
)
