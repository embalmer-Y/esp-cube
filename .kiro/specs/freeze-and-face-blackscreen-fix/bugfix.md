# Bugfix Requirements Document

## Introduction

本文档覆盖 docs/19 §47 的两个高严重度开放问题，并把 #3（表情观感复验）与 #4（UI 帧率目标）随 #2 一起验收。

- **#1 长时静默冻结**（§26/§34/§39/§40/§42/§46）：设备在随机时刻（数分钟至数十分钟，也出现过连续 2.75h 不复现）整机停止串口输出，只能人工复位恢复，复位后无数据损坏。完全空闲态（无电台、无会话、无网络负载）也会发生，"与电台强相关"的旧结论已作废（§34）。冻结时 task WDT（panic 模式，10s，监视双核 idle）与 interrupt WDT（500ms）均已开启，却没有任何 panic 输出（§40 观察窗 3，运行 1407s 时冻结）。§40/§42 据此推断为"硬件级自停"，但这一推断尚未排除另一种情况：CPU 仍在运行，只是控制台/日志通路阻塞。这种情况下 idle 任务照常运行，只监视 idle 的 task WDT 同样不会触发。复位后系统不留任何现场线索。影响：设备无法长时间无人值守运行，T45/T48 长稳验收和多项板上验证被阻塞。
- **#2 460 表情黑屏/卡顿**（§42–§46）：换用 460×460 重编码 EAF 素材后（§45），RST 后 splash 启动动画明显卡顿，splash 结束后主屏黑屏（§46）。串口证实系统存活，所以这是渲染通路问题，不是冻结。已确认的 EAF 根因有三点：（a）素材调色板 alpha 全为 0（`face_{neutral,happy,wink}.eaf` 的调色板项 alpha=0x00 占比 100%），esp_lv_eaf_player 因此发布 RGB565A8 整帧透明，屏幕只剩底色；（b）源帧仅 125×160，`tools/eaf_codec.py` 的 `upscale()` 按 x×3.68、y×2.875 非等比拉伸到 460×460，五官变形；（c）EAF 解码开销随分辨率暴涨（460×460 约为 125×160 的 10.6 倍），splash 期间 EAF 定时器仍在 LVGL 任务内逐帧解码，与 splash 动画争用 LVGL 任务。用户于 2026-11-15 裁决改用 esp_efc_codec 1.1.0（EFC），美术来源为乐鑫官方 Emote Pack 的 848×848 GIF，EAF 路线退役。影响：表情（FR-20）不可见，460 全屏观感（#3）无法复验，帧率目标（#4）无法测量。

约束：USB-JTAG 方案 A 已可用（2026-11-16，见 Introduction 末段），但浸泡与冻结验证必须在无调试器条件下进行，JTAG 仅用于冻结发生后的抓取（可用手段：UART0 串口、屏幕、局域网、板上 CLI、tools/freeze_watch.py 与 tools/reboot_soak.py）。新增诊断缓冲不得占用内部 RAM 静态空间（§26 教训）。

用户在本次需求阶段确认的验收口径：

- 帧率按**渲染能力口径**验收：板上实测单帧耗时满足简单表情 ≤33ms、复杂表情 ≤66ms 即达标；实际播放节奏（素材帧间隔与会话状态快慢调制）保持不变。
- 冻结按**累计 ≥4h 长窗浸泡 + 人为注入卡死验证**判定。

2026-11-15 用户裁决（表情线）：表情渲染改用 esp_efc_codec 1.1.0（EFC），美术来源限定为乐鑫官方 Emote Pack；EAF 路线（esp_lv_eaf_player、`tools/eaf_codec.py` 与三份 `.eaf` 素材）退役。本裁决影响下文 2.9–2.11、2.13–2.15、3.13、3.21–3.22 的表情部分，其余条款不变。用户裁决（2026-11-15）：个人 DIY 项目，非商业用途，按此口径使用 esp_efc_codec 与 Emote Pack 素材；仓库若对外公开，需再复核。用户于 2026-11-15 确认：采用 lv_efc；采用 B6 映射（3.9 随之修改）；测试阶段内置 4 段，其余候选片段作为候选池保留（候选池共 23 段）。

2026-11-16 用户决定：缺陷 #1（冻结）很可能是误报。先以 JTAG（USB-JTAG，J2 第 33/34 脚，方案 A）和无调试器长时浸泡验证；2.1–2.6 改为条件需求，复现后才实施。缺陷 #2（表情）不受影响。

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN 设备在任意负载状态（包括无电台、无会话、无网络负载的完全空闲态）连续运行到某个随机时刻 THEN 系统整机停止所有串口输出且不再自行恢复，只能人工按 RST 复位

