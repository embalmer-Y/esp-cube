# app_soul [L3 应用]: 灵魂内核(感知/黑板/行为 + 驱力/三层栈情绪(trait→mood→emotion) + trait + 点火 + 记忆)
# kd/ke/ki 三组件移植自 Kaleido(纯 C 零平台依赖, 黄金轨迹回归在其
# test/ 下, 板上不编); soul_bb/soul_budget/soul_face 同为纯 C(宿主可测,
# soul_face 与 soul_sim 同源编译——素材表/算法单一真源);
# soul.c 为 EspCube 编排层(L0 感知+反射, 1Hz 单写者, docs/14)
idf_component_register(
    SRCS "src/soul.c" "src/soul_mem.c" "src/soul_think.c"
         "src/soul_drive.c" "src/soul_emotion.c" "src/soul_ignition.c"
         "src/soul_bb.c" "src/soul_budget.c" "src/soul_motiv.c" "src/soul_face.c" "src/soul_jsonfix.c" "src/soul_moodgate.c"
         "src/soul_trait.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES cubecore cube_act app_storage esp_timer
)
