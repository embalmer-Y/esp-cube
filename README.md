# EspCube · 立方体小音响(AI 桌面伙伴)

ESP32-P4C5(鱼鹰 OSPTEK 开发板, 双核 RISC-V 400MHz + ESP32-C5 WiFi6 协处理器) + 2 寸 460×460 AMOLED(MIPI-DSI) + ES8311/NS4150 音频链路。
功能: 网络电台 / 蓝牙音乐 / 语音唤醒 + 大模型对话(本地记忆/人格/受控演化) / 表情动画 / 安卓 App 管理。
(首版目标板=仅 P4C5, docs/16 §1.2; S31 为历史开发板, 保留双目标构建作回归参照。)

## 当前状态(2026-11-19, 单一口径见 [docs/19 台账 §0/§53](docs/19_开发基线台账.md))

- **冲刺成果(11-18~19)**: **P1 网络稳定批实质关账**(A1 修复关闭 + A1a 余量加固关闭 §55.13: lwip→PSRAM 找回+hosted DMA 补丁, ×10 实测 dma_largest 恒 40,960/internal_free 翻倍至 67K/TLS 零错); **B2 开机电台解封板验**(L0 反射零人工自播); **P3a SoftAP 配网通道端到端全通**(用户安卓真机+官方 ESP Provisioning app, 五观察点全收 §55.11); **appsim Linux 模拟器**(App 全部网络行为面脚本化验证 ALL-OK + 设备撤销口 pairunbid 补缺 §55.12); **情感系统 PC 快 tick 模拟器**(sim/soul_sim, 固件真源同代码快进 30 天 §55.14)——月度极限测试抓获深思配额终身制 bug(已修 §55.14 续)并暴露四个情感调参议题(表情月尺度失效等, §55.14 续2)。
- **当前活动**: 8h 长稳浸泡(串口+ping 双通道); 用户四决议执行中(E3 暂不发布/安卓开发延后/S31 冻结仅同步编译/A1a+B2 已完成)。
- **开放问题**: 见台账 §53 与 §55.14 续2(情感调参 4 项)、五通道接线缺口、A1b 择机、A3 四码复测(S31 板窗)、B1 多轮解封、D8 点播验收、72h 终验(D12)。
- **验证**: A1a 后 ×10 压测 dma 40,960 恒定零漂移 + 电台 13min 播放零 underrun + memcheck PASS; 配网/配对/管理全链真机验证; 宿主全绿; 双目标构建+层检全绿。

## 文档索引(编号即阅读序)

| 文档 | 内容 |
|---|---|
| [docs/01](docs/01_技术方案.md) | 技术方案(选型决策/风险) · [docs/02](docs/02_详细设计.md) 详细设计(历史快照) |
| [docs/06](docs/06_分层架构与代码规范.md) | **结构宪法**: 顶层目录约定/分层规则/代码规范 |
| [docs/15](docs/15_架构与脑科学方案审查报告.md) | 架构审查报告 · [docs/16](docs/16_产品需求规格与功能边界.md) **产品需求规格(FR/NFR)** |
| [docs/17](docs/17_系统软件详细设计.md) | **系统软件详细设计**(架构合同) · [docs/18](docs/18_开发计划与任务拆解.md) **开发计划(T01-T48)** |
| [docs/19](docs/19_开发基线台账.md) | **开发基线台账**(进度单一速查) · [docs/20](docs/20_设备信任与安全方案.md) 信任与安全 · [docs/21](docs/21_数据schema升级与回退方案.md) schema 升级回退 |
| docs/03-05/07-14 | 接线指南/调研报告(部分为历史快照, 文首有标注) |

## 仓库结构(约定见 [docs/06 §0](docs/06_分层架构与代码规范.md))

```
firmware/     ESP-IDF 工程: main/(L5 装配) + components/(24 分层组件+vendored mp3_helix)
sim/          宿主 LVGL 模拟器(shim/src + run.sh; 与固件 ui/ 同源编译)
tools/        宿主工具: 层检/宿主测试/板验客户端/看护脚本/素材生成
docs/         编号文档(01-21)
media/        参考素材(只读)
testaudio/    测试音频(gitignored)
```

固件组件(L0→L4, 完整层表=docs/06 §1):
`cubecore/boardcfg/cube_act`(L0) → `drv_audio/drv_display/drv_touch`(L1) →
`app_audio/app_net/speech_provider/app_ncm/ai_skills/app_storage`(L2) →
`app_bt/app_player/app_voice/app_ai_session/app_soul/app_assist/persona/pair/credstore/llmprofile/sessguard`(L3) →
`app_ui`(L4) → `main`(L5)。

## 构建(IDF v6.1; 勿裸跑 set-target)

```bash
cd firmware
idf.py --preview -B build-s31 build                             # S31(回归参照)
idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 build      # P4C5(主板)
idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 -p /dev/ttyACM0 flash monitor
```
WiFi 凭据/LLM API key 经 menuconfig 填入**本地** sdkconfig(已 gitignored, 绝不入库)。
宿主测试: `tools/host_tests.sh`; 分层校验: `tools/check_layers.py`; 模拟器: `sim/run.sh`。
