# 冻结与表情黑屏修复 Bugfix Design

## Overview

本设计覆盖 bugfix.md 的两个缺陷，并把 §47 #3（表情观感复验）、#4（帧率）随缺陷 #2 一起验收。

- **缺陷 #1 长时静默冻结**：设计阶段的源码与日志复核推翻了 §40/§42 的"WDT 不可捕获的硬件级自停"结论。①本机 IDF v6.1 的 interrupt WDT 第二级是 `WDT_STAGE_ACTION_RESET_SYSTEM`，两核都关中断时 ≤1.5s 必然整机复位并打印 ROM 复位行，所以"零 panic 且不复位"与"中断全关"矛盾；②task WDT 只监视两个 IDLE 任务，"idle 照常运行、业务任务卡住"确实不会触发；③§40 观察窗 3 唯一一次被当作"冻结"的现场（`/tmp/freeze_watch.log`）末尾是 `rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))` 与三次 `waiting for download`，说明芯片被主机侧 DTR/RTS 时序复位进了 ROM 下载模式，属于主机诱发，不是固件冻结。修复策略：先消除主机侧假象（串口独占、监听器分类），再加一套不依赖 HP CPU 的检测与复位（LP 看门狗 + 监督器），加跨复位现场记录、非阻塞日志、串口以外的存活旁证与 CLI 故障注入，用累计 ≥4h 浸泡和注入实验判定。
- **缺陷 #2 460 表情黑屏/卡顿**：根因已在源码、素材与日志层面确认（EAF 路线，结论见附录 A）：调色板 alpha 全为 0 使整帧透明；125×160 源帧非等比拉伸使五官变形；EAF 逐帧解码开销与 splash 动画争用 LVGL 任务。2026-11-15 用户裁决改用 esp_efc_codec 1.1.0：美术来源为乐鑫官方 Emote Pack（23 段、848×848 GIF），PC 侧等比缩放到 460×460、按 100 ms 时间采样、带颜色键编码为 `.efc`（tile 4、zlib-only），设备侧以 `lv_efc` 对象播放，splash 与面板期间经 `lv_efc_pause/resume` 挂起。

设计阶段没有打开串口、也没有烧录：中止的 bugfix 运行日志显示 `python3` 调用因缺少 PIL 与 numpy 模块而报 ModuleNotFoundError，未记录权限拒绝，串口工具在设计阶段未运行。板上证据来自已有的 `/tmp/freeze_watch.log`、`/tmp/freeze_watch_report.json` 与源码阅读，所有结论在测试策略中都有对应的板上复核步骤。

### 执行记录（2026-11-15）：EAF 路线退役，表情改用 esp_efc_codec

- **2026-11-16 决定**：冻结线先验证后修复。JTAG 采用方案 A（USB-JTAG，J2 GPIO24/25，第 33/34 脚，不改 eFuse）。冻结修复设计（A 节）为条件设计，仅复现后实施。
- **决定**：表情渲染由 `espressif/esp_lv_eaf_player` + `tools/eaf_codec.py` + `face_{neutral,happy,wink}.eaf` 改为 `espressif/esp_efc_codec` 1.1.0 + `tools/efc_build.py` + `ui/assets/face/*.efc`。依据是用户于 2026-11-15 的裁决，以及下列事实。
- **为什么退役 EAF**：（1）三份素材的调色板 alpha 全为 0，整帧透明，这是黑屏的直接根因；（2）EAF 工具链以 125×160 源帧为起点，460 版只能靠非等比放大得到，无法利用官方原图的分辨率；（3）EAF 解码开销随分辨率暴涨，且 splash 期间仍在 LVGL 任务内逐帧解码。
- **为什么选 EFC**：官方 Emote Pack 为 848×848 GIF（23 段，透明背景，帧时长 40 ms）。esp_efc_codec 提供 PC 编码器（tile 4、zlib、PRED，内置落盘前无损往返校验）与 `lv_efc` 对象。实测 23 段带键 EFC（460×460，100 ms 采样）合计 206,846 字节，最大 44,565 字节。对照 EAF 9 段合计 1.09 MB（docs/19 §42）。
- **历史保留**：EAF 路线的取证结论、证据路径与退役清单写入附录 A。`evidence/2/`、`evidence/3/` 是 EAF 时期的产物，保留不删。
- **已完成任务的处置**：原任务 2、3、4.2、4.3 已完成，产物属 EAF 专用，按退役处理。原任务 4.4 为中止状态，已重置为未开始，新文本见任务 31.4。原任务 4.1 是通用件，保持不变。原任务 2–10 的编号退役，新编号见 tasks.md 第一部分。
- **已决（2026-11-15）**：lv_efc（B2）；B6 映射（采纳）；测试内置 4 段与候选池（B7）；lit 口径用 Glossary 定义，撤回 27 记录的非键占比近似（D-LIT）；键占比与 lit 记录第 0 帧值，全帧平均仅供信息，板上 alpha0 校验与屏幕自检以第 0 帧为准（D-FRAME0）；B10 底色圆维持隐藏（D-B10）；23 个 `.efc` 与 MANIFEST.json 提交进 git（D-GIT）；编码器环境迁至 `~/.venvs/efc`，`/tmp/efc-venv` 不再使用（D-VENV）。仍待定：R 规则与 36 的 230 px 问题（待 33 测量后）；face_perf_screen 去留（30.5）。

## Glossary

- **Bug_Condition (C)**：触发缺陷的输入集合。C₁ 为冻结类（现有 WDT 抓不到、需要人工复位的停机，含主机诱发的下载模式），C₂ 为表情类（splash 期间与表情解码争用、整屏透明或素材未经预检、帧耗时超预算、切换停顿）。
- **Property (P)**：C 成立时修复后应有的正确行为（自动复位并留现场 / 表情可见、流畅、达帧耗时门限）。
- **Preservation**：¬C 输入下必须保持不变的行为（bugfix.md 3.1–3.21）。
- **F / F'**：修复前 / 修复后固件。冻结线的 F 指阶段 A 观测固件（任务 17 产出）；表情线的 F0 指当前仓库的 EAF 固件，F' 指 EFC 固件。
- **TWDT**：task watchdog，现配置 panic 模式 10s，只订阅 IDLE0/IDLE1（`sdkconfig.p4c5` `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y`，固件内无 `esp_task_wdt_add` 调用）。
- **IWDT**：interrupt watchdog，TIMG1 的 MWDT，stage0 中断（500ms）→ panic，stage1 硬件整机复位（再 1000ms）。
- **LP WDT（RWDT）**：LP 域的 RTC 看门狗，RC_SLOW 时钟，独立于 HP CPU、HP 时钟与中断；HAL 见 `esp_hal_wdt/esp32p4/include/hal/lpwdt_ll.h`。
- **监督器（supervisor）**：新增 `sysdiag` 组件内 1Hz 运行的检查逻辑，只在所有被监视对象健康时喂 LP WDT。
- **现场记录（record）**：放在 `RTC_NOINIT_ATTR`（LP SRAM）的结构体，跨 HP 系统复位保留，含各核心/关键任务最后活动标记、日志通路计数、心跳摘要，带 magic+CRC。
- **logtx / 日志环**：新增的异步日志通路。`esp_log_set_vprintf` 钩子把日志格式化进 PSRAM 槽环，`logtx` 任务按行写出，生产者从不阻塞。
- **EN 复位**：CHIP_EN 被拉低（RST 键或 CH343 的 DTR/RTS 自动复位电路），ROM 报 `rst:0x1 (POWERON)`，与冷上电同码。
- **主机诱发下载模式**：EN 复位时 BOOT 被同时拉低，ROM 输出 `boot:0x307 (DOWNLOAD...)` 与 `waiting for download` 后静默等待；只有再次复位才能退出。
- **D / R / F / T**：单帧解码耗时（EFC 播放定时器回调，含准备与发布，见实测方案）/ 渲染耗时（`LV_EVENT_RENDER_START`→`RENDER_READY`）/ 刷屏耗时（`LV_EVENT_FLUSH_START`→`FLUSH_FINISH`，含 vsync 对齐等待）/ T = D + R + F。
- **I_refr / I_loop**：相邻两次 `LV_EVENT_REFR_START` 的间隔（用于持续动画阶段，如 splash）/ LVGL 任务两次定时器处理之间的空档（20ms 探针定时器实测间隔，用于表情播放与切换阶段）。
- **EFC（Emote Frame Codec）**：esp_efc_codec 的块码流格式，固定 4×4 tile，未变化的块复用解码器保留的上一帧缓冲。
- **lv_efc**：esp_efc_codec 提供的 LVGL 对象（`lv_efc_create`、`lv_efc_set_src_data`、`lv_efc_play`、`lv_efc_pause`、`lv_efc_resume`、`lv_efc_set_frame_delay`），本设计的设备集成对象。
- **颜色键（colour key）**：EFC 中表示透明的单一 RGB565 值 0xF81F（PC 编码器 `apply_color_key` 写入）。lv_efc 对键色像素发布 alpha=0，并把片段显示为 RGB565A8。
- **键控片段 / 不透明片段**：带颜色键的 EFC（透明背景，当前采用）与不带键、同帧合成到底色的 EFC（体积约为前者的 5.6 倍，只作对照与回退）。
- **变化 tile 占比**：相邻帧（含循环回绕）中 4×4 tile 逐像素不等的比例均值，由 PC 侧计算，是解码工作量的代理量。
- **简单 / 复杂片段**：简单 = 待机片段（smile_static）与变化 tile 占比 ≤3.0% 的片段；其余为复杂（见 B6）。
- **情绪底色圆（face_bg）**：表情下方的圆形底色控件。现状为加载成功后隐藏，失败时作回退显示。
- **lit 占比**：屏幕自检口径（EFC 路线）。表情圆内 32×32 采样点中，亮度 `0.299R+0.587G+0.114B ≥ 128` 的比例。它只看白色五官与深色背景的对比，与底色呼吸无关。板上核对（alpha0、屏幕自检）以第 0 帧为准，对照 MANIFEST 的 `frame0_lit_share_pct`；全帧平均只作信息记录。
- **预检（precheck）**：加载表情前对素材做的只读检查（打开、尺寸、帧数、大小、透明占比），不改变当前显示。
- **MANIFEST.json**：素材清单，记录来源归档、编码器版本与参数、每个输出的 sha256 与逐帧 CRC32。
- **PENDING**：待用户确认的决策项。确认前不生效，设计中以此标记。

## Bug Details

### 设计阶段取证结论

