# cube_act [S0 基础契约]: 统一动作契约+核心(docs/17 §5; 纯 C 零 OS 依赖)
# 能力注册/有界队列/状态机/回执/停止锁存; 宿主回归见 test/
idf_component_register(
    SRCS "src/cube_act_core.c" "src/cact_proposal.c"
    INCLUDE_DIRS "include"
)
