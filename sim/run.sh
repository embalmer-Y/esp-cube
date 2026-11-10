#!/usr/bin/env bash
# run.sh —— 构建并运行 EspCube 模拟器
#   ./run.sh            交互窗口(键鼠)
#   ./run.sh --demo     无屏自动演示 + shots/ 截图
set -euo pipefail
cd "$(dirname "$0")"

export SDL2_ROOT="${SDL2_ROOT:-$HOME/.local/sdl2}"
export LD_LIBRARY_PATH="$SDL2_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

cmake -B build -G Ninja -DSDL2_ROOT="$SDL2_ROOT" "$@" >/dev/null
cmake --build build
mkdir -p shots
exec ./build/espcube_sim "${@:2}"
