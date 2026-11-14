# app_ai_session [L3 应用]: AI 云端会话编排(百炼组合C, 移植自 Kaleido)
# 无条件注册 + 源内 #if CONFIG_ESP_CUBE_ENABLE_AI 守卫(同 app_voice)
idf_component_register(
    SRCS "ai_session.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/filler_16k.pcm"
    PRIV_REQUIRES cubecore cube_act app_audio speech_provider persona ai_skills app_soul app_storage llmprofile sessguard
)