1.2 WHEN 发生上述静默冻结 THEN 系统已启用的 task WDT（panic 模式，10s，监视双核 idle）与 interrupt WDT（500ms）都不触发，串口没有任何 panic、回溯或自动重启输出

1.3 WHEN 冻结后经人工复位重新启动 THEN 系统在应用层不报告、不归类上次复位原因（只有 ROM 启动行的原始复位码），也没有跨复位保留的冻结前现场记录（各核心/任务的最后活动），无法事后定位冻结点

1.4 WHEN 串口输出停止 THEN 系统不提供串口以外的存活旁证，无法在无 JTAG 条件下区分"CPU 全停（如卡在总线/PSRAM 访问、中断全关）"与"CPU 仍在运行、仅控制台/日志通路阻塞（含未接主机的 USB-Serial-JTAG 副控制台）"

1.5 WHEN 上电或 RST 后播放 splash 启动动画（固件内嵌 460×460 EAF 表情素材） THEN 系统的 splash 动画（立方体旋转、标题淡入、三点脉动）明显卡顿

1.6 WHEN splash 结束并切换到主屏表情 THEN 系统屏幕黑屏、无任何表情内容，而串口显示系统存活（`startup complete`、`mixer: health` 心跳正常、日志含 `face src 460x460` 与 `clip ok: 46 frames @100ms`、无 panic/EAF 错误），460 全屏表情观感因此无法复验

1.7 WHEN 播放 460 表情片段 THEN 系统没有任何板上实测的逐帧耗时证据，§1 帧率目标（简单表情 ≥30fps / 复杂表情 ≥15fps）处于未测、未达标状态

1.8 WHEN 系统播放当前内嵌的 460×460 EAF 表情素材 THEN 五官因源帧 125×160 被非等比拉伸（x×3.68、y×2.875）而变形，眼睛横向被拉宽

### Expected Behavior (Correct)

> 以下 2.1–2.6 为条件需求：仅当无调试器浸泡或 JTAG 抓取确认冻结复现后实施；未复现则记为未复现，不实施对应修复。

2.1 WHEN 设备在任意负载状态（含完全空闲态）连续运行 THEN 系统 SHALL 不再出现需要人工复位才能恢复的停机：要么根因被消除；要么在根因无法于无 JTAG 条件下消除时，冻结发生后 ≤30s 内自动复位，并按正常开机流程恢复（WiFi got_ip、SNTP、mixer 心跳），开机电台关、静默与音量策略与人工复位后一致

2.2 WHEN 冻结属于现有 WDT 无法捕获的类别（中断全关、核心卡在总线/cache/PSRAM 访问，或 idle 仍运行但关键任务与日志通路全部停滞） THEN 系统 SHALL 由一个不依赖被卡住的 CPU、中断或任务调度的检测/复位机制在 2.1 的时限内触发复位，且复位来源能从复位原因中辨识

2.3 WHEN 任意复位后启动 THEN 系统 SHALL 在启动日志输出上次复位原因（可区分上电、人工 RST、软件重启、panic、各类看门狗）与累计异常复位计数；上次为看门狗或异常复位时，SHALL 同时输出跨复位保留的现场记录，内容至少包括每个核心与关键任务（UI 刷新、音频 mixer、控制台/CLI、网络）的最后活动标记及时间戳、上次运行时长、冻结前最后一次系统心跳摘要；冷上电或记录校验失败时 SHALL 标明"无有效现场记录"，不输出陈旧或垃圾数据；记录 SHALL 不含用户语音/对话原文与凭据

2.4 WHEN 串口输出停止 THEN 系统 SHALL 提供至少一种串口以外、无需 JTAG 的存活旁证（例如屏幕表情动画是否持续推进、局域网是否仍可应答），并在现场记录中保留足以判定"CPU 全停"还是"仅控制台/日志通路阻塞"的证据；任一控制台通路（UART0，或未接主机的 USB-Serial-JTAG 副控制台）不可写时，日志输出 SHALL 不阻塞系统其他功能

2.5 WHEN 修复固件在板上做累计 ≥4h 长窗浸泡（包含完全空闲态，由 tools/freeze_watch.py 监听） THEN 系统 SHALL 出现 0 次需要人工复位的停机；期间若发生冻结并自动复位，每一次 SHALL 产出 2.3 所述的现场记录

