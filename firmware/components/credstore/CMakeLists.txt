# credstore [S3 应用/伙伴]: 服务凭据安全存储(T17 设备侧)
# EPSCV2 容器落盘(dev_key 域)+版本冲突语义; 明文仅瞬态
idf_component_register(
    SRCS "src/credstore.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_storage nvs_flash
)