| 疑点/证据 | 本机来源 | 结论 |
|---|---|---|
| (a) TWDT 是否只看 idle | `firmware/sdkconfig.p4c5` 2741–2750 行（`ESP_TASK_WDT_PANIC=y`、`TIMEOUT_S=10`、`CHECK_IDLE_TASK_CPU0/1=y`）；`firmware/main`、`firmware/components` 内无 `esp_task_wdt_add`/`esp_reset_reason` 调用 | 成立。idle 能跑、业务任务阻塞（等锁、等队列）时 TWDT 不会触发 |
| (b) P4 上 IWDT 第二级是否整机复位 | IDF `/home/emb/esp/esp-idf`（`v6.1-1-gc55d13c6`）`components/esp_system/int_wdt.c` 的 `reconfigure_ticks()`：stage0 `WDT_STAGE_ACTION_INT`，stage1 `WDT_STAGE_ACTION_RESET_SYSTEM`；`components/esp_system/include/esp_private/esp_int_wdt.h`：`IWDT_STAGE1_TIMEOUT_US = 2 × STAGE0`（500ms → 1000ms）；`tick_hook()` 只有 CPU1 也走过 tick 时 CPU0 才喂狗（`CONFIG_ESP_INT_WDT_CHECK_CPU1=y`） | 成立。两核都停 tick（中断全关、两核卡在总线访问）时 ≤1.5s 硬件复位，ROM 会打印复位行。"零 panic 且不复位"与"中断全关"互相矛盾 |
| 单核关中断 | 同上：中断源路由到两个核（`esp_int_wdt_cpu_init()` 每核各路由一次） | 另一核响应 stage0 → panic `Interrupt wdt timeout on CPUx`，属 3.5 已有行为 |
| UART0 不可写会怎样 | `components/esp_driver_uart/src/uart_vfs.c` `uart_tx_char()`：`while (uart_ll_get_txfifo_len(uart) < 2) { ; }` 忙等不让出 | FIFO 不排空时写者自旋 → idle 饿死 → TWDT panic，不会静默；只有"FIFO 照常排空但字节没上线"（TX 引脚路由被改）或"写者阻塞在锁上"才会静默 |
| USJ 副控制台无主机 | `components/esp_stdio/stdio_vfs.c` `console_write()`：先写主口 UART，再在 `s_lock` 下写副口；`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`：未连接时 `usb_serial_jtag_write()` 直接返回 EIO，FIFO 不可写最多等 `TX_FLUSH_TIMEOUT_US`=50ms 后丢弃；`usb_serial_jtag_connection_monitor.c` 用 SOF+tick hook 判连接（3ms 容差） | 副口阻塞有上界（50ms/次停顿），不能单独解释永久静默；仅当 J2 悬空的 D+/D− 噪声被误判为 SOF 时会反复付出 50ms |
| 观察窗 3 "冻结"现场 | `/tmp/freeze_watch_report.json`：`observed_s=1407, caught=silence-freeze, panic_markers=0`；`/tmp/freeze_watch.log`（1752 行）末尾：最后一条应用日志 `I (1389006) LV_EFC: perf ...`，紧接 `ESP-ROM:esp32p4-eco7-20260109` / `rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))` / `waiting for download` 重复 3 次，夹带 SLIP 帧式字节 | 芯片被 EN 复位且 BOOT 被拉低，进入 ROM 下载模式静默等待；三次复位 + SLIP 字节符合 esptool 连接重试特征。该事件是主机侧（另一进程或 USB 重枚举触发的 DTR/RTS 时序）诱发，`freeze_watch.py` 15s 无输出即判"静默冻结"，属误判 |
| 其他冻结报告 | §26（电台后两次）、§34（baseline 期间一次）无留存日志 | 无法复核，按未定性处理，由新的浸泡与旁证判定 |
| EN 复位的复位码 | 同一日志：主机诱发的 EN 复位显示 `rst:0x1 (POWERON)`；`components/esp_system/port/soc/esp32p4/reset_reason.c` 把 `RESET_REASON_CHIP_POWER_ON` 映射为 `ESP_RST_POWERON` | 人工 RST 与冷上电在硬件复位码上不可区分，需靠 LP SRAM 保留区是否存活来区分（实验 E0） |
| 芯片与组件版本 | 同一日志：`Chip rev: v3.2`、PSRAM AP 256Mbit HEX 200MHz、heap 区 `RTCRAM 31 KiB`；`idf_component.yml`：LVGL 9.6.0、esp_lvgl_port 2.9.0、esp_hosted 3.0.10；表情组件由 esp_lv_eaf_player 0.3.0 改为 esp_efc_codec 1.1.0（2026-11-15，见执行记录） | 设计按这些版本引用源码 |
| 表情黑屏（EAF 路线，已退役） | `tools/eaf_codec.py` `encode_frame()` 写入 alpha=0；`lv_eaf.c` 据此发布 RGB565A8；`esp_eaf_dec.c` 跳过 alpha=0 像素。task 2 的 lint 结果：三份素材调色板项 alpha=0x00 占比 100%（11520/11520、5632/5632、3840/3840），`evidence/2/` | 已证实。EAF 路线退役，不再修补 |
| §44 "LVGL9 对 RAW 不能缩放"（EAF 路线，已修正） | `lv_eaf.c` 与 `lv_efc.c` 都发布 RGB565/RGB565A8 图像；EAF 黑屏的直接原因是素材 alpha（上一行），不是缩放 | 归因不成立，勘误见任务 38.3 |
| esp_efc_codec 1.1.0 发布包 | ESP Component Registry 索引（`components-file.espressif.com/components/espressif/esp_efc_codec.json`）列出 1.1.0，归档 `espressif__esp_efc_codec-v1.1.0.zip`，归档 sha256 `d50c10fb…9316`；`CHECKSUMS.json` 列出的 49 个文件全部一致（`evidence/source/esp_efc_codec_1.1.0/`） | 已取证 |
| 许可证 | `LICENSE`：Espressif Modified MIT，"Permission ... EXCLUSIVELY with Espressif Systems products"，并禁止为非乐鑫产品再分发源码或二进制 | 固件面向乐鑫 SoC，属允许范围。仓库是否对外公开，需用户确认（见用户事项） |
| 组件结构 | 解码器核心为目标相关预编译库 `libs/<target>/libesp_efc_codec.a`（esp32、esp32s3、esp32p4、esp32c3、esp32c6、esp32s31），源码只含包装层 `src/esp_efc_codec.c` 与 `efc_player/lv_efc.c`；**没有宿主（x86）版本** | 宿主测试不能用 gcc 编译设备解码器，见 Testing Strategy 的替代方案 |
| 解码器接口 | `esp_efc_dec_open`（引用 blob，不复制，blob 须在解码器生命周期内有效）；`esp_efc_dec_get_info`（含 `scaled`、`coded_width/height`、`tile_size`、`color_key_index`、`changed_x/y_start/end`）；`esp_efc_dec_frame`（RGB565，输出缓冲跨帧保留，不得清零）；`esp_efc_dec_frame_with_alpha`（RGB565A8）；`esp_efc_dec_close`。Kconfig `ESP_EFC_DECODE_4X4_ONLY` 默认 y，只接受 tile 4 的 blob | 已取证（`include/esp_efc_codec.h`、`src/esp_efc_codec.c`、`Kconfig`） |
| lv_efc 行为 | `lv_efc_set_src_data` 与 `lv_efc_set_src`（dsc 或路径）都**复制整个 blob** 到 PSRAM（`efc_malloc`，PSRAM 优先）；加载时在调用者上下文**同步解码第 0 帧**并**恢复播放定时器**（`show_loaded`）；`lv_efc_play` 会再次解码当前帧；对象尺寸不小于片段时为 1:1 显示，不分配缩放缓冲，否则走 PPA 或软件最近邻；`publish` 在帧尺寸不变时只失效变化外接框 | 已取证（`efc_player/lv_efc.c`、`efc_player/lv_efc_private.h`） |
| 分辨率约束 | readme：对象只做最近邻缩小、不放大；片段宽高须为 4 的倍数 | 460 为 4 的倍数；对象尺寸须等于片段尺寸 |
| Emote Pack 素材 | `evidence/source/EmotePack.zip`（2,317,944 字节，sha256 `946654de…15e6`）解出 `gif/` 23 个文件，全部 848×848，帧时长全部 40 ms，透明背景（GIF 透明标志为真，约 83% 像素透明），文件名时长标签与 GIF 时间轴不一致（如 `badminton_[1.2s]` 为 2.0 s、`yawn_[2.0s]` 为 3.0 s） | 已取证（`evidence/source/efc_460/gif_facts.json`、`gif_extent.json`、`contact_sheet_848_over_bg.png`） |
| readme 与发布包不一致 | readme 提到的 `pc/js/dist/encode_gif.mjs` 与 `encode.html` 不在 1.1.0 包内（49 个文件已核对）。可用的官方 PC 编码器为 `pc/efc_py.py`（`encode_frames`、`decode_frames`）与 `pc/video_to_efc.py`（`encode()`，内置无损往返校验）。RGB565 量化为截断（R>>3、G>>2、B>>3） | 采用 `video_to_efc.encode()`，宿主以 `efc_py.decode_frames` 复核 |
| PC 实测编码（460×460，100 ms 采样，tile 4，zlib-only） | 编码器对 23 段均选 zlib-only（PRED 候选更大，未采用）。带键 23 段合计 206,846 字节（0.197 MiB），最小 2,622、中位 7,545、最大 44,565 字节，编码合计 12.6 s，23 段全部无损；不透明合成版合计 1,149,908 字节（5.56 倍）。带键版调色板 2–65 色（yummy 39 色，shy 3 色） | `evidence/source/efc_460/SIZES.md`、`sizes.json` |
| 板上既有 EFC 数据（docs/19 §26，批次 21） | 450×450 演示片段，PPA 缩放路径：decode 2–7 ms、prepare 3–8 ms、publish 0.2 ms/帧；未覆盖本设计的 1:1 与键控路径 | 仅作参考，须在任务 33 重测 |
| Flash 与 PSRAM | P4C5 分区表：factory app 0xA00000（10 MiB）（`firmware/partitions-p4c5.csv`）；docs/09 记录 flash 余量约 0.8 MB（规划口径，需以实际镜像复核）；PSRAM 启动时空闲约 30.9 MB（docs/19 §46 的 `psram=30.9MB`） | 预算见资源预算表 |

### Bug Condition

**缺陷 #1**：设备在任意负载（含完全空闲）运行到随机时刻后串口停止输出，且没有任何机制能在无人干预时恢复。可能的直接机制分三类：主机侧把芯片复位进下载模式或桥接链路停滞（设备可能仍在运行）；CPU 仍在运行但业务任务或日志通路阻塞（TWDT 看不到）；HP 域时钟/电源级停摆连同 MWDT 一起停（IWDT 也无法复位）。现有系统对三类都既不能自动恢复，也不留可区分的证据。

**Formal Specification:**
```
FUNCTION isBugCondition_freeze(X)
  INPUT: X = { fault, host_event, console_path, idle_running, uptime_ms }
         fault        ∈ {NONE, CPU_BOTH_STOPPED, IRQ_OFF_BOTH, BUS_STALL, HP_CLOCK_STALL,
                         KEY_TASK_STALL, LOG_PATH_BLOCKED, IRQ_OFF_ONE, SPIN_STARVE_IDLE}
         host_event   ∈ {NONE, DTR_RTS_DOWNLOAD_RESET, PORT_CONTENTION, BRIDGE_STALL}
         console_path ∈ {OK, UART_TX_UNROUTED, USJ_SPURIOUS_CONNECTED, LOCK_HELD_BLOCKED}
  OUTPUT: boolean

  // 现有 WDT 已能捕获的类别不属于缺陷(3.5 保持)
  IF X.fault IN {IRQ_OFF_ONE, SPIN_STARVE_IDLE} THEN RETURN false

  RETURN X.host_event IN {DTR_RTS_DOWNLOAD_RESET, PORT_CONTENTION, BRIDGE_STALL}
      OR X.fault IN {CPU_BOTH_STOPPED, IRQ_OFF_BOTH, BUS_STALL, HP_CLOCK_STALL}
      OR (X.idle_running AND X.fault IN {KEY_TASK_STALL, LOG_PATH_BLOCKED})
      OR X.console_path IN {UART_TX_UNROUTED, USJ_SPURIOUS_CONNECTED, LOCK_HELD_BLOCKED}
END FUNCTION
```

说明：`IRQ_OFF_BOTH` 与 `BUS_STALL` 在 F 上实际会被 IWDT stage1 复位（见取证表），但 F 不报告复位来源、不留现场（1.3），所以仍属缺陷输入；F' 对它们的要求是"复位来源可辨识 + 现场记录指向注入核心"。

### Bug Condition（表情，EFC 路线）

**缺陷 #2**：RST/上电后 splash 卡顿，splash 结束后主屏黑屏（当前 EAF 固件的实际缺陷）。EFC 路线还需排除同类风险：素材未经预检即替换、加载时在 LVGL 锁内读文件、同一帧重复解码、加载后播放定时器被意外恢复。

**Formal Specification:**
```
FUNCTION isBugCondition_face(Y)
  INPUT: Y = { phase, clip, loader, splash_active, panel_open }
         phase  ∈ {SPLASH, SPLASH_TO_MAIN, MAIN_PLAYBACK, MOOD_SWITCH, SD_HOTLOAD, CLI_FACE}
         clip   = { w, h, key_share_permille, frame_count, bytes, prechecked }
         loader ∈ {EMBEDDED, SD_PATH_UNDER_LOCK, SD_BUFFER_UNLOCKED}
  OUTPUT: boolean

  RETURN (Y.phase = SPLASH AND lv_efc_running_during_splash(Y))
      OR (Y.phase = SPLASH AND sd_hotload_overlaps_splash(Y))
      OR (Y.phase IN {SPLASH_TO_MAIN, MAIN_PLAYBACK, MOOD_SWITCH, SD_HOTLOAD, CLI_FACE}
          AND visible_face_pixels(Y) = 0)
      OR (Y.phase = MAIN_PLAYBACK AND Y.clip.w = 460 AND Y.clip.h = 460
          AND NOT measured(T_P95(Y.clip)))
      OR (Y.phase IN {MOOD_SWITCH, SD_HOTLOAD, CLI_FACE} AND decode_count_frame0(Y) > 1)
      OR (Y.phase IN {SD_HOTLOAD, CLI_FACE} AND Y.loader = SD_PATH_UNDER_LOCK)
      OR (Y.phase IN {SD_HOTLOAD, CLI_FACE} AND NOT Y.clip.prechecked)
END FUNCTION
```

其中 `visible_face_pixels(Y) = 0` 在两种情况下成立：当前 EAF 三份素材（调色板 alpha 全 0），以及键色占比 ≥99% 的 EFC 片段（素材整屏透明，预检必须拒绝）。

### Examples

- **当前 EAF 固件（F0，已发生）**：RST 后 splash 明显卡顿；splash 结束后屏幕只剩 `C_BG`。串口日志为 `face src 460x460`、`clip ok: 46 frames @100ms`，系统存活（§46），并输出 `cf=RGB565A8 alpha0=100%`。预期：五官可见，不黑屏。（任务 28 在板上复跑确认。）
- **当前 EAF 固件的几何**：125×160 源帧被拉到 460×460，x 方向 ×3.68、y 方向 ×2.875，眼睛横向被拉宽（1.8）。
- **EFC 带键片段（预期）**：启动日志 `cf=RGB565A8`，`alpha0` 与 MANIFEST 中该片段第 0 帧的键占比（`frame0_key_permille` ÷ 10）一致（±0.5 个百分点）；屏幕自检 lit 占比与 MANIFEST 中该片段第 0 帧的 lit 占比（`frame0_lit_share_pct`）差 ≤5 个百分点。
- **反例（预检必须拒绝）**：键占比 ≥99% 的 EFC（整屏透明）。预检返回 `REJECT_MOSTLY_KEYED`，保留当前表情并告警。
- **反例（边界）**：对象尺寸小于片段尺寸时 lv_efc 进入缩放路径（PPA 或软件最近邻，软件路径在内部 RAM 分配 `x_idx`）。本设计禁止这一路径，加载后断言对象尺寸为 460。

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- 宿主 `tools/host_tests.sh` 原 14 套件全部 PASS，`tools/check_layers.py` 全绿；S31、P4C5（`-DSDKCONFIG=$PWD/sdkconfig.p4c5`，`firmware/build-p4c5`）、sim 三目标构建退出码均为 0（3.1、3.2）。
- 启动联网流程（WiFi got_ip、SNTP、mixer health 心跳）、SRP 配对与 mTLS 管理通道授权规则不变；新增旁证不绕过授权、不暴露凭据和用户数据（3.3）。
- `tools/reboot_soak.py` 每次启动 got_ip，无 abort/assert/guru/hang（3.4）。
- TWDT/IWDT 现有可捕获场景（饿死 idle、单核长时间关中断）继续 panic、输出双核回溯并自动重启（3.5）。
- 合法长耗时操作（启动初始化、SD/NVS 读写、flash 操作、TLS/SRP 计算、表情素材加载、WiFi 重连）不被新机制误复位（3.6）。
- 任意复位发生在 SD/NVS 写入期间时，双槽/提交代协议保证数据可读回（3.7）。
- 开机电台关、soul 安静时段自动停播、音量默认值与运行时设置不变；自动复位后的开机流程与人工复位一致，不按复位原因分支业务行为（3.14）。
- 常规日志（含新增诊断、现场记录、帧耗时日志）不含用户语音/对话原文与凭据（3.15，NFR-03）。
- CLI 等任务栈分配成功；内部堆余量与最大连续块不低于修复前基线；新增缓冲走堆/PSRAM 或 LP SRAM 保留区，不新增内部 RAM 静态缓冲（3.16）。
- UART0 115200 的 CLI 输入输出、`cputop`、`baseline <n>`、`tools/p4_cli.py`、`tools/freeze_watch.py` 正常（3.17）。
- 启动日志可辨识固件版本/构建标识（3.18）。
- 固件不占用 GPIO24/25、不烧写禁用 JTAG 的 eFuse，OpenOCD 已可附着（3.19）；调试器附着时新 LP WDT 自动停用，避免 halt 期间被复位。方案 A：USB-JTAG 经 J2 第 33/34 脚（GPIO24 D−、GPIO25 D+），OpenOCD 使用 board/esp32p4-builtin.cfg；调试器接入时 LP WDT 不武装（3.19）。
- `sdkconfig.p4c5` 保持 gitignored，不复制、不提交其中凭据（3.20）。
- splash 内容（立方体、EspCube 标题、副标题、三点）与约 2.4s 后淡入主屏不变（3.8）。
- 3.9 已按采纳的映射修改（见 bugfix.md）；3.10 的帧间隔保持不变。
- 会话状态帧间隔调制（待机/聆听 100ms、思考 200ms、说话 70ms）与底色呼吸效果不变；SD 热载片段沿用现有 50ms 帧间隔（3.10）。
- 面板展开暂停表情、收起恢复，自动收起与手势冷却闸不变（3.11）。
- 素材加载失败时显示情绪底色圆并告警（3.12）；SD `face/idle.efc` 与 CLI `face` 热载异步执行，预检失败时保留当前表情并告警（3.13）。
- sim 目标以 `CONFIG_ESP_CUBE_ENABLE_FACE_ANIM` 关闭表情代码构建，不链接 esp_efc_codec（3.21）。

