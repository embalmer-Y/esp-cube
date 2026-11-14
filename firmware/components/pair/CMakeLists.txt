# pair [S3 应用/伙伴]: 配对服务(T15; docs/20 §3)
# SRP-6a 自实现(mbedtls 4 已移除 SRP)+PIN 窗口/限速/客户端指纹表
idf_component_register(
    SRCS "src/ubn.c" "src/srp6a.c" "src/pair_svc.c" "src/pair_port_psa.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash mbedtls
)
# SRP 模幂是热点: 全局 -Os 下 step1 >45s(板上实证), 独立 -O3(约快 3-5×)
set_source_files_properties(
    "src/ubn.c" "src/srp6a.c"
    PROPERTIES COMPILE_OPTIONS "-O3"
)
