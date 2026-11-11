# ai_skills [L2 服务]: AI 技能注册表(function calling 设备端半边)
# 纯注册/路由, 无业务依赖(handler 由 L5 注入, 见头注释)
idf_component_register(
    SRCS "ai_skills.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson
)
