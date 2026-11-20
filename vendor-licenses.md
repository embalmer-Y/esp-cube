# 第三方许可与出处（vendor licenses）

> 本文件是仓库内 vendored/衍生代码的许可义务清单。托管依赖（ESP-IDF 及
> `idf_component.yml` 声明的 Component Registry 组件）由构建期拉取、不入库，
> 各自许可随组件分发，不在此列（清单见下节）。

## 1. Galatea / Kaleido 情绪-驱力-点火引擎（MIT）

`firmware/components/app_soul/src/soul_drive.c`、`soul_emotion.c`、
`soul_ignition.c`、`soul_think.c` 由 Kaleido 项目（Galatea L05 affect 模型）的
MIT 许可代码提取/移植而来。**MIT 义务：保留出处与版权声明。**

- 原项目：Galatea（Kaleido 桌面伙伴，MIT License）
- 移植范围：三驱力内稳态（kd）、九维感受态动力学（ke）、深思意图协议（ki）、
  深思轮壳（soul_think）
- 义务履行：上述文件头部均保留移植出处注记；本文件集中声明

## 2. minimp3（CC0 公有领域）

`firmware/components/minimp3/` 为 vendored MP3 解码器（lieff/minimp3，CC0），
**2026-11-20 替换 RPSL mp3_helix**（用户裁决，docs/19 §55.20）：FSF 列 RPSL
与 AGPL 不兼容，已整体移除并从全部 git 历史剥除；现实现为 minimp3 +
helix 兼容垫片（符号前缀 `espcube_mp3dec_*` 避撞 esp_audio_codec 内嵌副本）。

## 3. 表情素材（.efc）来源与状态

`firmware/components/app_ui/ui/assets/face/*.efc` 由乐鑫
`espressif2022/esp_emote_assets`（截至 2026-11-20 **无 LICENSE 文件**）及
用户提供的参考素材（`media/`）经 `tools/efc` 管线转制。

**已处置（2026-11-20，用户裁决"素材不上云"）**：.efc 素材不入库
（目录级 gitignore，仅 MANIFEST.json 入库），本地获取见
`tools/fetch_face_assets.sh`，CMake 条件嵌入（无素材构建照绿）。

## 4. 托管依赖清单（构建期拉取，不入库）

| 依赖 | 许可 | 兼容性 |
|---|---|---|
| ESP-IDF v6.1 | Apache-2.0 | 与 AGPL-3.0 兼容（单向兼容） |
| espressif/esp_lvgl_port、esp_lcd_co5300、esp_hosted、esp_efc_codec、network_provisioning 等 espressif 组件 | Apache-2.0 / MIT | 兼容 |
| lvgl/lvgl 9.x | MIT | 兼容 |
| espressif__cjson | MIT | 兼容 |
| esp-sr（WakeNet 模型） | Apache-2.0（**模型文件按 Espressif 模型许可单独约束**，构建期下载不入库） | 兼容（模型条款另见 Espressif） |

## 5. 网易云音乐（app_ncm）说明

`app_ncm` 为**个人原型**：设备直连 weapi 游客搜索/取址（社区逆向协议）。
不构成官方授权，不承诺商业分发；详见 docs/16 §1.2/§12 与 docs/17 §12。