2.6 WHEN 经串口 CLI 人为注入卡死（至少覆盖：单核关中断死循环、双核同时关中断自旋、关键任务停滞而 idle 照常运行） THEN 系统 SHALL 在 2.1 的时限内自动复位，下次启动输出的复位原因与现场记录 SHALL 指向注入所在的核心/任务；注入入口 SHALL 仅限串口 CLI（物理在场），不经局域网管理通道或 App 暴露

2.7 WHEN 上电或 RST 后播放 splash 启动动画 THEN 系统 SHALL 流畅播放 splash：用户眼见无明显卡顿，板上实测 splash 期间 UI 刷新间隔 P95 ≤50ms 且无单次 >100ms 停顿，表情素材的加载与解码不挤占 splash 刷新

2.8 WHEN splash 结束并切换到主屏表情 THEN 系统 SHALL 立即显示持续推进的表情动画、不黑屏，连续 ≥10 次 RST/上电每次如此；表情播放期间 UI 刷新间隔满足 2.7 的门限，触摸手势与面板滑出保持响应

2.9 WHEN 表情在主屏显示 THEN 系统 SHALL 让表情铺满 460×460 圆屏（无中央小矩形、无播放器画布底色外露、构图居中）：表情的透明区域透出其下的底色，五官由 EFC 颜色键成形，素材不做非等比缩放，并经用户眼见复验确认（§47 #3）

2.10 WHEN 播放简单表情片段（待机片段，以及 PC 侧变化 tile 占比不超过 3% 的片段，分类见 design.md） THEN 系统 SHALL 在板上实测单帧（解码+渲染+刷屏）耗时 P95 ≤33ms（具备 ≥30fps 能力），证据包含 cputop 任务负载与逐帧耗时日志，并注明样本数与场景（§47 #4）

2.11 WHEN 播放其余表情片段 THEN 系统 SHALL 在板上实测单帧（解码+渲染+刷屏）耗时 P95 ≤66ms（具备 ≥15fps 能力），证据要求同 2.10（§47 #4）

2.12 WHEN mood 切换，或经 SD 就绪自动热载、CLI face 命令替换表情片段 THEN 系统 SHALL 正常显示新片段、不黑屏，切换过程中 UI 无单次 >100ms 停顿

2.13 WHEN 生成表情素材 THEN 系统 SHALL 以乐鑫官方 Emote Pack 的原始 GIF 为唯一美术来源（不以本仓库已有的 .eaf 派生件为输入），经可复现脚本与 esp_efc_codec 1.1.0 的官方 PC 编码器产出 .efc，并在清单 MANIFEST.json 中记录来源归档 sha256、编码器版本与参数、每个输出文件的 sha256

2.14 WHEN 内置或 SD 热载的表情片段被加载 THEN 系统 SHALL 先经 esp_efc_codec 完成预检（打开成功、取得 460×460 与帧数、未缩放、文件大小与帧数在上限内、透明像素未占满），预检通过后才替换当前表情；预检失败时 SHALL 保留当前表情并告警

2.15 WHEN 构建表情固件 THEN 系统 SHALL 只内置被选定的表情片段（当前为 smile_static、laugh、investigate、shocked），并按全部候选片段（23 段）的总容量核对 flash 分区余量，使将来增加片段时不需修改结构。

### Unchanged Behavior (Regression Prevention)

3.1 WHEN 在宿主运行 tools/host_tests.sh THEN 系统 SHALL CONTINUE TO 14 个套件全部 PASS

3.2 WHEN 构建 S31、P4C5（带 `-DSDKCONFIG=$PWD/sdkconfig.p4c5`，build 目录 firmware/build-p4c5）与 sim 三个目标并运行 tools/check_layers.py THEN 系统 SHALL CONTINUE TO 三个目标构建退出码均为 0、分层校验全绿

3.3 WHEN 板上启动并联网 THEN 系统 SHALL CONTINUE TO 完成 WiFi got_ip、SNTP 对时与 mixer health 心跳，SRP 配对与 mTLS 管理通道按现有授权规则工作；新增的存活旁证 SHALL CONTINUE TO 不绕过这些规则、不暴露凭据或用户数据

3.4 WHEN 执行 tools/reboot_soak.py 重启浸泡 THEN 系统 SHALL CONTINUE TO 每次启动 got_ip，且无 abort/assert/guru/hang

