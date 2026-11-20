#!/usr/bin/env bash
# soul_sim 编译+运行 —— 情绪/快慢思考框架 PC 快 tick 模拟器
# 复用固件真源五件套(纯 C 数值核), 与板上同代码同参数。
set -euo pipefail
ORIG="$PWD"
cd "$(dirname "$0")/../../firmware"
gcc -O2 -Wall -DSOUL_BB_HOST_TEST \
    -I components/app_soul/include \
    -o /tmp/soul_sim \
    ../sim/soul_sim/soul_sim.c \
    components/app_soul/src/soul_bb.c \
    components/app_soul/src/soul_budget.c \
    components/app_soul/src/soul_motiv.c \
    components/app_soul/src/soul_face.c \
    components/app_soul/src/soul_trait.c \
    components/app_soul/src/soul_drive.c \
    components/app_soul/src/soul_emotion.c \
    components/app_soul/src/soul_ignition.c -lm
echo "build ok: /tmp/soul_sim"
for a in "$@"; do :; done
args=()
for a in "$@"; do
    # 相对路径参数(scenario/csv)还原为调用目录绝对路径
    if [[ "$a" != --* && -e "$ORIG/$a" && ! -e "$a" ]]; then args+=("$ORIG/$a")
    else args+=("$a"); fi
done
exec /tmp/soul_sim "${args[@]}"
