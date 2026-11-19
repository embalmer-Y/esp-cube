#!/usr/bin/env bash
# fetch_face_assets.sh —— 表情素材获取(§55.20 素材出库后的本地安置)
#
# 背景: .efc 素材不随仓库分发(上游 esp_emote_assets 无 LICENSE, 见
# vendor-licenses.md §3)。本脚本给出两条本地获取路径; 素材就位后
# CMake 自动嵌入(app_ui/CMakeLists 逐文件 EXISTS 检测), 无需改代码。
#
# 用法: tools/fetch_face_assets.sh [目标目录, 默认 firmware/components/app_ui/ui/assets/face]
set -euo pipefail
DEST="${1:-firmware/components/app_ui/ui/assets/face}"
mkdir -p "$DEST"

cat <<'NOTE'
[路径 1] 已有素材(设备 SD 卡/旧工作区/tools/efc/out 转制产物):
  直接拷贝 23 段 .efc 到目标目录即可, 跳过本脚本其余部分。

[路径 2] 从乐鑫 esp_emote_assets 拉取上游素材(注意: 上游无 LICENSE,
  仅限自用; 分发须自行确认授权):
NOTE

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone --depth 1 https://github.com/espressif2022/esp_emote_assets "$TMP/src" \
    || { echo "克隆失败(网络/仓库可用性), 请走路径 1"; exit 1; }
echo "上游素材已拉到 $TMP/src (360_360 等多分辨率素材+layout.json)"
echo "下一步: 用 EFC 管线转制 → tools/efc_build.py (环境 ~/.venvs/efc),"
echo "产物拷入 $DEST 后构建自动嵌入。转制参数与 23 段名表见 docs/06 §9.5。"
echo "(脚本不自动转制: 分辨率/裁剪决策属素材工坊范畴)"
