# app_voice: 语音前端(esp-sr: AFE+AEC / WakeNet / MultiNet)
# 里程碑2启用: Kconfig CONFIG_ESP_CUBE_ENABLE_VOICE=y
#              + main/idf_component.yml 取消 esp-sr 依赖注释
if(CONFIG_ESP_CUBE_ENABLE_VOICE)
    idf_component_register(
        SRCS "app_voice.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES cubecore boardcfg drv_audio app_audio espressif__esp-sr
    )
else()
    # 关闭态: 空组件(仅头文件可见, main 中以 #if 守卫调用)
    idf_component_register(INCLUDE_DIRS "include")
endif()
