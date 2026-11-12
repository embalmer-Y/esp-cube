# app_soul [L2/L3]: 灵魂内核(驱力/九维情绪/点火 + 记忆轻量版)
# kd/ke/ki 三组件移植自 Kaleido(纯 C 零平台依赖, 黄金轨迹回归在其
# test/ 下, 板上不编); soul.c 为 EspCube 编排层(1Hz 单写者 + 事件漏斗)
idf_component_register(
    SRCS "src/soul.c" "src/soul_mem.c" "src/soul_think.c"
         "src/soul_drive.c" "src/soul_emotion.c" "src/soul_ignition.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore nvs_flash speech_provider
)
