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

## 2. mp3_helix（RealNetworks Public Source License, RPSL）

`firmware/components/mp3_helix/` 为 vendored MP3 解码器，**RPSL 许可**，
`LICENSE.txt` 随目录保留。

⚠️ **已知事项（2026-11-20 审计）**：FSF 将 RPSL 列为与 GPL/AGPL **不兼容**
（自由但互斥）。本项目主体采用 AGPL-3.0。处置选项与追踪见
docs/19 §55.19；在处置前，分发本仓库固件二进制时 mp3_helix 部分始终以
RPSL 单独适用（其目录自成许可单元，改动须遵循 RPSL 条款）。

## 3. 表情素材（.efc）来源与状态

`firmware/components/app_ui/ui/assets/face/*.efc` 由乐鑫
`espressif2022/esp_emote_assets`（截至 2026-11-20 **无 LICENSE 文件**）及
用户提供的参考素材（`media/`）经 `tools/efc` 管线转制。

⚠️ **已知事项（2026-11-20 审计）**：上游无许可标注的素材再分发存在版权
不确定性。处置追踪见 docs/19 §55.19（候选：素材移出仓库+拉取脚本 /
替换自制素材 / 等上游补许可）。

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
