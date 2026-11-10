#!/usr/bin/env bash
# install_sdl2.sh —— 无 root 安装 SDL2 开发环境(apt-get download + 解包到 ~/.local/sdl2)
# 模拟器(sim/)的构建依赖; 幂等, 重复运行无害。
set -euo pipefail

PREFIX="$HOME/.local/sdl2"
TMP="$(mktemp -d /tmp/sdl2dl.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

if [ -f "$PREFIX/usr/include/SDL2/SDL.h" ] && [ -f "$PREFIX/usr/lib/x86_64-linux-gnu/libSDL2.so" ]; then
    echo "SDL2 已就位: $PREFIX"
    exit 0
fi

echo "下载 libsdl2-dev / libsdl2-2.0-0 / libsdl2-classic ..."
cd "$TMP"
# Ubuntu 24.10+ 拆包: libsdl2-2.0-0 只含指向 sdl2-classic/ 的符号链接,
# 真实二进制在 libsdl2-classic —— 三个都要。
apt-get download libsdl2-dev libsdl2-2.0-0 libsdl2-classic

mkdir -p "$PREFIX"
for deb in libsdl2-dev_*.deb libsdl2-2.0-0_*.deb libsdl2-classic_*.deb; do
    echo "解包 $deb"
    dpkg-deb -x "$deb" "$PREFIX"
done

echo "完成: $PREFIX"
echo "运行时由 sim/run.sh 注入 LD_LIBRARY_PATH(或已由 rpath 指向该前缀)"
