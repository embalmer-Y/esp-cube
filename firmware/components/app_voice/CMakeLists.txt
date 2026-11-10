# app_voice [L3 应用]: 语音前端(esp-sr WakeNet; VOICE=n 时源文件内部 #if 守卫为空)
# 注意: IDF 6.1 组件注册期 CONFIG_ 变量不可靠(实测 else 分支粘死),
# 改用无条件注册 + 源内 #if CONFIG_ESP_CUBE_ENABLE_VOICE 守卫
idf_component_register(
    SRCS "app_voice.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore app_audio espressif__esp-sr
)