**Scope:**
所有不落在 C₁ ∪ C₂ 内的输入都应与 F 的行为一致。具体包括：
- 正常运行（无故障注入、无主机端口争用）下的全部业务功能：电台、会话、记忆、配对、管理通道、触摸手势、面板。
- 现有 WDT 已覆盖的故障类别（单核关中断、自旋饿死 idle）的检测路径与输出格式。
- CLI 现有命令的语义与输出（新增命令只追加）。
- 非表情 UI 元素（状态栏、AI 状态面板、断网遮罩、时钟/音量）的内容与布局。

正确行为本身（P）以 Correctness Properties 为唯一定义来源。

## Hypothesized Root Cause

### 缺陷 #1：根因假设与区分实验

| 编号 | 假设 | 与已知事实的一致性 | 若成立，下次事件的可观测特征 |
|---|---|---|---|
| H0 | 主机侧假象：另一进程（esptool、`p4_cli.py`、`reboot_soak.py` 等）在同一 `/dev/ttyACM0` 上切换 DTR/RTS，或 CH343 USB 重枚举/主机 USB 自动挂起导致 DTR/RTS 抖动，把芯片 EN 复位进下载模式；`freeze_watch.py` 15s 静默即判冻结 | 观察窗 3 日志直接支持；所有工具都不独占串口；`p4_cli.py` 用默认参数打开端口（DTR/RTS 先被内核置位），`freeze_watch.py` 每开新窗口都重开端口（板上已知 DTR 毛刺会复位） | 串口出现 ROM 复位行（`boot:0x307`、`waiting for download`）；`journalctl -k` 同时刻有 CH343 断连/重连或另一进程打开端口 |
| H1a | CPU 在跑，日志通路的锁（stdout FILE 锁、`esp_stdio` 的 `s_lock`、USJ `write_lock`）被一个阻塞/挂起的任务持有，所有记日志的任务依次阻塞，idle 照常运行 | 解释零 panic、零复位；但需要一个"持锁后阻塞"的持有者，代码审读未发现，可能性中低 | ping 仍通；屏幕表情若不记日志则仍推进；现场记录中 `log.enter ≠ log.exit` 且长期不变 |
| H1b | CPU 在跑，UART0 TX 字节进 FIFO 并排空，但 GPIO38 的 IO_MUX/GPIO 矩阵路由或 UART0 配置被运行时代码改掉，线上无字节 | 解释零 panic、零复位、"复位即复"（复位恢复引脚配置）；`sdprobe` 等台调命令会读写引脚，需核查 | ping 通、屏幕推进；监督器每秒比对的控制台引脚/UART 配置签名发生变化 |
| H1c | J2 悬空的 USJ D+/D− 噪声被判为已连接，副口每次停顿付出 50ms，日志极慢 | 只造成变慢，不能单独造成永久静默；可能性低 | 记录中 USJ 连接态翻转计数 >0；logtx 单行写出耗时出现 ≥50ms 峰 |
| H1d | 设备正常，CH343 桥接或主机 USB/驱动停滞（§26"UART 写阻塞/USB 仍枚举"即主机写阻塞） | 与"主机写阻塞"描述一致；与"人工 RST 才恢复"部分矛盾（RST 不复位 CH343），需实验确认 | ping 通、屏幕推进；现场记录显示日志计数持续增长；主机侧 `journalctl -k` 有 USB 异常 |
| H3 | HP 域时钟/电源级停摆（PLL/时钟门控/电源毛刺），两核与 TIMG MWDT 同时停，IWDT 无法复位，BOD 未触发 | 唯一能同时解释"真停机 + 零复位"的硬件类假设；没有直接证据 | ping 断、屏幕定格；LP WDT（LP 域 RC_SLOW）复位，复位码为 RWDT 类；记录中两核 tick 在同一毫秒附近停止 |
| H2 | 两核卡在 PSRAM/cache/总线访问（HEX 200MHz 时序裕量、hosted SDIO DMA 与 cache 竞态） | 按 IWDT stage1 机制应在 ≤1.5s 复位并打印 ROM 行，与零复位矛盾；只有与 H3 叠加才成立 | ROM 行显示 MWDT 类复位码（或若与 H3 叠加则同 H3）；记录中两核 tick 停止；`Saved PC` 落在 PSRAM/cache 相关代码 |

**区分实验（全部无需 JTAG）：**

- **E0 保留区存活性**：在诊断固件中写入现场记录，分别用 RTS 脉冲（与 RST 键同一 EN 电路）、软件重启、LP WDT 注入复位、IWDT 注入复位后读回；冷上电（拔插 USB，需用户在场）后确认记录判为无效。结果决定启动报告能否区分"人工 RST"与"冷上电"：保留区在 EN 复位后仍有效则可区分，否则启动报告如实标注"上电或 EN 复位（硬件同码，保留区未存活）"。
- **E1 证据复核与工具止血**：`freeze_watch.py` 新增分类器重放 `/tmp/freeze_watch.log`，应判为"主机诱发下载模式"；所有串口工具改为独占打开（`TIOCEXCL`），其他进程打开同一端口直接得到 EBUSY 而不是复位芯片；浸泡期间同步记录 `journalctl -k -f` 中 CH343 的 USB 事件。
- **E2 IWDT 第二级实证**：CLI 注入"两核同时清 `mstatus.MIE` 自旋"，预期 ≤1.5s 复位，ROM 行为 MWDT 类，启动报告归类 `MWDT_HW`。一旦成立，"中断全关/总线卡死却不复位"在本板上被实验排除，H2 只能以 H3 叠加形式存在。
- **E3 旁证浸泡**：诊断 + 修复固件下，`freeze_watch.py`（独占、常开不重开）+ `tools/liveness_watch.py`（每 2s ping 设备 IP）同时运行累计 ≥4h（含 ≥2h 完全空闲）。任何静默事件按下表判定：

| 串口 | ping | 屏幕/记录中的 UI 心跳 | 复位与记录 | 判定 |
|---|---|---|---|---|
| ROM 行含 `boot:0x307` / `waiting for download` | — | — | POWERON | H0 |
| 静默，无 ROM 行 | 通 | 推进 | 无自动复位；人工 RST 后记录（E0 证实保留区跨 EN 复位存活时）显示日志计数持续增长，否则以 ping、屏幕与主机 USB 事件为准 | H1d |
| 静默，无 ROM 行 | 通 | 推进 | 监督器 LOG_STALL 复位；`log.enter ≠ log.exit`；直写 FIFO 探针行可见 | H1a |
| 静默，无 ROM 行 | 通 | 推进 | 记录 `con_pin_ok=0` | H1b |
| 静默后 ≤13s 出现 ROM 行 | 断 | 定格 | RWDT 类复位码；两核 tick 同时停 | H3（或 H2+H3） |
| 静默后 ≤1.5s 出现 ROM 行 | 断 | 定格 | MWDT 类复位码 | H2 |

- **E4 PSRAM 降频对照**：仅当 E3 捕获到 ≥1 次 H3/H2 类事件时执行。在 `sdkconfig.p4c5` 中原地把 `CONFIG_SPIRAM_SPEED_200M` 切到 `CONFIG_SPIRAM_SPEED_80M`，构建到独立目录 `firmware/build-p4c5-psram80`，构建后立刻改回原值并用 diff 确认只改动该项（不复制该文件，避免复制凭据）。80MHz 固件浸泡时长 ≥ max(4h, 2× 200MHz 下观测到的平均事件间隔)；事件消失则判 PSRAM 时序相关，再评估 120/160MHz 档或保持降频的性能代价。80MHz 会拖慢表情解码与渲染，表情性能验收一律在 200MHz 下做。
- **E5 USJ 副控制台排查**：监督器每秒采样 `usb_serial_jtag_is_connected()` 并累计翻转次数，logtx 记录单行写出耗时直方图。J2 未接线时出现翻转或 ≥50ms 写出峰，则原地切到 `CONFIG_ESP_CONSOLE_SECONDARY_NONE`（不影响 USJ 的 JTAG 功能，满足 3.19），否则保持现状。
- **E6 日志通路阻塞注入**：CLI 注入"一个任务 `flockfile(stdout)` 后永久阻塞"，验证 UI/mixer 不受影响、监督器在 ≤26s 内复位、记录指向 LOG_STALL。

### 缺陷 #2（EFC 路线）：根因状态与实验

| 编号 | 假设 | 状态 | 依据 / 处置 |
|---|---|---|---|
| B1 | 素材调色板 alpha 全为 0，整帧透明，只剩底色 | 已证实（EAF）；EAF 退役 | `evidence/2/` lint；EFC 路线的键色只标记真实透明像素，并由预检拒绝整屏透明片段（2.14） |
| B2 | splash 期间播放定时器仍在 LVGL 任务内解码，与 33ms 的 splash 主定时器争用 | 已证实（EAF 代码路径）；EFC 同样有风险：`lv_efc_set_src*` 会恢复定时器（`show_loaded`） | 加载前后按 running 状态重新挂起（B2、Property 10） |
| B3 | 逐帧成本：EAF 每帧 memset 整帧缓冲、每块两次 malloc/free、RGB565A8 混合 | EAF 部分已确认；EFC 的 1:1 显示路径无逐帧整帧清零，但键控片段每帧对变化外接框生成 alpha | 板上实测（任务 33）；若超预算走 B2 的 R 规则 |
| B4 | 同一帧重复解码：EAF 的 `set_src_data` 之后又 `restart`；EFC 的 `set_src_data` 之后再调 `lv_efc_play` | 已证实（EAF 代码，`ui_widget.c` 两处 restart）；EFC 路线若照抄会重演 | EFC 只调用一次 `set_src_data`，禁止紧随 `lv_efc_play` |
| B5 | 非等比拉伸（125×160 → 460×460） | 已证实（EAF） | EFC 源为 848×848 方形，等比缩放到 460 |
| B6 | 文档误述："RAW 不可缩放"与源码不符；EAF 的 §44/§46 归因需更正 | 已证实 | 任务 38.3、38.4 更正 |
| B7 | 路径接口在 LVGL 锁内读 SD 文件（`lv_efc_set_src(path)` 会整文件读入） | 设计风险（代码审读） | 读取移到锁外的加载任务，锁内只做 `lv_efc_set_src_data`（复制 + 第 0 帧解码，实测纳入 D0 预算） |
| B8 | `set_src` 副作用：加载后播放定时器被恢复，若此时 splash 或面板仍要求暂停，画面会继续推进 | 已证实（`lv_efc.c` `show_loaded`） | 每次加载、热载与 mood 切换后按 running 重挂（Property 10） |
| B9 | 解码器内部分配位于内部 RAM 还是 PSRAM 未知（预编译库，无源码） | 未知 | 任务 31.5 与 33 以加载前后的 `MALLOC_CAP_INTERNAL` 对照确认（3.16） |
| B10 | 底色圆可见时，呼吸动画每步使整圆失效并重绘（约 185k 像素），增加 R 成本 | 风险；现状隐藏底色圆（批次 21 的理由） | 已决（2026-11-15）：底色圆维持隐藏（D-B10） |

### 板上实测方案（EFC 路线）

- **D（播放定时器回调）**：lv_efc 的播放定时器在对象构造时创建（`lv_timer_create(next_frame_cb, …, obj)`，user_data 为对象），加载与切换不重建定时器。`face_perf` 在首次挂接时遍历 `lv_timer_get_next()`，找到 `user_data == efc 对象` 的定时器，经私有头 `src/misc/lv_timer_private.h` 取出原回调，用 `lv_timer_set_cb()` 换成计时包装。包装只记录使帧号前进的调用（与 EAF 版相同的计数口径）。D 为回调总耗时，即解码、键控准备（alpha 生成）与发布之和。
- **D 的分项与纯解码**：`CONFIG_ESP_EFC_PERF_LOG` 每秒输出 decode、prepare、publish、outside_cb 的平均值，作为交叉核对。纯解码耗时可选用 `-Wl,--wrap=esp_efc_dec_frame` 获得（只包装 lv_efc 对解码函数的未定义引用，不改组件源码），默认关闭，启用前需在构建期验证。
- **R、F、I_refr、I_loop**：`lv_display_add_event_cb()` 订阅的事件与 EAF 版一致；样本环、百分位与 P95 取整规则不变（`face_perf_stats`，任务 4.1）。
- **splash 摘要与首帧**：与 EAF 版一致（`splash: n=… p95=… max=… gt100=… d=…`；`face first refresh +Xms`）。`d` 为 splash 期间的 D 样本数，目标为 0。
- **屏幕自检**：主屏首次渲染完成后，在表情圆内 32×32 采样，输出 `face screen check: lit=xx%`（口径见 Glossary）。与 MANIFEST 的 `frame0_lit_share_pct`（第 0 帧）对照。
- **加载日志**：每次加载（内置、热载、mood 切换、CLI）输出 `face clip=<名> mood=<tag> delay=<ms> cf=<RGB565|RGB565A8|other|none> alpha0=<x.y%|n/a>`。`cf` 与 `alpha0` 由加载后 `lv_image_get_src()` 读出，只遍历一次 alpha 平面。加载时 lv_efc 已同步解码第 0 帧，故 `alpha0` 即第 0 帧的键占比。
- **CLI**：`faceperf start <秒> <片段>` 在 LVGL 锁内用 `lv_efc_set_src_data()` 切到指定内置片段，测完按原暂停态恢复（不追加 restart）；`faceperf report` 与 `uimood <tag>` 同 EAF 版的语义。

