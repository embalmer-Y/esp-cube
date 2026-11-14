# llmprofile [S3 应用/伙伴]: LLM 服务商 profile(T30 首段)
# 解析/校验/选择/回退 —— 纯逻辑无 I/O, 宿主可测; 来源=管理通道 settings
idf_component_register(
    SRCS "src/llm_profile.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson
)
