#!/usr/bin/env bash
# run.sh —— 构建并运行 EspCube 模拟器
#   ./run.sh            交互窗口(键鼠)
#   ./run.sh --demo     无屏自动演示 + shots/ 截图
set -euo pipefail
cd "$(dirname "$0")"

export SDL2_ROOT="${SDL2_ROOT:-$HOME/.local/sdl2}"
export LD_LIBRARY_PATH="$SDL2_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

# -D 开头的参数传给 cmake, 其余(如 --demo)传给可执行文件
cmake_args=()
bin_args=()
for a in "$@"; do
    [[ "$a" == -D* ]] && cmake_args+=("$a") || bin_args+=("$a")
done
cmake -B build -G Ninja -DSDL2_ROOT="$SDL2_ROOT" "${cmake_args[@]}" >/dev/null
cmake --build build
mkdir -p shots
exec ./build/espcube_sim "${bin_args[@]}"
