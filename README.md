# EspCube · 立方体小音响(AI 桌面伙伴)

ESP32-P4C5(鱼鹰 OSPTEK 开发板, 双核 RISC-V 400MHz + ESP32-C5 WiFi6 协处理器) + 2 寸 460×460 AMOLED(MIPI-DSI) + ES8311/NS4150 音频链路。
功能: 网络电台 / 蓝牙音乐 / 语音唤醒 + 大模型对话(本地记忆/人格/受控演化) / 表情动画 / 安卓 App 管理。
(首版目标板=仅 P4C5, docs/16 §1.2; S31 为历史开发板, 保留双目标构建作回归参照。)

## 当前状态(2026-11-19 晚, 单一口径见 [docs/19 台账 §0/§53](docs/19_开发基线台账.md))

- **灵魂层 v2 定案与实施(docs/24, 本日主线)**: 全流程重走(精读 docs/11-23→需求 12 项裁决→四路二轮调研→技术选型 15 项)后定案——**行为来源大原则**(本地零行为生成规则, 行为只来自用户/LLM 深思; 取消开机自播)、**情绪三层栈 trait→mood→emotion**(D2 终案)、**项目目的重申入册**(docs/16 最高原则: 个人项目/有血有肉/最少干预/演化不可干预只能重建角色)、**四层自由度架构提案**(反应全自由/触发=LLM 评价+提前权+硬底线/参数=genesis 面试/持有留本地, 待用户确认)。实施 S1(黄金测试入 CI+R3 验收判据化)+S2(ke v2+mood 慢层+ethics 解锁+soul_face 扇区共享模块+kd 重标定): soul_accept C2 全绿(TV .41/.81/.86)+C3 闩锁×1 解锁×1+emotion 16 锚全绿(EMOT_KNOWN_FAIL 9→0)。
- **8h 浸泡收官(§55.16)**: 零崩溃/960 ping 全通/dma 恒 40,960; **浸泡后双问题事件**: ①SD 表情文件全程不可读(卡在位挂载成功但 BAD_FILE 跨重启, 候选=FAT 损坏或 EPSCV2 拒读, **待拔卡裁定**; 屏幕现为内置兜底脸) ②黑屏事件=在册"低概率黑屏"首次完整行为学捕获(UI 活/零日志/rst 恢复)。
- **已定案(2026-11-20)**: 四层自由度架构✅确认(触发层暂保守形态); C1 双向验收✅; 双臂实验✅(MiniMax M2.5)。
- **开源(2026-11-20)**: AGPL-3.0; minimp3(CC0) 替换 RPSL mp3_helix; 表情素材出库(上游无许可, fetch_face_assets.sh 本地获取); 全历史净化+安全扫描零凭据; **已推送 github.com/embalmer-Y/esp-cube**。
- **已完成**: S2.5b(演化+genesis)→S3(行为层重构)→S5(六维动机)→S6(注入/校准)全链交付(§55.22-25); 剩余 B-6(设备 LLM 实链/反思触发器, 待 prompt 审阅)→S7 文档终验(开放问题全清单见 docs/19 §56)。
- **历史冲刺(11-18~19)**: P1 网络稳定批关账(A1a: dma 40,960/internal 67K)/B2 开机电台板验/P3a 配网全链真机/appsim 模拟器/soul_sim 快 tick 模拟器(§55.11-14)。
- **验证**: host_tests 全量 ALL PASS(含 ke/kd 16+9 锚与 R3 门); 双目标构建+层检+mem_budget 全绿; 8h 板上浸泡零崩溃。

## 开源许可

本项目（除下述例外）以 **GNU AGPL-3.0** 发布（[LICENSE](LICENSE)）——使用、
修改或通过网络提供服务均须以同协议开源。例外与第三方义务见
[vendor-licenses.md](vendor-licenses.md)：MP3 解码为 **minimp3**（CC0）；
**表情素材不入库**（上游无许可，本地获取见 `tools/fetch_face_assets.sh`）。

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
firmware/     ESP-IDF 工程: main/(L5 装配) + components/(24 分层组件+vendored minimp3)
sim/          宿主 LVGL 模拟器(shim/src + run.sh; 与固件 ui/ 同源编译)
tools/        宿主工具: 层检/宿主测试/板验客户端/看护脚本/素材生成
docs/         编号文档(01-24)
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
