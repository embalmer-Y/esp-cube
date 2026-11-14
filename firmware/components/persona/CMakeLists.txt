# persona [S3 应用/伙伴]: 人格服务骨架(docs/17 §7.1/§7.3; T19/T20)
# 底座/草稿确认制/锁定字段/切换屏障/授权代次/双槽持久化; 纯 C 核心
idf_component_register(
    SRCS "src/persona_core.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_storage
)