## Correctness Properties

Property 1: Bug Condition - 冻结类故障自动恢复且现场可辨识

_For any_ 设备侧故障输入 X 满足 isBugCondition_freeze(X) 且 X.host_event = NONE（两核关中断、核心停摆/总线卡死、HP 时钟停摆、idle 照常运行但关键任务或日志通路停滞、控制台通路不可写），修复后的固件 SHALL 在故障发生后 ≤30s 内自动复位并按正常开机流程恢复（WiFi got_ip、SNTP、mixer 心跳）；下次启动日志 SHALL 输出可区分的复位类别与累计异常复位计数，以及通过校验的现场记录（各核心与 UI/mixer/CLI/网络/logtx 的最后活动标记与时间戳、上次运行时长、最后一次心跳摘要），记录指向故障所在的核心或任务；故障期间日志生产者 SHALL 不阻塞，现场记录 SHALL 足以区分"CPU 全停"与"仅控制台/日志通路阻塞"。对 X.host_event ≠ NONE 的主机诱发事件，串口工具 SHALL 阻止端口争用并把 ROM 下载模式/复位行判为主机诱发，不计为设备停机。

**Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.6**

Property 2: Preservation - 非故障输入下系统行为不变且无误复位

_For any_ 输入 X 满足 isBugCondition_freeze(X) = false（正常运行、合法长耗时操作、现有 WDT 可捕获的软件级卡死），修复后的固件 SHALL 产生与原固件相同的可观测行为：不发生新机制引起的复位；TWDT/IWDT 可捕获场景仍 panic、输出双核回溯并自动重启；启动、联网、配对授权、数据持久化、开机电台关/安静时段/音量策略、CLI 与串口工具、构建与分层校验、固件版本标识、JTAG 可用性和凭据隔离保持不变；内部堆余量与最大连续块不低于修复前基线。

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.14, 3.15, 3.16, 3.17, 3.18, 3.19, 3.20**

Property 3: Bug Condition - 表情可见、流畅且达到渲染能力门限

_For any_ 输入 Y 满足 isBugCondition_face(Y)（splash 播放、splash 切主屏、主屏播放、mood 切换、SD 热载、CLI face 替换），修复后的固件 SHALL 使 splash 期间 I_refr 的 P95 ≤50ms 且无单次 >100ms，且 splash 期间 D 样本数为 0；splash 结束后立即显示持续推进、键占比与 MANIFEST 一致的表情（连续 ≥10 次 RST/上电每次如此）；播放与切换期间 I_loop 的 P95 ≤50ms 且无单次 >100ms，触摸手势与面板保持响应；简单片段 T = D + R + F 的 P95 ≤33ms，复杂片段 ≤66ms，并以注明样本数与场景的逐段计时报告和 cputop 为证据。

**Validates: Requirements 2.7, 2.8, 2.9, 2.10, 2.11, 2.12**

Property 4: Preservation - 表情与 UI 既有语义不变

_For any_ 不落在 isBugCondition_face 内的 UI 输入（splash 内容与时长、mood_tag 映射、会话状态帧间隔与底色呼吸、面板暂停/恢复与手势、加载失败回退、SD/CLI 热载失败保留当前表情），修复后的固件 SHALL 产生与 F0 相同的 UI 语义（mood 映射除外，按 bugfix.md 3.9 的采纳映射执行）：splash 内容不变且约 2.4s 后淡入主屏；mood 映射按采纳的 B6 映射执行（见 bugfix.md 3.9）并从头播放；测试阶段未内置片段回退 smile_static（2.15、3.22）；帧间隔 100/200/70ms（SD 热载 50ms）不变；面板展开暂停、收起恢复；素材无效时显示情绪底色圆或保留当前表情并告警；sim 构建不链接 esp_efc_codec。

**Validates: Requirements 3.8, 3.9, 3.10, 3.11, 3.12, 3.13, 3.21**

Property 5: 现场记录编解码与复位分类的完备性

_For any_ 现场记录字节序列（含随机位翻转、截断、冷上电随机内容）与任意（ROM 复位码 × 复位提示 × 注入标记）组合，`sysdiag_core` 的校验 SHALL 要么判为无效并只输出"无有效现场记录"，要么还原出与写入时完全一致的字段；分类函数 SHALL 对所有组合给出确定且唯一的类别，输出文本只来自固定词表与数值字段，不含任何自由文本。

**Validates: Requirements 2.3, 3.15**

Property 6: 监督策略的限时检出与无误报

_For any_ 心跳时间序列，若每个被监视对象的心跳间隔都不超过其阈值（含声明的 busy 窗口、≤0.5s 的 flash 操作停顿、启动宽限期），监督策略 SHALL 每个周期都喂 LP WDT；若某个触发型对象（UI、mixer、logtx）在 t₀ 后停止心跳，策略 SHALL 在 [t₀ + 阈值, t₀ + 阈值 + 1s] 内给出该对象的触发原因，且从 t₀ 到复位的上界 ≤30s。

**Validates: Requirements 2.1, 2.2, 3.6**

Property 7: 日志环非阻塞与保序

_For any_ 生产者/消费者交错（任意并发写入、消费者任意停顿或永久阻塞、任意环容量），日志写入 SHALL 在有界步数内返回且从不等待消费者；消费者输出的序列 SHALL 等于生产序列去掉被丢弃项后的有序子序列，丢弃计数等于被拒绝的写入数，任一输出行不被撕裂。

**Validates: Requirements 2.4, 3.17**

Property 8: 素材流水线的无损往返、键控语义与来源可复现

_For any_ 随机 RGB565 帧序列 X（宽高为 4 的倍数）与键掩码 K，经 esp_efc_codec 官方编码器（tile 4、zlib-only）编码后，解码结果 SHALL 与 X 逐元素相等，解码后键像素 alpha SHALL 为 0、其余 SHALL 为 0xFF；对任意源 GIF，输出帧数 SHALL 等于 round(GIF 总时长 / 100ms)，848→460 的缩放 SHALL 为等比的正方形缩放；MANIFEST 中记录的 sha256 SHALL 与落盘文件一致，且来源归档 sha256 SHALL 等于 d50c10fb…9316。

**Validates: Requirements 2.9, 2.13**

Property 9: 预检判定的完备性

_For any_ 解码器返回的组合（open 结果、get_info 各字段、第 0 帧键占比、字节数），`face_efc_precheck` SHALL 接受当且仅当：open 成功 ∧ 宽高均为 460 ∧ 未缩放（scaled=false，coded 与显示尺寸相等）∧ tile 为 4 ∧ 帧数在 [1, 300] 内 ∧ 字节数 ≤262144 ∧ 键占比 <99%；拒绝时 SHALL 给出固定词表中的唯一原因码，且不改变当前显示。

**Validates: Requirements 2.14, 3.12, 3.13**

Property 10: 挂起状态机与加载副作用

_For any_ 由 splash 结束、面板展开/收起、内置加载、SD 热载、mood 切换与 CLI 替换组成的事件序列，设备侧 SHALL 在每次加载后把播放定时器的运行态重置为 running = ¬splash ∧ ¬panel；在 running = false 的期间，解码次数 SHALL 为 0（加载时同步解码的第 0 帧除外）。

**Validates: Requirements 2.7, 2.8, 2.12, 3.11**

## Fix Implementation

### Changes Required

Assuming our root cause analysis is correct:

#### A. 冻结：新组件 `sysdiag`（L0）

> 条件设计（2026-11-16）：仅在无调试器浸泡或 JTAG 抓取确认冻结复现后实施。未复现时本节不实施。

**File**: `firmware/components/sysdiag/`（新建；`include/sysdiag.h`、`src/sysdiag_core.c`、`src/sysdiag_esp.c`、`Kconfig`、`CMakeLists.txt`、`test/test_sysdiag.c`）

**Function**: `sysdiag_early_init()`、`sysdiag_boot_report()`、`sysdiag_start()`、`sysdiag_hb()`、`sysdiag_busy()`/`sysdiag_idle()`、`sysdiag_register_probe()`、`sysdiag_inject()`、`sysdiag_status_print()`

**Specific Changes**:
1. **分层登记**：`tools/check_layers.py` 的 `LAYERS` 加 `"sysdiag": 0`，docs/06 §1 分层图同步。L0 是全层可依赖的基础件，mixer（L2）与 main（L5）可直接调用 `sysdiag_hb()`；`app_ui/ui/`（L4，需与 sim 同源编译）不调用 sysdiag，UI 心跳由 L5 代为上报。`sysdiag_core.c` 为纯 C、零 OS 依赖，宿主可测；`sysdiag_esp.c` 为设备胶水。网络探针用依赖注入（`sysdiag_register_probe()`），sysdiag 本身不依赖 lwip。
2. **现场记录（`RTC_NOINIT_ATTR`，LP SRAM，预算 ≤512B，预计约 300B）**：

   | 区段 | 字段 | 写入者/频率 |
   |---|---|---|
   | 头 | `magic='RSTD'`、`ver`、`size`、`boot_seq`、`boot_seq_inv`、头 CRC32 | 启动时 |
   | live | 每核 `{tick 计数, last_ms, 当前任务名前 8 字节}` | 每核 FreeRTOS tick hook，每 8 tick 一次（`esp_register_freertos_tick_hook_for_cpu`） |
   | live | 关键任务 `{last_ms, site(u16 代码位置), state(idle/busy/in_log)}`：UI、MIXER、CLI、NET、LOGTX、SUPV | 各任务主循环 |
   | live | 日志 `{enter, exit, tx_bytes, drops, pending, last_tx_ms}`、`con_pin_ok`、`usj_flips` | 日志钩子/logtx/监督器 |
   | live | `trigger`、`trigger_ms`、`inject_kind` | 监督器/注入命令 |
   | sealed | live 快照 + `uptime_ms` + `epoch_s`（已校时才填）+ 心跳摘要 `{内部空闲, 内部最小, 内部最大块, PSRAM 空闲, wifi_up, ai_state, player, face_frames, refr_cnt}` + CRC32 | 监督器每 1s 封存一次，心跳摘要每 10s 更新 |

   所有写入都是对齐的 32 位存储，不加锁、可在 ISR 中执行。启动时以 sealed 区（CRC 通过）为准，live 区作为"最后 1s 内"的补充并标注未封存。记录只含枚举、数值与任务名前缀，不含日志文本、对话或凭据。
3. **LP WDT**：`sysdiag_early_init()` 在 `app_main()` 首行用 `wdt_hal_init(&ctx, WDT_RWDT, 0, false)` 武装启动保护（stage0 60s `WDT_STAGE_ACTION_RESET_SYSTEM`）；`sysdiag_start()` 在 `startup complete` 后改为 stage0 12s `RESET_SYSTEM`、stage1 再 4s `RESET_RTC` 兜底，由监督器每 1s 调 `wdt_hal_feed()`。tick 数按 `rtc_clk_slow_freq_get_hz()` 换算（与 `bootloader_config_wdt()`、`esp_panic_handler_enable_rtc_wdt()` 相同写法）。不改 `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE`：IDF 在 `app_main()` 前关闭引导期 RWDT，我们在 `app_main()` 首行重新武装，中间空窗由 TWDT/IWDT 覆盖。stage0 选 `RESET_SYSTEM` 是为了保留 LP 域（记录所在），E0 验证；若板上发现 `RESET_SYSTEM` 不能恢复某类卡死，stage1 的 `RESET_RTC` 兜底。`esp_cpu_dbgr_is_attached()` 为真时不武装 LP WDT 并打印一行提示（3.19）。panic 路径中 IDF 会自行把 RWDT 重配为 `RESET_RTC`（`panic.c`），与本设计兼容。备选方案对比：

   | 方案 | 能覆盖 H3（HP 时钟/电源停摆） | 代价 | 结论 |
   |---|---|---|---|
   | LP WDT + HP 侧监督器喂狗 | 能（LP 域 RC_SLOW） | 小：几十行 HAL 调用，无新任务 | 采用 |
   | 关键任务订阅 TWDT（`esp_task_wdt_add`） | 不能（TWDT 依赖 TIMG0 与 HP 中断） | 小，但 10s 统一超时，CLI 等合法长阻塞会误报 | 不采用；TWDT 保持只看 idle |
   | LP core（`SOC_LP_CORE_SUPPORTED`）独立监视 LP SRAM 心跳 | 能，且可在 HP 停摆后继续记录 | 需 ULP 工具链与 LP 固件，LP UART 引脚未接主机 | 暂不采用；E3 若证实 H3 且需要更多停摆期数据再评估 |
   | 依赖硬件 Super WDT | 能 | 不可按业务心跳控制 | 保持芯片默认，不作为主机制 |
4. **监督器**：放在一个 1s 周期的 `esp_timer` 回调里运行（复用现有 esp_timer 任务，不新增任务栈）。每周期：读各心跳年龄、日志计数、控制台引脚/UART 配置签名（GPIO38/37 的 IO_MUX 与矩阵选择、UART0 时钟分频）、USJ 连接态；封存记录；按下表决定是否喂狗：

   | 对象 | 阈值（Kconfig 默认） | 超时后的动作 |
   |---|---|---|
   | HP CPU/监督器自身 | LP WDT 12s 未被喂 | 硬件 `RESET_SYSTEM`（不依赖 HP CPU、中断与调度） |
   | UI（LVGL 任务内 500ms `lv_timer` 心跳） | 10s | 记 `trigger=UI_STALL` → `esp_restart()`；若重启路径卡住，LP WDT 12s 兜底 |
   | MIXER（`audio_mixer.c` 主循环） | 5s | `trigger=MIXER_STALL` → 同上 |
   | LOGTX（有待发数据但无进展） | 15s | 先经 `esp_rom_output_putc` 直写 UART0 FIFO 一行 `[sysdiag] console stall`（绕过 VFS 锁，主机可见即说明硬件通路正常、是锁问题）；再等 10s 仍无进展则 `trigger=LOG_STALL` → 重启 |
   | CLI | 仅记录 | 不触发复位（`wget`/`ncm` 等命令可合法阻塞数分钟，执行期间标记 busy 与截止时间） |
   | NET（`tcpip_try_callback` 探针，每 2s） | 仅记录 | 不触发复位；浸泡证据足够后可经 Kconfig 升级为触发型 |

   启动宽限：`sysdiag_start()` 后 10s 才开始判定。最坏时限：UI 10s + 1s 周期 + 重启 ≤12s；LOGTX 15s + 10s + 1s ≤26s；HP 全停 ≤13s（含 RC_SLOW 频偏余量）；均 ≤30s。
