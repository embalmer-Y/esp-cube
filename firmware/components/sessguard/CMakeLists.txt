# sessguard [S3 应用/伙伴]: 会话守卫(T31 首段)
# 有界上下文+失效门+工具去重(sessguard) + 会话生命周期状态机(sessstate)
idf_component_register(
    SRCS "src/sessguard.c" "src/sessstate.c"
    INCLUDE_DIRS "include"
)