3.5 WHEN 发生现有 WDT 可捕获的软件级卡死（任务饿死 idle、单核长时间关中断） THEN 系统 SHALL CONTINUE TO 由 task WDT / interrupt WDT 触发 panic、输出双核回溯并自动重启

3.6 WHEN 系统执行合法的长耗时操作（启动初始化、SD/NVS 读写、flash 操作、TLS/SRP 配对计算、表情素材加载、WiFi 重连） THEN 系统 SHALL CONTINUE TO 正常完成，新增的检测/复位机制不发生误复位

3.7 WHEN 复位（人工或自动）发生在 SD/NVS 写入期间 THEN 系统 SHALL CONTINUE TO 由现有双槽/提交代协议保证数据不损坏，复位后数据可正常读回

3.8 WHEN 上电或 RST 后进入 splash THEN 系统 SHALL CONTINUE TO 展示现有 splash 内容（立方体、EspCube 标题、副标题、三点），约 2.4s 后淡入主屏

3.9 WHEN mood_tag 变化或会话状态（聆听、思考）变化 THEN 系统 SHALL 按采纳的映射切换表情片段并从头播放：idle→smile_static，happy→laugh，relaxed→leisure，sad→sad，angry→angry，surprised→shocked，confused→question，sleepy→yawn，listen→investigate，think→ponder；speak 沿用当前情绪片段，并只调整帧间隔（3.10）。聆听与思考的片段切换为新增行为。

3.10 WHEN 会话状态变化 THEN 系统 SHALL CONTINUE TO 按现有节奏调制帧间隔（待机/聆听 100ms、思考 200ms、说话 70ms）与底色呼吸效果

3.11 WHEN 用户展开状态栏或 AI 状态面板 THEN 系统 SHALL CONTINUE TO 暂停表情播放、收起后恢复，面板自动收起与手势行为不变

3.12 WHEN 表情素材加载失败 THEN 系统 SHALL CONTINUE TO 显示情绪底色圆作为回退并输出告警日志

3.13 WHEN SD 就绪且存在 face/idle.efc，或经 CLI face 命令热载 THEN 系统 SHALL CONTINUE TO 异步加载并替换当前表情；文件不存在或预检失败时保留当前表情并告警

3.14 WHEN 设备开机或处于夜间安静时段 THEN 系统 SHALL CONTINUE TO 保持开机电台关、soul 安静时段自动停播，以及现有音量默认值与运行时设置

3.15 WHEN 系统输出常规日志（含新增的诊断日志、现场记录、帧耗时日志） THEN 系统 SHALL CONTINUE TO 符合 NFR-03：I 级常规日志不记录用户语音/对话原文

3.16 WHEN 新增的诊断、恢复或性能代码启用 THEN 系统 SHALL CONTINUE TO 正常启动、CLI 等任务栈分配成功，内部堆余量与最大连续块不低于修复前基线（新增缓冲走堆/PSRAM 或复位保留区，不新增内部 RAM 静态缓冲）

3.17 WHEN 通过 UART0（115200）使用 CLI 以及 tools/freeze_watch.py、tools/p4_cli.py 等串口工具 THEN 系统 SHALL CONTINUE TO 正常输入输出，`cputop` 输出逐任务 CPU%，`baseline <n>` 输出内存采样报表

3.18 WHEN 板上烧录新固件后启动 THEN 系统 SHALL CONTINUE TO 在启动日志输出可辨识的固件版本/构建标识，用于确认板上运行的确为新固件（§45 教训）

3.19 WHEN 用户经 J2 排针（GPIO24/25）接入 USB-JTAG（方案 A，2026-11-16 已接线） THEN 系统 SHALL CONTINUE TO 允许 OpenOCD 附着调试（固件不占用 GPIO24/25，不烧写禁用 JTAG 的 eFuse）

3.20 WHEN 修改构建配置（如 PSRAM 速率、看门狗、控制台设置）并提交 THEN 系统 SHALL CONTINUE TO 不把 sdkconfig.p4c5 等本地文件中的凭据纳入提交（sdkconfig.p4c5 保持 gitignored）

3.21 WHEN 构建 sim 目标 THEN 系统 SHALL CONTINUE TO 以 `CONFIG_ESP_CUBE_ENABLE_FACE_ANIM` 关闭表情代码并构建通过，且不链接 esp_efc_codec（其解码器只提供乐鑫目标的预编译库）

3.22 WHEN mood 或会话状态对应的片段未内置 THEN 系统 SHALL 回退到 smile_static 片段、输出一条告警日志，且不出现黑屏。