5. **非阻塞日志**：`esp_log_set_vprintf()` 钩子（IDF 6.1 `CONFIG_LOG_VERSION_1`）把每条日志 `vsnprintf` 进 PSRAM 槽环（64 槽 × 256B = 16KB，`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）。生产者用原子递增取槽，槽未被消费即丢弃并计数，永不等待；超长行截断并标记。`logtx` 任务（`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`，栈 3KB，优先级 3，core0）按槽顺序整行 `fwrite(stdout)` + `fflush`，每写满约 115 字节（UART FIFO 128B 以内）`vTaskDelay(10ms)`，按 115200 波特率节奏写出，不在 FIFO 上忙等，也就不会饿死 idle。CLI 的 `printf` 保持同步直写（保证交互即时）；两者都经 stdout 的 FILE 锁，按行不撕裂。panic 输出走 ROM 直写，不受影响。代价：硬复位会丢失环中尚未写出的最多约 1s 日志，由现场记录补足。
6. **启动报告与分类**：`sysdiag_boot_report()` 在 `nvs_flash_init()` 之后、其余初始化之前执行，读取 `esp_rom_get_reset_reason(0/1)` 原始码、`esp_reset_reason()` 提示与记录：

   | 原始复位码（core0） | 提示 | 类别 | 异常计数 |
   |---|---|---|---|
   | `RESET_REASON_CHIP_POWER_ON` | — | 记录有效：`EN_RESET`（RST 键/串口 DTR-RTS）；无效：`POWERON_OR_EN`（E0 结论决定措辞） | 否 |
   | `RESET_REASON_CORE_SW` / `CPU0_SW` | `ESP_RST_PANIC` | `PANIC` | 是 |
   | 同上 | `ESP_RST_TASK_WDT` | `TASK_WDT`（panic） | 是 |
   | 同上 | `ESP_RST_INT_WDT` | `INT_WDT`（panic） | 是 |
   | 同上 | 无提示且记录 `trigger≠NONE` | `SUPERVISOR(UI_STALL/MIXER_STALL/LOG_STALL)` | 是 |
   | 同上 | 无提示且无触发 | `SW_RESTART` | 否 |
   | `CORE_MWDT` / `CPU_MWDT` | — | `MWDT_HW`（IWDT stage1 等） | 是 |
   | `CORE_RWDT` / `SYS_RWDT` / `CPU_RWDT` | — | `LP_WDT` | 是 |
   | `SYS_SUPER_WDT` / `SYS_BROWN_OUT` / `CPU_LOCKUP` / `CORE_PWR_GLITCH` | — | `SUPER_WDT` / `BROWNOUT` / `CPU_LOCKUP` / `PWR_GLITCH` | 是 |
   | 其他 | — | `UNKNOWN(0x..)` | 是 |

   `esp_reset_reason()` 会把所有硬件看门狗合并成 `ESP_RST_WDT`（`reset_reason.c`），所以分类以原始码为主。记录中 `inject_kind≠0` 时类别后追加 `(inject=<kind>)`。异常复位时累计计数写 NVS（命名空间 `sysdiag`，键 `abn`/`last_cls`/`last_up`），只在异常启动时写一次。输出示例：
   ```
   I (2600) sysdiag: fw=b5a3382-dirty elf=d973d258 built=Nov 14 2026 06:52:21
   I (2601) sysdiag: boot#124 reset=LP_WDT raw0=0x?? raw1=0x?? abn_total=3
   I (2602) sysdiag: last run 1407.2s trigger=NONE inject=none sealed@1407.0s
   I (2603) sysdiag: core0 tick@1406.98s task=mixer | core1 tick@1406.98s task=LVGL
   I (2604) sysdiag: ui=1406.6s(site1) mixer=1407.0s cli=1406.9s(idle) net=1406.0s logtx=1407.0s
   I (2605) sysdiag: log enter=58231 exit=58231 drops=0 pending=0 pin_ok=1 usj_flips=0
   I (2606) sysdiag: hb@1400.0s int_free=… int_min=… int_lb=… psram=… wifi=1 ai=0 player=0
   ```
   记录无效时只输出 `sysdiag: 无有效现场记录(冷上电或校验失败)`。报告后记录重置为本次启动（`boot_seq+1`，live/sealed 清零）。ROM 横幅若带 `Saved PC`（`CONFIG_ESP_SYSTEM_HW_PC_RECORD=y`，assist_debug PC 记录，见 `hal/esp32p4/include/hal/assist_debug_ll.h`），由 `freeze_watch.py` 提取并给出 `riscv32-esp-elf-addr2line -pfiaC -e firmware/build-p4c5/esp_cube.elf <pc>` 结果；该寄存器在 RWDT `RESET_SYSTEM` 后是否保留，由 E0 一并验证。
7. **故障注入（仅 UART CLI）**：`inject <kind> [core] CONFIRM`，受 Kconfig `ESP_CUBE_SYSDIAG_INJECT`（开发默认 y）门控；管理通道与 App 不注册任何注入路由。执行前先写 `inject_kind` 到记录：

   | kind | 实现 | 预期复位路径 |
   |---|---|---|
   | `irqoff <0/1>` | 绑核高优先级任务清 `mstatus.MIE`（`RV_CLEAR_CSR`，屏蔽含 Level 4 IWDT 在内的全部中断）后自旋 | 另一核响应 IWDT stage0 → panic `INT_WDT` |
   | `irqoff both` | 两核任务屏障同步后同时清 MIE 自旋 | IWDT stage1 → `MWDT_HW`（E2） |
   | `irqoff both nowdt` | 先确保 LP WDT 已武装（阶段 A 由该命令临时以 12s 武装），再禁用 TIMG0/TIMG1 WDT，两核清 MIE 自旋 | 只剩 LP WDT → `LP_WDT` |
   | `stall` | `esp_cpu_stall(other)` 冻结另一核，本核清 MIE 自旋 | IWDT stage1 → `MWDT_HW` |
   | `task ui` / `task mixer` | 让目标任务进入 `vTaskDelay` 死循环（阻塞而非自旋，idle 照常） | `SUPERVISOR(UI_STALL/MIXER_STALL)` |
   | `logblock` | 新建任务 `flockfile(stdout)` 后永久阻塞 | `SUPERVISOR(LOG_STALL)`，期间 UI/mixer 心跳持续 |
   | `spin <0/1>` | 高优先级任务开中断自旋（保持行为对照） | TWDT panic `TASK_WDT` |

**File**: `firmware/main/main.c`

**Function**: `app_main()`

**Specific Changes**:
1. 首行 `sysdiag_early_init()`；`nvs_flash_init()` 之后调 `sysdiag_boot_report()`；`startup complete` 之后调 `sysdiag_start()`。
2. 注册网络探针：`sysdiag_register_probe(SYSDIAG_T_NET, net_probe)`，`net_probe` 内 `tcpip_try_callback()` 投递一个只调 `sysdiag_hb(SYSDIAG_T_NET, 1)` 的回调。
3. 不新增任何按复位原因分支的业务逻辑（自动复位后开机流程与人工复位一致，2.1/3.14）。

**File**: `firmware/components/app_audio/audio_mixer.c`（及其 `CMakeLists.txt` 的 `PRIV_REQUIRES sysdiag`）

**Function**: mixer 任务主循环

**Specific Changes**:
1. 每次循环顶部 `sysdiag_hb(SYSDIAG_T_MIXER, 1)`，I2S 写后 `site=2`；单次 32 位存储，不改变混音时序。

**File**: `firmware/main/cli.c`

**Function**: `cli_task()`、`handle_line()`

**Specific Changes**:
1. `uart_read_bytes` 每轮（200ms 超时）上报 CLI 心跳；`handle_line()` 前后 `sysdiag_busy(SYSDIAG_T_CLI, deadline)`/`sysdiag_idle()`（`wget`/`ncm` 截止 300s，其余 60s）。
2. 新增命令 `rstinfo`（重印本次启动报告，PSRAM 副本）、`diag`（监督器实时状态：各心跳年龄、日志计数、LP WDT 状态、USJ 翻转、引脚签名；`diag corrupt` 在注入 Kconfig 打开时故意破坏记录 CRC，用于验证"无有效现场记录"分支）、`inject …`。`faceperf …` 与 `uimood <tag>` 属表情线（B4、任务 31.4）。现有命令语义不变。

**File**: `firmware/main/ui_port_esp.c`

**Function**: `ui_port_init()`

**Specific Changes**:
1. UI 启动后在 LVGL 锁内创建 500ms `lv_timer`，回调 `sysdiag_hb(SYSDIAG_T_UI, 1)`（运行于 LVGL 任务，能真实反映 UI 循环存活）。
2. 表情加载的改动（等待 splash、锁外读取与预检）属表情线，见 B3。

**File**: `tools/freeze_watch.py`、`tools/p4_cli.py`、`tools/reboot_soak.py`、`tools/p4_boot_capture.py`、`tools/serial_log.py`；新建 `tools/liveness_watch.py`、`tools/serial_excl.py`

**Specific Changes**:
1. `serial_excl.py`：统一打开函数。先构造未打开的 `serial.Serial`，设好 `dtr`/`rts` 再 `open()`，随后 `fcntl.ioctl(fd, termios.TIOCEXCL)` 独占；所有工具改用它。`reboot_soak.py` 仍按原逻辑用 RTS 复位。
2. `freeze_watch.py`：整个浸泡期间端口只打开一次，不再每窗重开（每次重开都会因 DTR 毛刺复位板子）；静默不退出，持续记录；事件分类 `PANIC`、`ROM_RESET(<rst 文本>)`、`HOST_DOWNLOAD_MODE`（`boot:0x3..(DOWNLOAD` 或 `waiting for download`）、`SILENT_STOP`（≥15s 无输出且无 ROM 行）、`AUTO_RECOVERED`（随后出现 `sysdiag:` 启动报告）；解析 `sysdiag:` 行与 `Saved PC`；日志按窗口追加保存（`/tmp/freeze_watch_<时间戳>.log`），事件写 `/tmp/freeze_watch_events.jsonl`，汇总累计有效观察时长与各类事件数；附 `--replay <log>` 离线分类模式（用于 E1 与宿主测试）。
3. `liveness_watch.py`：每 2s `ping -c1 -W1 <设备IP>`，写 `/tmp/liveness.jsonl`（时间戳、RTT/超时）；可选 `--mgmt` 通过现有 mTLS 客户端证书调用管理通道只读端点作为第二旁证（不新增未鉴权端点，3.3）。
4. 浸泡期间旁路采集 `journalctl -k -f` 中 CH343（`1a86:`）的 USB 事件到 `/tmp/usb_events.log`，用于 H0/H1d 判定。

**File**: 配置

**Specific Changes**:
1. 核心设计不需要改 `sdkconfig.p4c5`；新增 Kconfig 项（`ESP_CUBE_SYSDIAG`、阈值、`ESP_CUBE_SYSDIAG_ASYNC_LOG`、`ESP_CUBE_SYSDIAG_INJECT`）在 `components/sysdiag/Kconfig` 中给默认值（P4 默认 y，S31 默认 n），首次构建时自动写入本地 sdkconfig，无需人工编辑含凭据文件。
2. E4（PSRAM 80M）与 E5（副控制台）若执行，均为原地改一项、独立 build 目录、构建后改回并 diff 确认，不复制 `sdkconfig.p4c5`，不提交（3.20）。

#### B. 表情（EFC 路线）

**B1 资产流水线（PC 侧，不构建固件）**

- **输入与锁定**：美术为 `evidence/source/EmotePack/gif/*.gif`（23 个，848×848），该目录不入库。编码器为 `evidence/source/esp_efc_codec_1.1.0/pc/`（registry 归档 1.1.0，sha256 `d50c10fb…9316`，CHECKSUMS 已核对）。`firmware/main/idf_component.yml` 精确锁定 `1.1.0`。`tools/efc_build.py` 启动时核对 `efc_py.py` 与 `video_to_efc.py` 的 sha256，不一致则拒绝运行。
- **时间采样**：100 ms 网格，sample-and-hold（取 t=k·100ms 所在的 GIF 帧）。输出帧数 N = round(GIF 帧时长之和 / 100 ms)。23 段的 GIF 帧时长全部为 40 ms，等效 10 fps，与 3.10 的 100 ms 基准一致。
- **缩放**：848→460，正方形等比。先按 α 预乘 RGB，再对 RGB·α 与 α 分别做 LANCZOS，颜色 = 预乘结果 ÷ α。预乘避免 GIF 透明色（绿色 0,255,0）渗入边缘。
- **键控**：缩放后 α < 128 的像素记为键色，由官方 `apply_color_key` 写入 0xF81F。
- **编码**：`video_to_efc.encode(frames, out, key)`，即 tile 4、zlib-only。`encode_frames(compress=True, legacy=False)` 在 PRED 与 zlib 候选中取较小者，编码器对 23 段均选 zlib-only（PRED 候选更大，未采用）。编码器内部已解码比对（无损）。落盘后脚本再用 `efc_py.decode_frames` 读回，与 RGB565 参考逐元素比对。
- **构图**：不平移、不裁切。可见区中心偏移只记录，不作为失败条件（附属图标会拉偏包围盒，例如 `question` 为 +58 px@460）。
- **工具与位置**：`tools/efc_build.py`（新，替换 `tools/gif2efc.py`）与 `tools/efc_verify.py`（新，宿主契约检查与测试），两者平铺在 `tools/`，符合 docs/06 §0 的命名约定。`gif2efc.py` 依赖已不存在的 `managed_components` 路径，并做黑底合成与 CRF 下采样，不满足本流水线。删除前经用户确认（B5）。
- **输出**：`firmware/components/app_ui/ui/assets/face/<name>.efc`（23 个，键控）与 `MANIFEST.json`。MANIFEST 字段：`encoder{package, version, archive_sha256, efc_py_sha256, video_to_efc_sha256}`；`params{size:460, step_ms:100, tile:4, zlib:true, pred:false, resample:"LANCZOS-premultiplied", key:"0xF81F", key_threshold:128}`；`env{python, pillow, numpy}`；`clips[]{name, src_sha256, src_frames, src_total_ms, out_frames, delay_ms, efc_bytes, efc_sha256, crc32_per_frame, frame0_key_permille, frame0_lit_share_pct, key_share_permille（全帧平均，仅供信息）, changed_tile_pct, lit_share_pct（全帧平均，仅供信息）, centre_offset_px}`。
- **入库条件**：已满足：用户于 2026-11-15 裁决授权（个人 DIY，非商业用途），并决定 23 个 `.efc` 与 MANIFEST.json 提交进 git（D-GIT）。

**B2 设备集成（lv_efc）**

- **选型**：已确认采用 `lv_efc`（用户 2026-11-15）。解码器 API 保留为 R2b 的退路，不在本轮实现。
  1. 所需控制都已存在：`lv_efc_pause/resume`（splash 与面板）、`lv_efc_set_frame_delay`（100/200/70/50 ms）、`lv_efc_set_loop_enabled`、键控片段的 RGB565A8 发布、按变化外接框的局部失效（`invalidate_shown`）、1:1 显示不分配缩放缓冲。
  2. 改用解码器 API 需要自行实现播放定时器、帧循环、脏矩形与图像缓存失效（约 150–250 行，且需重新测试）。目前解码都在 LVGL 任务内执行，解码器 API 在 R2b（解码移出 LVGL 任务）触发前不带来收益。
  3. 代价（已接受）：每次加载复制一份 blob 到 PSRAM；加载时在调用者上下文同步解码第 0 帧（须持 LVGL 锁，实测纳入预算）；`set_src` 恢复定时器（须重挂）；D 通过定时器回调挂钩。
  4. 退路：R2b 触发时切换为解码器 API（独立解码任务 + 双缓冲，LVGL 只切换数据指针）。
- **对象与显示**：`ui_widget_create()` 中 `ui_w.face_anim = lv_efc_create(scr)`，`lv_obj_set_size(…, 460, 460)`，居中；`lv_image_set_inner_align(…, LV_IMAGE_ALIGN_CENTER)` 取代 `CONTAIN_DOWNSCALE`。对象尺寸必须等于片段尺寸（1:1）。
- **加载（内置）**：`lv_efc_set_src_data(obj, start, len)` → `lv_efc_set_frame_delay(obj, FACE_DELAY_BASE_MS)` → `lv_efc_set_loop_enabled(obj, true)` → `ui_face_set_running(obj)`。禁止紧随 `lv_efc_play()`（B4），禁止使用 `lv_efc_set_src(path)`（B7）。
- **键控与底色**：键控片段发布 RGB565A8，透明区透出其下的底色。`face_bg` 维持现状：加载成功后隐藏，失败时显示（3.12）。B10 已决（2026-11-15）：维持隐藏，改为可见未采用。
- **挂起与恢复**：状态为两个标志，`splash_active`（来自 `app_ui_splash_active()`）与 `panel_open`（面板展开）。统一入口 `ui_face_set_running(obj)`：`running = !splash_active && !panel_open`，`running ? lv_efc_resume : lv_efc_pause`。调用须在 LVGL 锁内。调用时机：`ui_pages_create()` 建好表情后以 splash=true 调用；`master_timer_cb()` 在 `el >= SPLASH_MS` 清除 splash 标志后调用；面板展开与收起处（现有 `lv_eaf_pause/resume` 所在位置）改为设置 `panel_open` 后调用；每次 `lv_efc_set_src_data` 之后调用（B8）。
- **帧间隔**：`FACE_DELAY_BASE_MS=100`（待机）、`FACE_DELAY_LISTEN_MS=100`、`FACE_DELAY_THINK_MS=200`、`FACE_DELAY_SPEAK_MS=70`，SD 热载 50 ms；经 `lv_efc_set_frame_delay()` 写入，常量仍集中在 `ui_internal.h`。
- **日志与自检**：加载后读 `lv_image_get_src(obj)` 的 `header.cf` 与 alpha 平面 0 值占比（加载后即第 0 帧，与 MANIFEST 的 `frame0_key_permille` 比对）。`ui_face_fmt.h` 的纯 C 计算与 RGB565A8 布局一致（颜色平面 w·h·2 字节，之后紧跟 alpha 平面，行跨度 w），算法可复用；按用户裁决视为 EAF 专用，接线与测试在任务 31.2 重新实现。
- **依赖与 Kconfig**：`firmware/main/idf_component.yml` 删除 `espressif/esp_lv_eaf_player: "^0.3.0"`，加入 `espressif/esp_efc_codec: "1.1.0"`，并以 `rules: - if: "target == esp32p4"` 限定只在 P4 引入（S31 表情关闭，不增加其固件体积；S31 构建须仍通过）。组件 Kconfig：`ESP_EFC_LVGL_WIDGET` 须为 y（由 P4 表情开关选中，实施时核对是 `select` 还是默认值）；`ESP_EFC_DECODE_4X4_ONLY` 保持默认 y（与 tile 4 一致）；`ESP_EFC_USE_SIMD` 保持默认；`ESP_EFC_PERF_LOG` 只在观测固件中开启。通过项目 Kconfig 默认值或 `sdkconfig.defaults` 设置，不手工编辑 `sdkconfig.p4c5`。
- **门控**：全部改动位于 `#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM` 内。sim 构建不链接 esp_efc_codec（3.21）。

**B3 加载、预检、热载与底色**

- **内置片段**：EMBED_FILES 只列测试内置的 4 段（smile_static、laugh、investigate、shocked）。候选池 23 段保留在 assets/face/，不编入固件。`face_mood_load(tag)`：mood 映射按采纳的 B6 映射执行（见 bugfix.md 3.9）；测试阶段未内置片段回退 smile_static（2.15、3.22）。
- **SD 热载**（`face/<name>.efc`，名称校验 `[a-z0-9_-]{1,15}`，3.13）：
  1. `face_load_task()` 等待 `app_ui_splash_active()` 为假（50 ms 轮询，最长 5 s），与 EAF 版相同；
  2. 在任务上下文把整个文件读入 PSRAM 缓冲（上限 262,144 字节），不持 LVGL 锁；
  3. 预检（不持锁，见下）；
  4. 持 `lvgl_port_lock`：`lv_efc_set_src_data(obj, buf, len)` → `lv_efc_set_frame_delay(obj, 50)` → `ui_face_set_running(obj)` → 输出 `face clip=sd:<name> …` → 解锁；
  5. 释放缓冲。文件缺失、读取失败或预检失败时保留当前表情并告警（3.13）。
- **预检**（`firmware/main/face_efc_precheck.h/.c`，纯 C）：通过 `face_efc_api_t` 注入解码器函数（open、get_info、frame0_key_permille、close）。设备胶水在 `ui_port_esp.c` 把它适配到 `esp_efc_dec_open/get_info/frame_with_alpha/close`；宿主测试注入桩函数。规则按顺序判定，首个失败给出原因码：
  1. `OPEN_FAILED`：`esp_efc_dec_open` 失败；
  2. `NOT_460`：`width != 460 || height != 460`；
  3. `SCALED`：`scaled` 为真，或 `coded_width/coded_height` 与显示尺寸不等；
  4. `TILE_NOT_4`：`tile_size != 4`；
  5. `FRAMES_OUT_OF_RANGE`：`frame_count` 不在 [1, 300]；
  6. `TOO_LARGE`：字节数 > 262,144；
  7. `MOSTLY_KEYED`：第 0 帧键占比 ≥ 990‰（用 `esp_efc_dec_frame_with_alpha` 解码到 PSRAM 暂存 634,800 字节，统计 alpha=0 像素）。
  
  解码器失败按失败处理；暂存缓冲在返回前释放；原因码为固定词表，不含文件内容。
- **缩放防护**：加载后断言 `lv_obj_get_width(obj) == 460 && lv_obj_get_height(obj) == 460`；不满足则告警，并回到内置默认片段。
- **底色圆**：不变，维持隐藏（B10 已决，2026-11-15）。

**B4 观测挂钩（阶段 A 只观测，任务 31.4）**

- `face_perf.c` 改为 lv_efc 挂钩，见 Hypothesized Root Cause 中的实测方案。`face_perf_stats` 与 `face_perf_geom.h` 保持不变。`face_perf_screen` 的判定由 C_BG 差异改为亮度阈值（lit）。
- 观测固件开启 `ESP_EFC_PERF_LOG`，作为 D 分项的交叉核对。
- CLI `faceperf start <秒> <片段>` 在 LVGL 锁内用 `lv_efc_set_src_data()` 切到指定内置片段，测完按原暂停态恢复，不追加 restart。`faceperf report` 与 `uimood <tag>` 的语义同 EAF 版。

**B5 退役与保留清单**

- **退役**（删除前列出清单并经用户确认，遵守删除类操作的确认规则）：
  - `tools/eaf_codec.py`、`tools/gif2efc.py`（由 `tools/efc_build.py` 替换）、`tools/gen_face_gifs.py`（合成演示动画，不是官方素材）。
  - `firmware/components/app_ui/ui/assets/face_{neutral,happy,wink}.eaf`。
  - `firmware/main/face_precheck.h`（EAF 预检接口）与 `firmware/main/test/test_face_precheck.c`。
  - `firmware/components/app_ui/ui/ui_face_fmt.h` 与 `firmware/main/test/test_ui_face_fmt.c`（按用户裁决视为 EAF 专用；任务 31.2 可复用其算法）。
  - `ui_widget.c` 与 `face_perf.c` 中的 `lv_eaf_*` 调用及挂钩（重写）；`idf_component.yml` 中的 `esp_lv_eaf_player`；`tools/host_tests.sh` 中 [18] 的旧内容（替换）。
- **保留（通用件）**：
  - `firmware/main/face_perf_stats.{c,h}`（任务 4.1，用户确认为通用件）。
  - `firmware/main/face_perf_geom.h`（用户确认为通用件）。
  - `firmware/main/face_perf_screen.{c,h}`：纯 C 采样函数。用户的通用件清单未列出本文件，本设计视为通用件保留，判定改为亮度阈值。暂不删除；其用途待梳理后由用户决定（2026-11-15）。

**B6 映射与分类（已由用户采纳，2026-11-15）**

- **现状（旧映射，已由 3.9 修改取代）**：`happy`/`relaxed` → `laugh`（happy 片段），其余 → `smile_static`（neutral 片段）。3.9 已按采纳的映射修改（见 bugfix.md，2026-11-15）。
- **提议表**（画面依据见 `evidence/source/efc_460/contact_sheet_848_over_bg.png`，变化 tile % 来自 `SIZES.md`）：

| 状态或情绪 | 提议片段 | 备选 | 变化 tile % | 类别 | 画面依据 |
|---|---|---|---|---|---|
| 待机 idle（即 neutral） | `smile_static` | — | 3.94 | 简单（待机规则） | 圆眼、静态微笑 |
| 聆听 listen | `investigate` | — | 4.53 | 复杂 | 眼球左右环视 |
| 思考 think | `ponder` | — | 3.86 | 复杂 | 眼球侧移、视线停在一侧 |
| 说话 speak | 无专用片段，沿用当前情绪片段，帧间隔 70 ms | — | — | — | 说话只调帧间隔（3.10） |
| happy | `laugh` | `yummy`、`confident`、`smile` | 5.81 | 复杂 | 闭眼弧线与张口 |
| relaxed | `leisure` | `music`、`shy` | 2.68 | 简单 | 平直闭眼，带小图标 |
| sad | `sad` | `cry` | 3.14 | 复杂 | 泪滴形眼 |
| angry | `angry` | — | 5.52 | 复杂 | 倒八字眉 |
| surprised | `shocked` | `panic` | 2.46 | 简单 | 圆眼与小 O 形嘴 |
| confused | `question` | `mock` | 4.67 | 复杂 | 问号图标 |
| sleepy | `yawn` | `asleep` | 5.04 | 复杂 | 张口打哈欠（`asleep` 为 22 s 长循环，不建议作默认） |
| 未映射 | `sigh`、`shy`、`snigger`、`badminton`（带球拍图标）、`mute`（带静音图标） | — | 见 SIZES.md | 见 SIZES.md | 备选或保留 |

- **类别规则（简单/复杂）**：简单 = `smile_static` ∪ {变化 tile % ≤ 3.0 的片段}，即 `shy` 2.16、`shocked` 2.46、`asleep` 2.63、`leisure` 2.68、`sigh` 2.74 与 `smile_static`，共 6 段。其余 17 段为复杂。阈值与分类写入 MANIFEST，任务 33 按类别验收。
- **采纳的默认值（2026-11-15 采纳即生效）**：`speak` 沿用当前情绪片段，帧间隔 70 ms；`sleepy` 映射为 `yawn`，`asleep` 不作默认；`mute` 与 `badminton` 不映射；简单/复杂分类采用变化 tile 占比 ≤3.0% 的规则。
- **未决项（已按默认采纳）**：
  - `speak` 是否需要专用片段：已按默认采纳。
  - `asleep` 是否用作 sleepy：已按默认采纳。
  - `mute` 与 `badminton` 是否保留为非情绪片段：已按默认采纳。

**B7 Flash 与 SD 放置（已由用户决定，2026-11-15）**

- 测试阶段内置 4 段：`smile_static`、`laugh`、`investigate`、`shocked`，合计 29,641 字节。它们覆盖待机、听、开心、惊讶，并包含简单与复杂两类。
- 23 段全部保留为候选池（任务 26、27、30.1 的产物），不是已采用的集合。
- 构建期按 23 段全部内置计算容量（206,846 字节加 MANIFEST），核对 factory 分区余量，以保证将来加入候选段时不需改架构。
- 未内置片段对应的情绪回退到 `smile_static`，并输出一条告警日志。这是默认行为，用户可改。
- SD 覆盖机制（`face/idle.efc` 与 `face/<name>.efc`）保留。
- **依据**：docs/09 原规划因 flash 余量约 0.8 MB 而把多段表情放到 SD。实测总量远小于该前提。若改用不透明版（1,149,908 字节），内置总量会超过该余量，需另行决定。
- docs/09 第 16 行与第 43 行需同步改写（任务 38.5）。

### 资源预算与核对方法

| 项 | 位置 | 大小 | 核对 |
|---|---|---|---|
| 现场记录 | LP SRAM（`RTC_NOINIT_ATTR`） | ≤512B（预计约 300B） | `idf.py size-components` 中 rtc_noinit；RTCRAM 堆区相应缩小，单列报告 |
| 日志槽环 | PSRAM 堆 | 16KB | `heapstat`/`baseline` |
| logtx 任务栈与 TCB | PSRAM（`xTaskCreatePinnedToCoreWithCaps`） | 约 3.5KB | 若 TCB 被 IDF 强制放内部则 ≤0.5KB，计入基线对比 |
| 监督器 | 复用 esp_timer 任务 | 0 栈 | — |
| faceperf 样本环 | PSRAM 堆，测量时分配 | 16KB | 测完释放 |
| 内部静态增量 | `.bss/.data` | ≤128B（控制变量） | `idf.py size` 前后对比 DRAM `.bss` |
| 内置表情素材（测试 4 段，新） | flash rodata | 29,641 字节 | 构建后 `idf.py size` 与 MANIFEST 对照；docs/09 余量口径复核 |
| 候选池（23 段键控，新） | 构建期预算，不占用内置 | 206,846 字节（最大 44,565 字节） | 构建期按 23 段计算容量，核对 factory 分区余量（B7） |
| 播放器对象（新） | PSRAM（lv_efc 复制 blob + 帧缓冲） | 每个活动片段 634,800 字节（键控帧缓冲：423,200 颜色 + 211,600 alpha）+ blob（内置 ≤44,565 字节；SD ≤262,144 字节） | 加载前后 `heap_caps` 统计 PSRAM 增量 |
| SD 热载峰值 | PSRAM | 锁外：文件缓冲 F + 预检暂存 634,800；锁内：文件缓冲 F + 新 blob F + 新帧 634,800（旧片段先释放），F ≤ 262,144 | 峰值 ≤ 约 1.2 MB，任务 34 实测 |
| 解码器内部分配（预编译库） | 未知（内部 RAM 或 PSRAM） | 待测 | 加载前后 `MALLOC_CAP_INTERNAL` 最大块与空闲对照（3.16）；任务 31.5 与 33 |

### 实施顺序

1. **表情线 E0（PC 资产与宿主测试，不烧录）**：任务 26 → 27 → 30.1–30.3 生成素材与 MANIFEST，并通过宿主套件 [18]；30.4 的退役经用户确认后执行。
2. **表情线 E1（板上基线，F0）**：任务 28、29 在当前 EAF 固件上执行，可与 E0 并行，不依赖新代码。
3. **表情线 E2（实现与观测）**：任务 31.1–31.6，构建三目标，烧录 F'。
4. **表情线 E3（板上验收）**：任务 32–35。
5. **表情线 E4（条件）**：任务 36，仅在 33 判定 R2–R4 时执行。
6. **表情线 E5（眼见确认与文档）**：任务 37、38。
7. **冻结线**：先 1.5（JTAG 连通）；13.3–13.6（监测与板上检查）；22.1 无调试器长时浸泡（在最终面板固件 F' 上，见 tasks.md）。仅当 22.1 复现冻结，才执行条件任务 14–21 与 22.2–22.3。

## Testing Strategy

### Validation Approach

测试分两步：先在未修复固件上制造并观察反例，确认或推翻根因假设；再在修复固件上验证 Bug Condition 输入得到正确行为（Fix Checking），非 Bug Condition 输入行为不变（Preservation Checking）。

- 冻结线的 F 指阶段 A 观测固件。它只增加记录、计时、自检日志与 CLI 命令，复位行为与 UI 行为同原固件，因此新增 CLI 命令可在 F 与 F' 上同样执行。
- 表情线的 F0 指当前仓库固件（EAF），不新增观测代码。启动日志中的 `splash:`、`face first refresh`、`face screen check` 以及 `cf=`/`alpha0=` 行已由现有 face_perf 启动挂接与 UI 日志输出。

宿主可测部分全部接入 `tools/host_tests.sh`；板上部分给出命令、预期日志与证据文件。

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bug BEFORE implementing the fix. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: 阶段 A 固件（冻结线）只增加记录、计时与自检日志，不改业务和复位行为；表情线在 F0 上只读取现有日志并在宿主侧运行契约检查。每项都写明预期反例，结果不符时回到 Hypothesized Root Cause 重新假设。

**Test Cases**:
1. **X1 素材契约（宿主，任务 26）**：`python3 tools/efc_verify.py --contract firmware/components/app_ui/ui/assets/face`。预期失败：三份当前素材不是 EFC（magic 不是 `EFC1`），键占比为 0，可见占比为 0%（will fail on unfixed code）。历史对照：`evidence/2/` 中的 EAF lint 反例。
2. **X2 当前屏幕与格式（板上 F0，任务 28）**：RST 启动。预期 `cf=RGB565A8 alpha0=100%`、`face screen check: lit≈0%`，确认 B1（will fail on unfixed code）。
3. **X3 splash 争用（板上 F0，任务 28）**：同一启动的 `splash:` 摘要。预期 I_refr P95 >50ms，或出现 >100ms 停顿，或 d>0（will fail on unfixed code）。
4. **X4 PC 基线（已完成，`evidence/source/efc_460/`）**：23 段带键 EFC 的尺寸、无损结果、键占比与变化 tile % 已记录在 SIZES.md。它们是任务 32、33 的期望值来源，不是板上测试。
5. **X5（已取消）**：EAF 的 `faceperf` 基线不再采集。EAF 路线退役，板上只保留 X2、X3 的启动日志证据。
6. **X6 IWDT 第二级（板上，E2）**：`inject irqoff both CONFIRM` 预期 ≤1.5s 复位，ROM 行为 MWDT 类，阶段 A 启动报告给出原始码（may fail：若不复位则推翻疑点 (b) 的结论，H2 重新上升）。
7. **X7 TWDT 盲区（板上）**：`inject task ui CONFIRM`（阶段 A 监督器只记录）。预期 30s 内无任何复位，`diag` 显示 UI 心跳年龄持续增长而 idle 正常；随后执行 `inject irqoff both CONFIRM` 让 MWDT 复位（保留区在 HP 复位下存活），下次启动记录指向 UI（will fail on unfixed code：没有自动恢复，只能人工复位）。
8. **X8 保留区存活性（板上，E0）**：写记录后分别做 RTS 脉冲、软件重启、X6 的 MWDT 复位，检查下次启动记录是否有效；请用户配合一次拔插 USB 冷上电，预期判为无效（edge case）。

**Expected Counterexamples**:
- 素材全透明，屏幕 lit 占比约 0%；splash 期间 LVGL 任务被逐帧解码占用（X2、X3）。
- 业务任务停滞而 idle 正常时没有任何自动恢复；窗 3 事件实为 ROM 下载模式。
- Possible causes: 调色板 alpha 写 0（B1）、播放定时器未随 splash 暂停（B2）、TWDT 只订阅 idle（疑点 a）、主机端口争用或 USB 重枚举（H0）。

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed function produces the expected behavior.

**Pseudocode:**
```
FOR ALL X WHERE isBugCondition_freeze(X) AND X.host_event = NONE DO
  result := run_board_F'(X)                       // 注入或浸泡中自然发生
  ASSERT expectedBehavior_freeze(X, result)
END FOR

FOR ALL X WHERE X.host_event ≠ NONE DO
  ASSERT tools_classify(X.serial_log) = HOST_INDUCED AND port_open_by_others = EBUSY
END FOR

FOR ALL Y WHERE isBugCondition_face(Y) DO
  result := run_board_F'(Y)
  ASSERT expectedBehavior_face(result)
END FOR

FUNCTION expectedBehavior_freeze(X, r)
  RETURN r.auto_reset_after_fault_s <= 30
     AND r.next_boot.reset_class = expected_class(X.fault)        // 注入矩阵对照表
     AND r.next_boot.record_valid
     AND r.next_boot.record.points_to = X.fault_location           // 核心或任务
     AND r.next_boot.reaches(got_ip, sntp, mixer_health)
     AND r.log_producers_blocked_ms_max <= 1                       // 故障期间
END FUNCTION

FUNCTION expectedBehavior_face(r)
  RETURN r.splash.I_refr_p95_ms <= 50 AND r.splash.I_refr_max_ms <= 100
     AND r.splash.d = 0
     AND r.main.first_face_refresh_after_splash_ms <= 100
     AND r.main.cf = RGB565A8                                      // 键控片段
     AND r.main.alpha0_pct = manifest.frame0_key_permille / 10 ± 0.5
     AND r.main.screen_lit_pct within manifest.frame0_lit_share_pct ± 5
     AND r.playback.I_loop_p95_ms <= 50 AND r.playback.I_loop_max_ms <= 100
     AND r.T_p95_ms <= (clip IN SIMPLE_CLASS ? 33 : 66)
     AND r.switch.I_loop_max_ms <= 100
END FUNCTION
```

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed function produces the same result as the original function.

**Pseudocode:**
```
FOR ALL X WHERE NOT isBugCondition_freeze(X) DO
  ASSERT observable(F(X)) = observable(F'(X))     // 无新增复位, 原 WDT 路径与输出格式不变
END FOR

FOR ALL Y WHERE NOT isBugCondition_face(Y) DO
  ASSERT ui_semantics(F0(Y)) = ui_semantics(F'(Y)) // 映射/帧间隔/面板/回退不变
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many test cases automatically across the input domain
- It catches edge cases that manual unit tests might miss
- It provides strong guarantees that behavior is unchanged for all non-buggy inputs

**Test Plan**: Observe behavior on UNFIXED code first for 正常启动、联网、CLI、WDT 可捕获故障、表情映射与面板，then write property-based tests capturing that behavior（宿主侧针对监督策略、日志环、记录编解码、素材流水线；板上针对启动与业务回归）。

**Test Cases**:
1. **启动与联网保持**：在 F 上记录 `reboot_soak.py 30` 结果（got_ip、无 abort/assert/guru/hang），F' 上重跑 ≥30 次，结果一致（3.3、3.4）。
2. **WDT 原路径保持**：F 与 F' 上各执行 `inject spin 1 CONFIRM` 与 `inject irqoff 0 CONFIRM`，均出现 TWDT/IWDT panic、双核回溯与自动重启，输出格式一致；F' 额外打印 sysdiag 启动报告（3.5）。
3. **合法长操作无误复位**：F' 上依次执行 SRP 全链配对（`tools/pair_client.py`）、`wget` 1MB 到 SD、`memtest` 连续写 50 条、`sdretry`、mood 切换 100 次、`face` 热载 20 次、断开 AP 让 WiFi 重连一次，全程 `diag` 无触发、无复位（3.6）。
4. **写入期间复位的数据完整性**：`memtest` 循环写入时注入 `inject irqoff both nowdt CONFIRM`（LP WDT 复位），重复 10 次，每次启动后 `memstat`/记录读回正常，宿主套件 [4/14] 双槽协议保持全绿（3.7）。
5. **表情语义保持**：F0 与 F' 上用 `uimood happy`/`relaxed`/`sad` 与会话状态切换观察片段选择和帧间隔（`face clip=` 行的 delay 字段 100/200/70ms，SD 热载 50ms）；`panel 1`/`panel 0` 期间 D 样本暂停/恢复；删除 SD 上的 `face/idle.efc`，以及放入键占比 ≥99% 的测试素材时，保留当前表情并告警（3.9–3.13）。F0 上没有 `uimood` 时以 `face` 热载代替，并在 `evidence/29/` 注明。
6. **资源与工具保持**：修复前后 `baseline 20`、`heapstat`、`cputop`、`idf.py size` 对比（3.16、3.17）；sim 构建的链接清单不含 esp_efc_codec（3.21）。

### Unit Tests

- `sysdiag_core`：复位分类表逐行用例（每个原始码 × 提示 × 记录状态 × 注入标记）；记录头/封存区 CRC 正反例；ms → RC_SLOW tick 换算（含 efuse `WDT_DELAY_SEL` 移位语义的上下界）。百分位函数不属于 `sysdiag_core`，其单测在 `test_face_perf_stats.c`（任务 4.1，[18]）。
- 监督策略：启动宽限期内不触发；busy 截止前不触发、截止后按阈值触发；CLI/NET 只记录不触发；LOGTX 先直写探针再触发。
- `tools/efc_build.py`：采样计划（N、索引单调、帧起点对齐）、预乘缩放后的键阈值（α<128）、MANIFEST 字段完整性、编码器目录 sha256 不一致时拒绝运行。
- `tools/efc_verify.py`：头部解析（magic、tile、标志位、尺寸、帧数）、逐帧 CRC32、键占比与 lit 占比计算、与 MANIFEST 比对。
- `face_efc_precheck`：7 条规则的正反例（桩解码器注入）。
- `face_run_state`（Property 10）：running 计算与加载后的重挂顺序（纯 C 状态机，加上 lv_efc 副作用模型）。
- `face_perf_stats`（任务 4.1，保留）；`face_perf_screen` 的亮度谓词。
- `freeze_watch` 分类器：真实片段用例（窗 3 日志尾部、panic 回溯、`rst:0x7`/MWDT 行、`sysdiag:` 启动报告、15s 静默）。

### Property-Based Tests

宿主 PBT 不引入新依赖：C 侧用固定种子 xorshift 生成器 + ASan/UBSan（与现有套件一致），Python 侧用标准库 `random` 固定种子循环（每个属性 ≥1000 例；若环境已装 Hypothesis 则同一断言改用其生成器）。新增套件接入 `tools/host_tests.sh` 的 [15]–[18]，原 14 套件编号与内容不动：

- **[15] test_sysdiag_record**（Property 5）：随机生成记录 → 随机位翻转/截断/全随机字节 → `validate()` 为无效或字段逐字节相等；随机 (原始码 0–0x3F, 提示, 状态, 注入) → 分类确定、唯一、输出字符串属于固定词表。
- **[16] test_sysdiag_supervisor**（Property 6）：随机心跳轨迹（抖动 ≤阈值、busy 窗口、≤0.5s 停顿）→ 每周期都喂狗；随机 t₀ 处切断某触发对象 → 触发时刻 ∈ [t₀+阈值, t₀+阈值+1s] 且总上界 ≤30s；切断非触发对象（CLI/NET）→ 不触发。
- **[17] test_sysdiag_logring**（Property 7）：模拟多生产者（随机交错、随机长度含超长行）与随机停顿/永久阻塞的消费者 → 生产调用步数有上界、输出为生产序列的有序子序列、丢弃计数精确、无撕裂行。
- **[18] 表情素材管线（EFC，替换 EAF 版内容）**（Property 8、9、10）：
  - `python3 tools/efc_verify.py --contract`：对 `ui/assets/face/*.efc` 检查 magic `EFC1`、tile 4、未缩放、460×460、帧数与 MANIFEST 一致、字节数上限、文件 sha256 与 MANIFEST 一致、来源归档 sha256 与编码器文件 sha256 一致、第 0 帧键占比 <99%、第 0 帧 lit 占比与 MANIFEST 的 `frame0_lit_share_pct` 一致。
  - 无损往返：用 `efc_py.decode_frames` 解码每个 `.efc`，逐帧 CRC32 与 MANIFEST 的 `crc32_per_frame` 比对。CRC32 为 IEEE 802.3，输入是该帧颜色平面的 w·h 个小端 u16（即 `esp_efc_dec_frame` 的输出）。
  - PBT（固定种子 ≥1000 例）：随机小尺寸 RGB565 帧（宽高为 4 的倍数）加随机键掩码 → 官方编码器 → 解码逐元素相等，键像素 alpha=0（Property 8）；随机 GIF 时间轴 → 采样计划（N = round(总时长/100ms)，索引单调，帧起点 ≤ t < 帧终点）。
  - `face_efc_precheck`（C，gcc + ASan/UBSan）：7 条规则的桩解码器用例，随机 info 组合 ≥1000 例，判定与 Property 9 的 iff 规则一致。
  - `face_run_state`（C，ASan/UBSan）：随机事件序列 ≥1000 例，running 与解码计数满足 Property 10。
  - `test_face_perf_stats`（任务 4.1，保留）。
  - **替代说明**：宿主无法编译设备解码器，因为发布包只提供目标相关的预编译库（见 Bug Details 取证表）。因此，宿主侧以 PC 参考解码器 `efc_py.decode_frames` 验证编码器输出的无损性；设备侧以 `faceefc selftest <名>` 命令（任务 31.4）对照同一组 CRC32 验证设备解码器的输出。
- **[附] freeze_watch selftest**（Property 1 的工具部分）：在随机正常日志流中插入 ROM 下载模式、ROM 复位、panic、静默片段 → 分类正确，ROM 行与下载模式永不被判为 `SILENT_STOP`。

### Integration Tests

板上验证统一纪律：构建与烧录各自检查退出码；P4 一律 `idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 build`，全量烧录 `idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 -p /dev/ttyACM0 -b 230400 flash`（不用 diff-with）；烧录后以启动日志 `app_init: App version / Compile time / ELF file SHA256` 与 `sysdiag: fw=… elf=…` 行确认板上版本，不信 esptool 的 Done；任何时刻 `/dev/ttyACM0` 只允许一个进程打开（`serial_excl.py` 独占）；打开串口的 DTR 毛刺会复位板子，浸泡期间端口常开。

| 需求 | 步骤 | 证据形式 |
|---|---|---|
| 2.1/2.2/2.6 注入矩阵 | 对注入表每一项各执行 ≥3 次：`p4_cli.py "inject <kind> CONFIRM"`，`freeze_watch.py` 常驻记录 | `/tmp/freeze_watch_events.jsonl` 中每次注入到 ROM 行的间隔（≤30s，按类别核对 1.5s/12s/26s 等上界）；启动报告类别与 `inject=<kind>`；记录指向的核心/任务；随后 got_ip、SNTP、mixer health 出现 |
| 2.3 启动报告 | 正常 RTS 复位、软件重启、每种注入、冷上电各一次 | 每次启动的 `sysdiag:` 行原文；冷上电与人为破坏记录（`diag corrupt` 调试子命令）时为"无有效现场记录" |
| 2.4 旁证与非阻塞日志 | `inject logblock CONFIRM` 期间运行 `liveness_watch.py` 并观察屏幕表情是否持续推进；监督器在 15s 时经 ROM 直写输出探针行 | ping 全程通；屏幕持续推进；下次启动记录中 UI/mixer 心跳持续到复位前而 logtx 停滞、`drops` 增长；≤26s 复位；主机可见 `[sysdiag] console stall` 直写行 |
| 2.5 浸泡 | 修复固件，`freeze_watch.py`（独占、常开）+ `liveness_watch.py` + USB 事件采集，累计 ≥4h，其中 ≥2h 完全空闲（开机电台关、无会话）；其余可含电台播放 | 汇总 JSON：有效观察时长、各类事件数；人工复位次数 = 0；每次 `AUTO_RECOVERED` 附启动报告；`HOST_DOWNLOAD_MODE` 事件单列并作废所在窗口（不计入时长） |
| 2.7 splash | `reboot_soak.py 10 45 --face`（RTS 复位，抓取 `splash:`、`face first refresh`、`face screen check`、`cf=` 行） | 10 份 splash 摘要：n、P95、max、>100ms 次数、d；用户眼见确认至少 1 次无明显卡顿 |
| 2.8 splash 后表情 | 同上 10 次 + 用户在场 RST/上电若干次；播放期间 `panel 1`/`panel 0` 与手势滑动 | 每次 `face first refresh +Xms`、`lit` 与 MANIFEST 的 `frame0_lit_share_pct` 差 ≤5 个百分点、`cf=RGB565A8`；faceperf 中 I_loop P95/max；用户确认面板滑出与手势响应 |
| 2.9 观感 | 用户眼见复验（附照片或文字描述），对比 `evidence/source/efc_460/contact_sheet_848_over_bg.png` 与板上照片 | docs/19 §47 #3 记录用户结论 |
| 2.10/2.11 帧耗时 | `faceperf start 40 <片段>` 对 23 段逐段执行，空闲与电台播放两场景；前后 `cputop` | 报告原文：片段、类别、场景、n（≥300）、D/R/F/T 的 P50/P95/max、超阈计数；cputop 中 LVGL 任务 CPU% |
| 2.12 切换 | `faceperf start 60` 期间 `uimood happy`/`uimood neutral` 交替 50 次；SD `face/idle.efc` 热载与 RST 后自动热载 | I_loop max ≤100ms；每次切换后 lit 正常；热载日志 |
| 2.14 预检 | 板上放入键占比 ≥99% 的测试素材执行 `face <名>`；放入超尺寸与截断文件各一次 | 预检原因码、告警行与当前表情保持（截图） |
| 3.1/3.2 | `tools/host_tests.sh`；S31：`idf.py --preview -B build-s31 build`；P4C5 同上；sim 按 `sim/README.md`；`python3 tools/check_layers.py` | 各命令退出码与末行输出 |
| 3.3/3.4 | `pair_client.py` 配对正反例、`mgmt_client.py` 无证书 401；`reboot_soak.py 30` | 报告 JSON 与 HTTP 状态码 |
| 3.5 | `inject spin 1 CONFIRM`、`inject irqoff 0 CONFIRM` | panic 原文（含 `Task watchdog`/`Interrupt wdt timeout on CPU0` 与双核回溯） |
| 3.6/3.7 | Preservation 用例 3、4 | `diag` 截取与 `memstat` 读回 |
| 3.13 | SD 放置合法的 `face/idle.efc`，再分别放置缺失与损坏两种情况 | 替换、保留与告警日志 |
| 3.14 | 自动复位后检查 `player` 状态、音量与安静时段行为 | 启动日志 `vol=`、`uimodel`/`bb` 输出 |
| 3.15 | 宿主 grep 新增日志格式串与记录字段定义；板上抽查启动报告、faceperf、diag 输出 | 审计结论写入 docs/19 |
| 3.16 | 修复前后 `baseline 20`、`heapstat`、`idf.py size`；CLI 任务创建成功（启动出现 `=== EspCube CLI`） | 对比表（HP 内部堆、最大块、RTCRAM 区单列） |
| 3.17/3.18 | `cputop`、`baseline 5`、`p4_cli.py help`；启动版本行 | 输出原文 |
| 3.19 | 源码检查不使用 GPIO24/25、无 eFuse 写 JTAG 禁用；调试器附着时 LP WDT 不武装（以 `esp_cpu_dbgr_is_attached()` 分支的宿主单测与代码审读代替实测）。OpenOCD 实测：方案 A 已接线（2026-11-16），由任务 1.5 执行。 | 审读记录 |
| 3.20 | `git check-ignore firmware/sdkconfig.p4c5`；提交前 diff 中无 `sdkconfig.p4c5` 与凭据串 | 命令输出 |
| 3.21 | sim 构建后检查链接清单与目标文件，确认不含 esp_efc_codec 与 `lv_efc` 符号 | 构建输出与 map 摘要 |

### 需要用户确认或配合的事项

- 2.3 要求区分"上电"与"人工 RST"：两者 ROM 复位码相同（`rst:0x1 (POWERON)`），只能依赖 LP SRAM 在 EN 复位后是否存活来区分。若 E0 显示不存活，启动报告只能标注"上电或 EN 复位"，需用户接受该硬件限制。
- 2.8/2.12 的"UI 刷新间隔"在表情播放阶段按 I_loop 口径验收：表情帧间隔保持 100/200/70ms（3.10），播放期间的渲染间隔本身就等于帧间隔，按渲染间隔计 P95 必然 >50ms。
- 2.5 浸泡中主机诱发的下载模式事件（ROM `boot:0x307`）不属于固件可恢复范围（ROM 下载模式下应用代码不运行），按"主机诱发"单列并作废所在窗口，靠串口独占与 USB 事件采集预防。
- E0 冷上电一步需要用户拔插一次 USB；2.9 观感需用户眼见复验（任务 37）。

EFC 路线新增的待用户确认事项（与 bugfix.md 的修改相关）：

- **授权与入库（许可已裁决）**：esp_efc_codec 为 Espressif Modified MIT（限乐鑫产品），Emote Pack 无授权文件。请用户确认：(a) 已决（D-GIT，2026-11-15）：23 个 `.efc` 与 MANIFEST.json 提交进 git；(b) 原始 GIF 是否入库（建议不入库，留在 `evidence/`）；(c) 仓库对外公开时的再分发边界。用户裁决（2026-11-15）：个人 DIY 项目，非商业用途；按此口径使用 esp_efc_codec 与 Emote Pack 素材。仓库若对外公开，需再复核。
- B2 已决（lv_efc）；B6 已采纳（2026-11-15，默认项见 B6 段）。
- **键控（默认）与不透明（对照）**：不透明版体积为 5.56 倍，超出 docs/09 的 flash 余量。
- **底色圆（B10）**：已决（2026-11-15）：加载成功后维持隐藏（现状）。改为可见会让呼吸效果可见，但每步整圆重绘会增加 R 成本，并使 2.9 的呈现与现状不同，未采用。
- **10 fps 节奏**：源 GIF 为 25 fps，按 3.10 的 100 ms 基准采样为 10 fps。若希望 25 fps，需修改 3.10 的帧间隔基准，属于需求变更。
- **屏幕自检口径**：已决（D-LIT，2026-11-15），由 C_BG 差异改为亮度 lit。`face_perf_screen.c` 的通用件归属（B5）待 30.5 决定。
- **docs/09 SD 规划与 flash 余量**：按已决的 B7 改写（任务 38.5），并以实际镜像复核余量。
- **宿主解码测试的替代方案**：宿主无法编译设备解码器，以 PC 参考解码器加设备 CRC 自检替代（见 [18]）。
- **退役文件的删除**：`tools/eaf_codec.py`、`tools/gif2efc.py`、`tools/gen_face_gifs.py`、三份 `.eaf` 与 EAF 专用测试（B5）。清单已由用户确认（2026-11-15，face_perf_screen 除外，见 30.5）。

## 附录 A：EAF 路线历史（已退役，结论与证据索引）

本附录保留 EAF 路线的结论、证据位置与退役原因。规格目录未纳入 git，EAF 时期的设计段落已由本文件的 EFC 设计替换，此处保留其结论，不保留逐字原文。

- **时间线（docs/19）**：批次 38（§42，2026-11-14）换装 `esp_lv_eaf_player`，嵌入 9 个官方情绪片段，合计 1.09 MB，mood 与 soul 词典 1:1 对应。批次 40–41（§43–§44）尝试 image 级缩放，黑屏，回退为 1:1。批次 42（§45）上板 460 重编码素材（`face src 460x460`、`clip ok: 46 frames @100ms`）。批次 43（§46）出现 splash 卡顿与动画后黑屏，系统存活。§47 #2 立为开放问题。
- **取证结论**：(1) 调色板 alpha 全 0（task 2 的 lint：11520/11520、5632/5632、3840/3840），RGB565A8 整帧透明，屏幕只剩底色（黑屏根因）。(2) 125×160 源经 `upscale()` 非等比放大，x×3.68、y×2.875。(3) EAF 解码开销约为 125×160 的 10.6 倍（docs/19 §46），splash 期间定时器仍在 LVGL 任务内解码（卡顿）。
- **EAF 产物（已退役）**：`tools/eaf_codec.py`（decode、upscale、encode、lint、selftest 组）；`firmware/components/app_ui/ui/assets/face_{neutral,happy,wink}.eaf`；`firmware/main/face_precheck.h` 与其测试；`firmware/main/face_perf.c` 的 lv_eaf 挂钩；`ui_widget.c` 的 cf/alpha0 日志接线；`ui_face_fmt.h` 与 `test_ui_face_fmt.c`；`tools/host_tests.sh` 中 [18] 的 EAF 版内容。证据：`evidence/2/`（lint 与 selftest 反例）、`evidence/3/`（EAF 解码摘要基线）。
- **EAF 路线的替代方案（记录，均未执行）**：230×230 EAF 重编码（docs/19 §46 备选，约 2.6 倍开销）；回退 125 素材（§46 备选）；image 级缩放（§44，失败）。
- **任务状态**：原任务 2、3、4.2、4.3 已完成；4.4 中止；4.5 及之后的 EAF 版任务未执行。全部记录在 tasks.md 的附录中。
- **PBT 状态**：`tasks.meta.json` 的 `pbtResults` 中，EAF 版任务 2 记录为 `passed`（探索测试在未修复代码上按预期失败，失败示例见该文件）。该记录保留为历史，不对应新的任务文本。
