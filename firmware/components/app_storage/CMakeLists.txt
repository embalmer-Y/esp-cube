# app_storage [S2 平台服务]: SD 卡存储服务(TF 卡槽 SDMMC+FATFS)
# + 双槽可恢复提交协议 store_slot(docs/17 §8.2; T07)
# 记忆系统/表情素材/唤醒词模型/参考音频的统一落盘层, 无卡优雅降级
idf_component_register(
    SRCS "app_storage.c" "src/store_slot.c" "src/store_crypt.c" "src/trust.c" "src/crypt_port_mbedtls.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore boardcfg sdmmc fatfs vfs nvs_flash mbedtls
)
