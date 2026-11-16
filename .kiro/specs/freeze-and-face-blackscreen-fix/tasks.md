# Implementation Plan

## Overview

本计划按 bug condition 方法拆解 bugfix.md 的两个缺陷：表情线（#2，含 #3 观感与 #4 帧率）（2026-11-15 起改用 esp_efc_codec，见设计文档执行记录）在任务 26–38，冻结线（#1）在任务 11–23，任务 1 与 4.1 为公共准备，任务 39 为总检查点。每条线都先写在未修复代码上暴露缺陷的探索测试，再写保持性测试，然后修复，最后做 Fix Checking 与 Preservation Checking。需要用户亲手参与的步骤单列为任务 37、15、23。

执行约定（所有任务适用）：

- **两条线按设计顺序推进**：表情线（任务 26–38）与冻结线（任务 11–23）按设计的实施顺序推进，两条线在任务 22 汇合；同一条线内必须按编号顺序执行（先探索、再保持、再修复、最后 Fix/Preservation Checking）。
- **构建纪律**：每条构建或烧录命令单独检查退出码，不与后续命令用 `&&` 混判。命令均在 `firmware/` 目录执行：
  - S31：`idf.py --preview -B build-s31 build`
  - P4C5：`idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 build`
  - sim：`sim/run.sh --demo`（无屏构建并演示，退出码为 0）
- **烧录纪律**：全量烧录 `idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 -p /dev/ttyACM0 -b 230400 flash`，不用 diff-with，烧录前删除 `build-p4c5` 下陈旧的 `*_flashed.bin`。烧录后以启动日志确认版本，不信 esptool 的 Done：`app_init: ELF file SHA256` 前缀须与 `sha256sum firmware/build-p4c5/esp_cube.elf` 前缀一致；sysdiag 落地后再核对 `sysdiag: fw=… elf=…` 行。
- **串口纪律**：任何时刻只允许一个进程打开 `/dev/ttyACM0`。任务 13 的 `serial_excl.py` 落地前，每次打开前用 `fuser -v /dev/ttyACM0`（或 `lsof`）人工确认无占用。打开端口时的 DTR 毛刺会复位板子，属已知行为。
- **凭据**：不复制、不打印、不提交 `firmware/sdkconfig.p4c5`。必须改配置项时原地改一行，改前改后记录该文件 sha256；核对改动只 diff 构建产物 `build-*/config/sdkconfig.h` 中的目标项。新增 Kconfig 项首次构建时被写入本地 sdkconfig，属预期。
- **内存**：新增诊断缓冲走 PSRAM 堆（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`、`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`）或 `RTC_NOINIT_ATTR` 保留区，不新增内部 RAM 静态缓冲；每次新增后用 `idf.py size` 对比 DRAM `.bss`（累计增量 ≤128B）。
- **证据**：原始日志、报告 JSON、cputop 原文存 `.kiro/specs/freeze-and-face-blackscreen-fix/evidence/<任务号>/`（下称 `evidence/`，目录内 `.gitignore` 排除全部内容，不提交）；docs/19 只引用关键行与汇总数字。
- **宿主测试**：新套件 [15]–[18] 与 `[附] freeze_watch selftest` 追加到 `tools/host_tests.sh`，原 [1]–[14] 的编号与内容不改。C 侧 PBT 用固定种子 xorshift + ASan/UBSan；Python 侧用 `random.Random(固定种子)`，每个属性 ≥1000 例（环境装有 Hypothesis 时同一断言改用其生成器）。
- **F 的含义**：板上对照中的 F 指"阶段 A 观测固件"（任务 17 产出）。它只增加记录、计时、自检日志与 CLI 命令，复位行为与 UI 行为同原固件。表情线的 F0 指当前 EAF 固件，F' 指 EFC 固件。

## Tasks

### 第 0 部分：公共准备

- [x] 1. 证据保全与修复前基线
  - [x] 1.1 保全观察窗 3 现场并建立证据目录
    - 新建 `evidence/` 及其 `.gitignore`（内容为 `*` 与 `!.gitignore` 两行）
    - 在运行任何旧版 `freeze_watch.py` 之前，把 `/tmp/freeze_watch.log`、`/tmp/freeze_watch_report.json` 复制到 `evidence/1.1/` 并记录 sha256（旧工具每窗结束都会覆盖这两个文件）
    - 截取最后一条应用日志 `I (1389006) LV_EFC: perf …` 之前约 40 行到文件末尾（含 `rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))`、3 次 `waiting for download` 与 SLIP 字节），扫描确认无密码、密钥、令牌后存为入库夹具 `tools/testdata/freeze_win3_tail.log`
    - 若 `/tmp/freeze_watch.log` 已丢失：在 `evidence/1.1/README` 注明，夹具改用 design.md 取证表引用的原文行构造，并在任务 38.2 的 docs/19 记录中说明该降级
    - _Requirements: 1.1, 2.5_
  - [x] 1.2 宿主与构建基线
    - `tools/host_tests.sh` 14 套件 PASS、`python3 tools/check_layers.py` 全绿，存末行输出
    - 按执行约定构建 S31、P4C5、sim，三条命令退出码均为 0
    - `idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 size` 与 `size-components` 输出存 `evidence/1.2/`（DRAM `.bss/.data`、`rtc_noinit`、RTCRAM 区），作为 3.16 的对比基线
    - _Requirements: 3.1, 3.2, 3.16_
  - [x] 1.3 板上基线（F0 = 当前固件）
    - 按执行约定全量烧录并确认版本
    - 存证完整启动日志（`got ip`、SNTP、`mixer: health`、`vol=`、电台关、`face src 460x460`、`efc clip ok: 46 frames @100ms`），以及 `baseline 20`、`heapstat`、`cputop`、`python3 tools/p4_cli.py help` 的输出原文
    - _Requirements: 3.3, 3.14, 3.16, 3.17, 3.18_
  - [x] 1.4 JTAG 可用性与凭据隔离基线
    - `git check-ignore -v firmware/sdkconfig.p4c5` 有输出；源码检索确认固件无 GPIO24/25 引用
    - `idf.py -B build-p4c5 -DSDKCONFIG=$PWD/sdkconfig.p4c5 -p /dev/ttyACM0 efuse-summary` 只读确认 JTAG 禁用类 eFuse 未烧写（该命令会经 DTR/RTS 复位板子，属预期），输出存 `evidence/1.4/`
    - _Requirements: 3.19, 3.20_
  - [x] 1.5 JTAG 连通（方案 A：USB-JTAG，J2 第 33/34 脚；不改 eFuse，不烧录）
    - 前提：用户已按接线指导接好：白线 D− 接 J2 第 33 脚（GPIO24），绿线 D+ 接 J2 第 34 脚（GPIO25），黑线接 GND，红线不接。`lsusb` 应出现 `303a:1001`。
    - 工具：IDF 自带的 OpenOCD v0.12.0-esp32-20260703 与 `board/esp32p4-builtin.cfg`。
    - 步骤：只读连接，读取目标列表，`halt` 后读 PC，并在 2 秒内 `resume`。记录完整输出。
    - 验证板子仍在运行：经 /dev/ttyACM0（不拉 DTR/RTS）读取 resume 后的开机日志，确认无复位与异常。
    - 禁止：eFuse 写入、烧录、esptool write、改动固件源码。
    - 产物：evidence/1.5/（连接日志与结论）。
    - _Requirements: 3.19_

  - [x] 4.1 计时统计纯 C 模块
    - 新建 `firmware/main/face_perf_stats.{c,h}`（纯 C、零 IDF 依赖）：样本环、P50/P95/max、超阈计数；P95 取整规则与现有 `baseline` 命令一致（排序后取下标 `(n*95)/100`）
    - 新建 `firmware/main/test/test_face_perf_stats.c`：覆盖 n=1、n=2、重复值、取整规则，并与排序参考实现做随机对拍（≥1000 例），接入 [18]
    - design.md 原定把百分位函数放在 `sysdiag_core.c`；因表情线先于 sysdiag 推进，改放 L5 纯 C 文件，sysdiag 不依赖它。在 design.md 对应句旁加注说明
    - _Requirements: 2.10, 2.11_

### 第一部分：表情线（EFC 路线）

- 已退役（不再执行，保留历史）：原任务 2、3（已完成，EAF 专用）；原任务 4.2、4.3（已完成，EAF 专用）；原任务 4.4（中止，已重置，新文本见 31.4）；原任务 4.5、5–10（由 26–38 取代）。

状态：已并入 tasks.md。门禁项须经用户确认后才执行。

依据：design.md 的执行记录、B1、B5、Property 8、资源预算与实施顺序、Testing Strategy 的 X1–X5，以及集成测试表的 2.7、2.8 行；bugfix.md 的 2.8、2.9、2.13、2.14、3.21 行。2.7 未直接读取，其存在由 2.8 行的交叉引用确认。

术语：F0 指板上当前运行的 EAF 固件，未改动。E0 指 PC 侧素材与宿主测试，不烧录。E1 指 F0 上的板上基线。

顺序：26 → 27 → 30.1 → 30.2 → 30.3 → 30.4 → 30.5。28 在板上执行（不烧录），可与 E0 并行。

选择说明：
- 任务 28 取自 Testing Strategy 的 X2、X3。design 的 Examples 段不在本次读取范围内，未核对。
- 任务 29 已取消（2026-11-15，用户决定）。原内容为 10 次 RTS 复位的 splash 统计，28 只做开机日志与眼见确认。

门禁：30.4（B5 清单已于 2026-11-15 确认）。30.1 的素材授权已由用户裁决（2026-11-15），不再作为门禁。

与 design 的两处偏离，请用户确认：
1. 30.3 对 [18] 只追加 EFC 检查，旧 EAF 内容留到 30.4 确认后删除。design 要求 30.3 通过 [18]，B5 又要求删除前先确认。
2. 30.4 只删除经 grep 确认引用已归零的文件。仍被 ui_widget.c、face_perf.c 引用的代码与素材，随任务 31 的改写删除。这两个文件要到 E2 才改写，提前删除可能断开构建。

未决（不阻塞本批任务）：
- splash 摘要中 d 字段的定义不在读取范围内，沿用原字段名。
- B2、B6 已于 2026-11-15 决定（见 31.0）。

- [x] 26. 冻结 E0 素材流水线（PC 资产与宿主测试，不烧录）
  - 输入：`evidence/source/EmotePack/gif/*.gif`，共 23 个，848×848，帧时长 40 ms。该目录不入库。记录每个 GIF 的 sha256、帧数与总时长。目录缺失则停止并报告。
  - 编码器：`evidence/source/esp_efc_codec_1.1.0/pc/`，registry 归档 1.1.0。归档 sha256 以 `d50c10fb` 开头、`9316` 结尾，全长值从 CHECKSUMS 核对后记录。同时记录 `efc_py.py` 与 `video_to_efc.py` 的 sha256。
  - 冻结参数：tile 4，zlib 开启，PRED 按 `encode_frames(compress=True, legacy=False)` 择优；460×460 键控输出；100 ms 网格 sample-and-hold；键色 0xF81F，键阈值 α<128；先按 α 预乘 RGB，再做 LANCZOS 缩放。
  - 新增 `tools/efc_build.py`，替换 `tools/gif2efc.py`。启动时核对两份编码器脚本的 sha256，不一致则拒绝运行。记录 python、pillow、numpy 版本。
  - 新增 `tools/efc_verify.py` 的 `--contract` 模式，检查 magic `EFC1`、460×460、帧数、大小上限、键占比与透明像素占满情况。其余测试在 30.3 补全。
  - 用 `python3 tools/efc_verify.py --contract <路径>` 分别对 `firmware/components/app_ui/ui/assets/face` 与三份 `face_*.eaf` 运行，记录 X1 反例：magic 不是 `EFC1`，键占比为 0，可见占比为 0%。`assets/face` 是新目录，当前 EAF 位于 `assets/` 下，两处都要跑，避免目录不存在被误判为通过。
  - 冻结记录（哈希、参数、命令行）写入 `evidence/source/efc_460/`，与产物同目录。
  - _Requirements: 2.9, 2.13_

- [x] 27. 生成 23 段键控 .efc 与尺寸记录（PC 侧，产物暂存于 evidence）
  - 运行 `tools/efc_build.py`，把 23 个 `<name>.efc` 输出到 `evidence/source/efc_460/`。入库前产物只留在此目录（B1）。
  - 复用 `evidence/source/efc_460/` 已有产物，需以下六项全部核对通过，否则全量重跑：
    1. 每段源 GIF 的 sha256 与 26 的记录一致。
    2. `efc_py.py`、`video_to_efc.py` 与编码器归档的 sha256 与 26 一致。
    3. 参数与 26 一致：460、100 ms、tile 4、zlib、PRED、键色 0xF81F。
    4. 每个 .efc 经 `efc_py.decode_frames` 读回，与 RGB565 参考逐元素相等。
    5. 每个 .efc 的 sha256 与字节数与 SIZES.md 一致。
    6. SIZES.md 的合计与最大值应为 206,846 字节与 44,565 字节（design 执行记录）。若实测不同，以实测为准，并报告用户以修订设计。
  - 尺寸记录（SIZES.md）每段至少包含 out_frames、efc_bytes、key_share_permille、changed_tile_pct、lit_share_pct、centre_offset_px。changed_tile_pct 供 2.10 的简单片段判定（≤3%）使用，分类表属 B6，待用户确认。
  - 构图偏移只记录，不作为失败条件（例：`question` 为 +58 px@460）。
  - _Requirements: 2.13, 2.14_

- [x] 28. F0 开机日志与眼见确认（不烧录、不改代码，需用户在场）
  - 前提：板上运行 1.3 烧录的 F0 固件（ELF 前缀 d870bafb，见 evidence/1.3）。核对启动日志中的 app_init ELF SHA256 与 evidence/1.3 一致；不一致则停止报告，不烧录。
  - 执行：单次 RTS 复位，或由用户按 RST，经 serial_excl.py 独占串口，抓取开机日志。预期含 `face src 460x460` 与 `efc clip ok` 行。
  - 眼见确认（需用户在场）：用户看屏幕，记录是否黑屏（X2）以及 splash 是否卡顿。F0 没有 lit、splash 统计等日志，因此这两项只以眼见为准。
  - 证据：脱敏后的启动日志原文、ELF SHA256、用户的眼见记录，存 evidence/28/。
  - 本任务先于任何烧录（14.2、31.6）执行。
  - _Requirements: 2.7, 2.8, 2.9_

- 已取消：原任务 29（F0 splash 统计基线，10 次 RTS 复位）。用户于 2026-11-15 决定不做，28 只做开机日志与眼见确认。编号 29 保留，不再执行。

- [ ] 30. E0 收尾：正式素材、MANIFEST、宿主套件 [18] 与 EAF 退役
  - 本任务由下列子任务组成，按 30.1 → 30.2 → 30.3 → 30.4 顺序执行。
  - _Requirements: 2.9, 2.13, 2.14, 3.21_

  - [x] 30.1 生成正式素材到 firmware 路径
    - 素材授权已由用户裁决（2026-11-15，个人 DIY，非商业用途），不再作为门控
    - 由 `tools/efc_build.py` 直接生成 23 个键控 `<name>.efc` 到 `firmware/components/app_ui/ui/assets/face/`，不复制 27 的产物。
    - 每个文件的 sha256 应与 27 的尺寸记录一致。不一致即判定不可复现，回到 26 排查，不手改二进制。
    - 本任务只写素材，不改 C 源码与 `idf_component.yml`。
    - _Requirements: 2.9, 2.13_

  - [x] 30.2 写入 MANIFEST.json
    - 由扩展后的 `efc_build.py` 直接写出（用户 2026-11-15 决定：扩展脚本）。路径 `firmware/components/app_ui/ui/assets/face/MANIFEST.json`。数值不手写。
    - 字段按 B1：`encoder{package, version, archive_sha256, efc_py_sha256, video_to_efc_sha256}`；`params{size:460, step_ms:100, tile:4, zlib:true, pred:<实际值>, resample:"LANCZOS-premultiplied", key:"0xF81F", key_threshold:128}`；`env{python, pillow, numpy}`；`clips[]{name, src_sha256, src_frames, src_total_ms, out_frames, delay_ms, efc_bytes, efc_sha256, crc32_per_frame, frame0_key_permille, frame0_lit_share_pct, key_share_permille（全帧平均，仅供信息）, changed_tile_pct, lit_share_pct（全帧平均，仅供信息）, centre_offset_px, embedded（布尔值，测试内置的 4 段为 true，其余为 false）}`。
    - pred 写实际值（本批为 false；26、27 与 SIZES 记录均为 zlib-only），不得写 true。
    - `encoder.archive_sha256` 必须为 `d50c10fb…9316` 对应的全长值。
    - 提交：按 D-GIT 提交 `firmware/components/app_ui/ui/assets/face/` 下的 23 个 `.efc` 与 `MANIFEST.json`，以及 `tools/efc_build.py`、`tools/efc_verify.py`。不提交 `evidence/`、`.kiro/` 规格文件夹、EAF 时期改动或任何 `sdkconfig`。提交前用 `git diff --cached --name-only` 核对，只能包含上述路径。提交信息用中文，风格与仓库近期提交一致（先用 `git log --oneline -5` 查看）。
    - _Requirements: 2.13_

  - [x] 30.3 宿主套件 [18] 通过：EFC 往返、键控语义、可复现来源
    - 在 `tools/host_tests.sh` 的 [18] 下追加往返、键控语义、可复现来源三组检查，依据 Property 8，并加入契约检查。旧 EAF 内容保留，待 30.4 确认后删除。
    - 往返：固定种子生成随机 RGB565 帧序列（宽高为 4 的倍数）与键掩码，编码后解码，逐元素等于原帧。对 23 段输出经 `efc_py.decode_frames` 读回，与 RGB565 参考逐元素相等。
    - 键控语义：解码后键像素 alpha 为 0，其余为 0xFF，键色为 0xF81F。
    - 可复现来源：每段输出帧数等于 round(GIF 总时长 / 100 ms)；848→460 为等比正方形缩放；重跑 `efc_build.py` 得到的每个 .efc sha256 与 MANIFEST 一致。
    - 契约：`python3 tools/efc_verify.py --contract firmware/components/app_ui/ui/assets/face` 通过。这是 X1 的修复后复跑，结果应由失败变为通过。
    - 往返与键控检查可先在 `evidence/source/efc_460/` 上运行。契约与 MANIFEST 比对需 30.1、30.2 完成后执行。
    - _Requirements: 2.9, 2.13, 2.14_

  - [ ] 30.4 退役 EAF（清单已由用户确认，2026-11-15）
    - 清单已由用户确认（2026-11-15）。执行时仍按下列顺序，每一步前用 grep 确认引用。
    - 确认后执行：先删除 `tools/host_tests.sh` 中 [18] 的旧 EAF 内容，再删除 `tools/eaf_codec.py`、`tools/gif2efc.py`、`tools/gen_face_gifs.py`。删除前 grep 确认除 [18] 旧内容外无其他引用。
    - 引用归零后执行：`face_{neutral,happy,wink}.eaf`、`firmware/main/face_precheck.h`、`firmware/main/test/test_face_precheck.c`。执行前用 grep 确认无引用；有引用的随任务 31 删除。
    - 任务 31.2 完成后执行：`firmware/components/app_ui/ui/ui_face_fmt.h`、`firmware/main/test/test_ui_face_fmt.c`。
    - 随任务 31 改写：`ui_widget.c`、`face_perf.c` 中的 `lv_eaf_*` 调用与挂钩；`idf_component.yml` 中的 `esp_lv_eaf_player`。
    - 保留：`face_perf_stats.{c,h}`、`face_perf_geom.h`。`face_perf_screen.{c,h}` 暂不删除（用户 2026-11-15 决定：其用途待梳理，见 30.5）。
    - 删除前确认冻结线（任务 11–21）当前不引用待删文件。不删除 `evidence/2/`、`evidence/3/`。
    - 清单与执行结果写入 design.md 附录 A，无则新建。
    - 删除后 `tools/host_tests.sh` 应通过。按 `sim/README.md` 构建 sim 目标（关闭 `CONFIG_ESP_CUBE_ENABLE_FACE_ANIM`），确认链接清单不含 esp_efc_codec 与 lv_efc。
    - _Requirements: 2.13, 2.14, 3.21_

  - [ ] 30.5 梳理 `face_perf_screen` 的作用（只分析，不删除）
    - 列出 `face_perf_screen.{c,h}` 的全部调用方、输入与输出，说明它与 lit 占比计算的关系，并判断 31.2 的屏幕自检能否替代它。
    - 结论写入 `evidence/30.5/`，并提交用户决定：保留、合并或删除。用户决定前不改动该文件。
    - _Requirements: 2.9_

状态：已并入 tasks.md。门禁项须经用户确认后才执行。

依据：design.md 的 B2、B4、B6、B7、Property 9 与 10、资源预算。bugfix.md 经 grep 读取 2.12、2.14、3.9、3.13、3.16、3.21 行，上下文行另含 2.11、3.10、3.12、3.15、3.17、3.20，仅用于确认 ID 存在。B8、B9 正文与执行约定未读取，本稿只沿用 design 中的引用。

术语：F0 指板上当前运行的 EAF 固件，未改动。F' 指本任务构建并烧录的 lv_efc 固件（P4 目标）。

顺序：31.0 → 31.1 → 31.2 → 31.3 → 31.4 → 31.5 → 31.6。31.5 的板上测量须在 31.6 烧录 F' 之后执行，编号不变。

选择说明：
- B2 采用 lv_efc。R2b 触发时才改用解码器 API，本稿不提前实现。
- 31.2 只复用 ui_face_fmt.h 的纯 C 算法，接线与测试按用户裁决重写。
- 31.6 的三目标在已读取的 design 段落中未列出，本稿推定为 P4（表情开）、S31（表情关）与 sim。

门禁：31.0 已解除（B2、B6、B7 于 2026-11-15 决定）。31.1 另有 ESP_EFC_LVGL_WIDGET 前置核对。

与 design 的偏离：无。采纳的 B6 与 3.9 旧映射不同，3.9 已按采纳映射修改（2026-11-15）。

未决（不阻塞本批任务）：
- 内置来源的日志前缀：design 只定义 `face clip=sd:<name>`。
- alpha 0 值占比的告警阈值未定义。31.4 亮度阈值的数值未在已读范围内。31.4 的 D 口径与分项已按 design「板上实测方案（EFC 路线）」核对。
- B10（底色圆常显）未进 31.0 门禁，31.1 按现状实现。是否加门禁请用户决定。
- 修复前内部堆基线：内部堆基线取自任务 1.3（evidence/1.3/cmd_baseline_20.txt）。仅当该文件缺失时才需在 F0 补采，并须用户确认重烧 F0。
- 31.6 的执行约定未读取，实施前需先读。
- 三目标名称在已读取的 design 段落中未列出，见选择说明。
- Property 10 引用的 2.7、2.8、3.11 未核对正文，_Requirements_ 行未引用。

- [ ] 31. 实现 E2 设备侧改写：lv_efc 接入、加载与预检、观测与资源核对，烧录 F'
  - 范围：不改素材，不改 tasks.md、design.md、bugfix.md。
  - _Requirements: 2.11, 2.12, 2.14, 3.9, 3.10, 3.12, 3.13, 3.15, 3.16, 3.17, 3.20, 3.21_
  - [ ] 31.0 门禁：B2、B6、B7 已决（2026-11-15）
    - B2：采用 lv_efc（已决）。
    - B6：采用提议映射（已决）。默认项：speak 沿用当前情绪片段（帧间隔 70 ms）；sleepy 映射 yawn；mute 与 badminton 不映射。3.9 已按此修改。
    - B7：测试内置 4 段（`smile_static`、`laugh`、`investigate`、`shocked`，合计 29,641 字节）。23 段候选池按全部内置计算预算（206,846 字节）。未内置片段回退 `smile_static` 并告警（2.15、3.22）。
    - 门禁已解除，31.1 起可执行。
    - _Requirements: 2.12, 2.14, 2.15, 3.9, 3.10, 3.13, 3.22_
  - [ ] 31.1 接入 lv_efc：依赖、Kconfig、CMake 与表情对象（B2）
    - 前置核对：`ESP_EFC_LVGL_WIDGET` 须为 y。design 说它由 P4 表情开关选中，实施时核对是 `select` 还是默认值；若是默认值，则在项目 Kconfig 或 `sdkconfig.defaults` 显式设置。核对前不改 Kconfig。
    - `firmware/main/idf_component.yml`：删除 `espressif/esp_lv_eaf_player: "^0.3.0"`，加入 `espressif/esp_efc_codec: "1.1.0"` 并加 `rules: - if: "target == esp32p4"`。S31 构建须仍通过，固件体积不增加。
    - Kconfig：`ESP_EFC_DECODE_4X4_ONLY`、`ESP_EFC_USE_SIMD` 保持默认，`ESP_EFC_PERF_LOG` 默认关。不手工编辑 `sdkconfig.p4c5`（3.20）。
    - CMake：`EMBED_FILES` 只列测试内置的 4 段（见下条），符号 `_binary_<name>_efc_start/end`。改动包在 `#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM` 内，sim 不链接 esp_efc_codec（3.21）。
    - EMBED 只列测试内置的 4 段（`smile_static`、`laugh`、`investigate`、`shocked`）。候选池 23 段保留在 `assets/face/`，不编入固件（2.15）。
    - 表情对象：`ui_w.face_anim = lv_efc_create(scr)`，尺寸 460×460 居中，`lv_image_set_inner_align(…, LV_IMAGE_ALIGN_CENTER)` 取代 `CONTAIN_DOWNSCALE`。尺寸须与片段 1:1。
    - 帧间隔：`FACE_DELAY_*_MS` 常量留在 `ui_internal.h`，经 `lv_efc_set_frame_delay()` 写入。
    - 挂起入口：新增 `ui_face_set_running(obj)`，`running = !splash_active && !panel_open`，`running ? lv_efc_resume : lv_efc_pause`，须在 LVGL 锁内调用。改写 `ui_pages_create()`、`master_timer_cb()`（`el >= SPLASH_MS` 清除 splash 后）与面板展开收起处，替换全部 `lv_eaf_pause/resume`。
    - 底色：`face_bg` 维持现状（3.12）。删除 `ui_widget.c` 的 EAF 引用；30.4 留下的 EAF 代码与素材在 31.1、31.4 改写后经 grep 确认归零再删。
    - _Requirements: 2.12, 3.10, 3.12, 3.20, 3.21_
  - [ ] 31.2 日志与自检（B3、B4 日志）
    - B3 日志：SD 热载成功输出 `face clip=sd:<name> …`；预检失败输出固定原因码并保留当前表情（3.13）。日志不含文件内容或用户话语（3.15）。
    - 自检（B2）：每次加载后读 `lv_image_get_src(obj)` 的 `header.cf` 与 alpha 平面 0 值占比，`cf` 不符时告警，占比输出数值。alpha0 校验与屏幕自检（lit）都以第 0 帧为准：alpha0 对照 MANIFEST 的 `frame0_key_permille`（÷10），屏幕 lit 对照 `frame0_lit_share_pct`；全帧平均只作信息记录（D-FRAME0）。
    - 算法：复用 `ui_face_fmt.h` 的纯 C 计算。RGB565A8 布局为颜色平面 w·h·2 字节，紧跟 alpha 平面，行跨度 w。接线与宿主测试按 lv_efc 路径重写。
    - B4 日志：`ESP_EFC_PERF_LOG` 输出只在观测固件中出现，作为 D 分项交叉核对；开关在 31.4 设置。
    - _Requirements: 2.12, 2.14, 3.13, 3.15_
  - [ ] 31.3 加载、预检、热载与挂起重置（B3、B8；Property 9、10）
    - 内置加载：`face_mood_load(tag)` 按 31.0 采纳的 B6 映射选片段（见 bugfix.md 3.9）。顺序固定为 `lv_efc_set_src_data(obj, start, len)` → `lv_efc_set_frame_delay(obj, FACE_DELAY_BASE_MS)` → `lv_efc_set_loop_enabled(obj, true)` → `ui_face_set_running(obj)`。不得紧跟 `lv_efc_play()`（B4），不得用 `lv_efc_set_src(path)`（B7）。
    - 重挂起（B8）：每次 `lv_efc_set_src_data` 之后调用 `ui_face_set_running(obj)`。
    - 锁：读文件与预检在任务上下文执行，不持 LVGL 锁；只有 set_src_data、帧间隔与挂起调用持 `lvgl_port_lock`。
    - SD 热载：`face_load_task()` 等 `app_ui_splash_active()` 为假（50 ms 轮询，最长 5 s）。名称匹配 `[a-z0-9_-]{1,15}`（3.13）。整个文件读入 PSRAM 缓冲（上限 262,144 字节），预检后持锁依次调用 set_src_data、`lv_efc_set_frame_delay(obj, 50)`、`ui_face_set_running(obj)`，输出日志，解锁后释放缓冲。此路径不含 set_loop_enabled，实施时核对 set_src_data 是否保留 loop 状态。
    - 失败：文件缺失、读取失败或预检失败时保留当前表情并告警（3.13）。
    - 预检：新建 `firmware/main/face_efc_precheck.h/.c`（纯 C），经 `face_efc_api_t` 注入 open、get_info、frame0_key_permille、close；`ui_port_esp.c` 映射到 `esp_efc_dec_open/get_info/frame_with_alpha/close`。按序判定，首个失败给出原因码：`OPEN_FAILED`、`NOT_460`、`SCALED`、`TILE_NOT_4`、`FRAMES_OUT_OF_RANGE`（帧数 [1, 300]）、`TOO_LARGE`（>262,144 字节）、`MOSTLY_KEYED`（第 0 帧键占比 ≥ 990‰）。
    - MOSTLY_KEYED 用 `esp_efc_dec_frame_with_alpha` 解码到 634,800 字节 PSRAM 暂存，统计 alpha=0 像素。解码失败按失败处理，返回前释放暂存。
    - **Property 9: 预检判定的完备性** - 宿主 PBT。用注入桩生成 open、get_info、键占比与字节数的组合，断言接受当且仅当全部条件成立；拒绝时原因码唯一，显示不变。
    - 缩放防护：加载后断言宽高均为 460，否则告警并回到内置默认片段（B3）。
    - **Property 10: 挂起状态机与加载副作用** - 宿主桩测试覆盖 splash 结束、面板收展、内置加载、SD 热载、mood 切换、CLI 替换，断言每次加载后 `running = ¬splash ∧ ¬panel`。running 为假时解码次数为 0（加载时同步解码的第 0 帧除外），板上用 31.4 挂钩核对。
    - 新增缓冲走 PSRAM 堆，不新增内部 RAM 静态缓冲（3.16）。
    - _Requirements: 2.12, 2.14, 3.12, 3.13, 3.16_
  - [ ] 31.4 观测挂钩：`face_perf.c` 改接 lv_efc（B4）
    - `face_perf.c` 改为 lv_efc 挂钩，实测方案见 design 的 Hypothesized Root Cause 中「板上实测方案（EFC 路线）」。
    - D 是 lv_efc 播放定时器（帧定时器）的回调耗时：按 user_data 为 efc 对象找到该定时器，用 `lv_timer_set_cb()` 换成计时包装。D 覆盖解码、键控准备（alpha 生成）与发布之和。
    - 纯解码分项可选，经 `-Wl,--wrap=esp_efc_dec_frame` 获得，默认关闭，启用前需在构建期验证。`CONFIG_ESP_EFC_PERF_LOG` 的分项输出只作交叉核对。
    - `face_perf_stats` 与 `face_perf_geom.h` 保持不变。`face_perf_screen` 判定由 C_BG 差异改为亮度阈值（lit）。
    - 观测固件开启 `CONFIG_ESP_EFC_PERF_LOG`，作为 D 分项交叉核对。
    - CLI `faceperf start <秒> <片段>`：在 LVGL 锁内用 `lv_efc_set_src_data()` 切到指定内置片段，结束后按原暂停态恢复，不追加 restart。`faceperf report` 与 `uimood <tag>` 语义同 EAF 版。
    - 样本环 16 KB 走 PSRAM 堆，测量时分配，测完释放。删除 `face_perf.c` 残留的 EAF 引用。
    - _Requirements: 2.11, 3.17_
  - [ ] 31.5 资源核对：加载前后的 PSRAM 与内部 RAM（3.16，资源预算）
    - 加载路径前后采样：PSRAM 空闲与增量（`heap_caps`），内部 RAM（`MALLOC_CAP_INTERNAL`）最大连续块与空闲。内置加载与 SD 热载各测一次。
    - 预期 PSRAM 增量约为 blob（内置不超过 44,565 字节，SD 不超过 262,144 字节）加 634,800 字节帧缓冲。偏差超出即报告。
    - 内部 RAM 最大连续块与空闲不低于修复前基线，启动与 CLI 任务栈分配须成功（3.16）。修复前基线取自 evidence/1.3/SUMMARY.md（任务 1.3 板上基线）。内部 free 与最大块值须从该文件全文取得，其前 40 行只有启动行的总量（free heap=30984339、psram=30965708）。仅当全文也缺失这两项时，才需在 F0 补采一次，须用户确认重烧 F0；否则不新增板上步骤（3.17）。
    - 解码器内部分配位置 design 标为待测，本次一并记录。
    - 板上测量须在 31.6 烧录后执行。本任务只完成采样代码与记录格式。
    - _Requirements: 3.16, 3.17_
  - [ ] 31.6 三目标构建、sim 链接检查，并烧录 F'
    - 三目标：推定为 P4、S31 与 sim（见选择说明）。
    - sim：`CONFIG_ESP_CUBE_ENABLE_FACE_ANIM` 关闭，构建通过，链接产物不含 esp_efc_codec 符号（3.21）。
    - S31：表情关闭时构建通过，固件体积与改动前对照，不增加（B2）。
    - P4：构建通过。`idf.py size` 记录 DRAM `.bss/.data` 增量（不超过 128 字节），并记录 flash rodata 增量，与 MANIFEST 对照（design 数值为 206,846 字节）。
    - 烧录：按执行约定（未读取）全量烧录 P4 的 F'。烧录后只做启动与串口 CLI 检查（3.17），表情验收留给后续任务。不提交 `sdkconfig.p4c5`（3.20）。
    - _Requirements: 3.17, 3.20, 3.21_

状态：已并入 tasks.md。门禁项须经用户确认后才执行。

依据：design.md 的 Property 3、4，实施顺序第 4、5 项，Fix Checking 的 expectedBehavior_face，Preservation Checking 用例 5，Integration Tests 中 2.7–2.12、3.8–3.11、3.13、3.16 的行，根因表 B2、B3、B9、B10，选型段第 2–4 条（R2b），待决清单中 B6、B2、B10 及其上下文。bugfix.md 经 grep 读取 2.7–2.12、3.8–3.11、3.13 行。3.16 正文未读，其存在依据设计集成表与 part2 的 grep 记录。

术语：F' 指 lv_efc 固件（P4 目标，任务 31 烧录）。R2b 指解码移出 LVGL 任务，即改用解码器 API。E3 指板上验收（任务 32–35），E4 指条件优化（任务 36）。证据目录写作 evidence/<任务号>/，design 中只出现 evidence/29/ 与 evidence/2/，此为本稿沿用的约定。

顺序：32 → 33 → 34 → 35，36 仅在 33.3 判定 R2b、R3 或 R4 后执行。32 与 34 依赖 31 烧录的 F'。烧录后先核对启动日志的 app_init 版本、编译时间、ELF SHA256 与 sysdiag 行（design 集成测试统一纪律）。串口按 serial_excl.py 独占。

选择说明：
- 32 拆为 splash 计时（2.7）、splash 后表情与播放（2.8）、眼见复验（2.7、2.9）三个子项。
- 33 按场景拆分为空闲与电台播放，判定单列为 33.3，因为 design 以 33 为 36 的入口。
- 34 的 SD 热载成功分支与失败保留分开，失败分支需要人工更换 SD 卡内文件。
- 35 只保留 3.8–3.11 的语义核对，依 Property 4 执行。
- 36 的子任务与 33.3 决策表一一对应：36.1 为 R2b，36.2 为 R3，36.3 为 R4，R2 无独立子任务。底色圆（B10）的决定不属于 R1–R4，移入门禁。EAF 专有的整帧 memset 与 RLE 展开不列入。

门禁：
- 需用户在场：32、32.2、32.3、34、34.2、34.3、35、35.1、35.4（眼见、手势与 SD 卡人工操作）。
- 33.3 的 R3、R4 判定条件须先经用户裁定（见偏离 1）。
- 35.2 的 mood 映射按 31.0 采纳的 B6 映射核对。
- 底色圆可见性（B10）需用户决定。不属于 R1–R4，故不列入 36。改为可见会增加每步整圆重绘（约 185k 像素）的 R 成本，并改变 2.9 的现有呈现。未经决定不改动。

与 design 的偏离或冲突（请用户确认）：
1. R1、R2、R3、R4 在 design.md 中没有定义，只有 R2b 有定义（选型段第 2、4 条，待决清单）。改写前的 tasks.md 8.3 与 9.x 有这四条规则，本稿已取回并适配 EFC，列于 33.3 决策表，待用户确认。R3、R4 未经裁定前，不启动 36.2、36.3。
2. design.md 的 B6 有两个含义。根因表 B6 是「文档误述」，由任务 38.3、38.4 更正。待决清单 B6 是「八情绪与三状态映射（PENDING）」。35.2 采用待决清单的含义，与任务指示及 part2 说明一致。
3. design 没有独立的 36 选项列表。B3 写「若超预算走 B2 的 R 规则」，但 B2 行没有 R 规则。

未决（不阻塞本批任务）：
- 2.14 预检的实现在 31.3。34.3 的板上预检步骤（超尺寸、截断、键占比 ≥99%）保留，交叉引用见 31.3；用户不同意时删去。
- 3.12 与 3.21 已分别列入 31.3 与 31.6 的 _Requirements。两者都没有板上步骤，故不列入 32–36。
- 33.1 的窗口已改为每个测量状态至少 60 s，思考态在 200 ms 下恰为 300 个样本，无余量。执行前须确认 faceperf 采样按状态帧间隔驱动。
- 34.4 的修复前基线引用 evidence/1.3/SUMMARY.md（见 31.5）。该文件前 40 行只有启动行的总量（free heap=30984339、psram=30965708），内部 free 与最大块值须从全文核对。仅当全文也缺失时，才需在 F0 补采一次，并须用户确认重烧 F0。
- 35.1 的「约 2.4 s 淡入」口径未在读取范围内核对。

- [ ] 32. F' 板上验收：splash 流畅、表情可见、连续 ≥10 次 RST 或上电（需用户在场）
  - 对应 Property 3：splash 期间 I_refr P95 ≤50 ms、无单次 >100 ms、D 样本为 0。splash 结束后表情持续推进，cf=RGB565A8，alpha0 与 MANIFEST 键占比差 ≤0.5 个百分点，lit 差 ≤5 个百分点。
  - 证据目录 evidence/32/。
  - _Requirements: 2.7, 2.8, 2.9_
  - [ ] 32.1 splash 计时（自动，RTS 复位 10 次）
    - 执行 `reboot_soak.py 10 45 --face`，抓取 `splash:`、`face first refresh`、`face screen check`、`cf=` 行。
    - 每次记录 n、P95、max、>100 ms 次数与 d。通过条件为 P95 ≤50 ms、max ≤100 ms、d = 0。10 份摘要存入 evidence/32/。
    - _Requirements: 2.7_
  - [ ] 32.2 splash 后表情与播放对照（需用户在场）
    - 与 32.1 同次采集，逐次记录 `face first refresh +Xms`（≤100 ms）、`cf=RGB565A8`、alpha0 与 lit。
    - 用户在场另做若干次 RST 或上电，逐次核对主屏表情持续推进、不黑屏。
    - 播放期间执行 `panel 1`、`panel 0` 与手势滑动，记录 faceperf 的 I_loop P95 与 max（≤50 ms、≤100 ms）。用户确认面板滑出与手势响应。
    - _Requirements: 2.8_
  - [ ] 32.3 眼见复验（需用户在场）
    - 用户确认 splash 无明显卡顿，至少 1 次。
    - 用户将板上照片与 `evidence/source/efc_460/contact_sheet_848_over_bg.png` 对比，附照片或文字描述。结论记入 docs/19 §47 #3。
    - _Requirements: 2.7, 2.9_

- [ ] 33. 逐段帧耗时测量与修复路径判定
  - 对应 2.10：简单片段 T P95 ≤33 ms（具备 ≥30 fps 能力）。简单片段指待机片段与 PC 侧变化 tile 占比不超过 3% 的片段，分类以 design.md 为准。
  - 对应 2.11：其余片段 T P95 ≤66 ms（具备 ≥15 fps 能力）。
  - T = D + R + F。D 为播放定时器回调耗时（解码、键控准备与发布之和），由 face_perf 的定时器包装取得。
  - 证据目录 evidence/33/，附日志原文与 cputop 输出。
  - _Requirements: 2.10, 2.11, 3.16_
  - [ ] 33.1 空闲场景逐段计时
    - 对 23 段逐段执行 `faceperf start 60 <片段>`，每段前后各取一次 `cputop`，记录 LVGL 任务 CPU%。
    - 测量窗口：每个测量状态至少运行 60 s。帧间隔取设计值（design Property 4）：待机与聆听 100 ms，思考 200 ms，说话 70 ms。按间隔估算，100 ms 约 600 个样本，200 ms 恰为 300 个，70 ms 约 857 个，均满足 n ≥300。思考态无余量，实测 n 不足时须核对。
    - 每段报告类别、场景、n、D/R/F/T 的 P50、P95、max 与超阈计数。
    - _Requirements: 2.10, 2.11_
  - [ ] 33.2 电台播放场景逐段计时
    - 与 33.1 同口径，在电台播放中执行。
    - 记录两场景的差值，说明电台播放对 T 的影响。
    - _Requirements: 2.10, 2.11_
  - [ ] 33.3 R1–R4 判定
    - 规则取自改写前的 tasks.md 8.3/9.x，设计文档未给出 R1、R3、R4 的定义，待用户确认。
    - 路由（原 8.3）：不满足 R1 时，按 D_P95 与 R_P95 + F_P95 的占比进入 R2/R2b/R3/R4。
    - 决策表（阈值与触发条件按原文保留，EAF 专有项已替换或丢弃）：

    | 规则 | 触发条件（原文） | EFC 处置 | 36 子任务 |
    |---|---|---|---|
    | R1 | 简单 T_P95 ≤33ms、复杂 ≤66ms、播放与切换期间 I_loop P95 ≤50ms 且无 >100ms | 接受，不执行 36 | 无，36 整体跳过 |
    | R2 | MAIN_PLAYBACK 460×460 且 T_P95 超预算，且 D_P95 > 0.5 × T_P95 | 丢弃 EAF 专有项：vendoring（EFC 解码核心为预编译库，无 esp_eaf_dec.c）、O1 整帧 memset（输出缓冲跨帧保留，不得清零）、O2 缓冲预分配（解码器内部）、O3 RLE 展开、O4 调色板 LUT，以及条件套件 [19]。EFC 替代：用 CONFIG_ESP_EFC_PERF_LOG 的 prepare 分项核对 alpha 生成占 D 的比例，仅作交叉核对 | 无独立子任务，按 R2b 行判定 |
    | R2b | 至少一段 P95 超标，且 D 是超标的主要来源（设计定义，「主要来源」比例口径待定） | 解码移出 LVGL 任务，改用解码器 API，即 design 选型段第 4 条的独立解码任务加双缓冲。保留原 9.2 的 O5 写法（core1，优先级低于 LVGL）。丢弃 JPEG 分支（ESP_EAF_ENCODING_JPEG）。原前置条件「O1–O4 后」不再适用 | 36.1 |
    | R3 | R_P95 + F_P95 超预算（原文未给预算值，待用户确认） | 不用宿主预计算差异包围盒写入片段元数据。lv_efc 的 publish 在帧尺寸不变时已只失效变化外接框（design 选型段第 1 条），改为核对该行为。保留 cf=RGB565 核对与 LVGL 9.6 PPA draw unit 评估 | 36.2 |
    | R4 | 460 仍不达标（即仍不满足 2.10、2.11） | 原方案为素材改为 230 等比版本，再用 PPA 放大到 460。EFC 下 230 不是 4 的倍数（design 分辨率约束），且 2.14 预检要求 460×460，尺寸须用户裁定。源分辨率选项丢弃：EFC 源为 848，大于 460，无放大步骤 | 36.3 |

    - R2 同样未在设计中定义，待用户确认。R3、R4 未经裁定前，不得据此启动 36.2、36.3。
    - 判定结论与依据数字写入 evidence/33/。
    - _Requirements: 2.10, 2.11_
  - [ ] 33.4 加载前后内部 RAM 对照（B9）
    - 每次加载前后记录 `MALLOC_CAP_INTERNAL` 的可用量与最大块，确认解码器的内部分配（B9 未知项）。
    - 口径与 34.4 相同，内部 RAM 单列。
    - _Requirements: 3.16_

- [ ] 34. 切换、SD 热载与失败保留验收（需用户在场）
  - 对应 2.12：mood 切换与热载期间不黑屏，单次 I_loop ≤100 ms；切换后 lit 与 MANIFEST 差 ≤5 个百分点。
  - 对应 3.13：热载成功则替换；文件缺失或预检失败则保留当前表情并告警。
  - 证据目录 evidence/34/。
  - _Requirements: 2.12, 3.13, 3.16_
  - [ ] 34.1 mood 切换 50 次
    - `faceperf start 60` 期间，交替执行 `uimood happy` 与 `uimood neutral` 共 50 次。
    - 每次记录 I_loop max 与 lit，并保留对应的 `face clip=` 日志行。
    - _Requirements: 2.12_
  - [ ] 34.2 SD 热载与 RST 后自动热载（需用户在场）
    - 用户在场放入合法的 `face/idle.efc`。先经 CLI `face` 命令热载，再 RST，核对开机后的自动热载。
    - 每次热载记录 I_loop max（≤100 ms）。加载时第 0 帧的同步解码（D0）持锁，须计入 max。
    - 日志为 `face clip=sd:<名>`，delay 为 SD 热载值 50 ms（判据见 35.3）。
    - _Requirements: 2.12, 3.13_
  - [ ] 34.3 热载失败保留（需用户在场）
    - 用户在场分别执行三种情况：`face/idle.efc` 缺失；文件损坏；预检拒绝的超尺寸或截断文件（2.14 的预检步骤，见 31.3）。
    - 每种情况记录预检原因码与告警行，并截图确认当前表情保持不变。
    - _Requirements: 3.13_
  - [ ] 34.4 SD 热载峰值内存与资源对照
    - 热载前后与 RST 后各取一次 `heapstat` 与 `memstat`，记录峰值。峰值须包含每次加载复制到 PSRAM 的 blob 与第 0 帧解码（选型段第 3 条）。
    - 对照表单列 HP 内部堆、最大块与 RTCRAM 区，修复前基线取自 evidence/1.3/SUMMARY.md（见 31.5 与未决）。
    - _Requirements: 3.16_

- [ ] 35. 表情语义保持验收（需用户在场）
  - 对应 Property 4：splash 内容与淡入时机、B6 mood 映射与从头播放、帧间隔 100/200/70 ms（SD 热载 50 ms）、面板暂停与恢复、手势行为，与 F0 的 UI 语义一致。
  - F0 上无 `uimood` 时以 `face` 热载代替，并在 evidence/35/ 注明（Preservation 用例 5）。
  - 证据目录 evidence/35/。
  - _Requirements: 3.8, 3.9, 3.10, 3.11_
  - [ ] 35.1 splash 内容与淡入（需用户在场）
    - 用户眼见 splash 含立方体、EspCube 标题、副标题与三点，并在约 2.4 s 后淡入主屏。
    - _Requirements: 3.8_
  - [ ] 35.2 mood 映射（B6）与从头播放
    - 依次执行 `uimood happy`、`relaxed`、`sad` 及其余标签，记录 `face clip=` 行的片段名。
    - 判据：片段名与 31.0 采纳的 B6 映射逐项一致，切换后从头播放。
    - 本项映射以 31.0 采纳的 B6 为准（2026-11-15）。
    - _Requirements: 3.9_
  - [ ] 35.3 帧间隔与底色呼吸
    - 按会话状态切换，核对 `face clip=` 行的 delay 字段：待机与聆听 100 ms、思考 200 ms、说话 70 ms；SD 热载 50 ms。
    - 底色呼吸：B10 现状为隐藏底色圆，板上不可见。以代码审读确认呼吸参数未改动，结论记入 evidence/35/。
    - _Requirements: 3.10_
  - [ ] 35.4 面板暂停、恢复与手势（需用户在场）
    - `panel 1` 期间 D 样本停止增长，`panel 0` 后恢复。
    - 面板展开中执行一次 `face` 热载，D 样本仍为零（B8 的重挂要求，见 Property 10）。
    - 用户在场完成展开、收起与手势滑动，确认面板自动收起与手势行为不变。
    - _Requirements: 3.11_

- [ ] 36. 渲染优化（条件执行：仅 33.3 判定 R2b、R3 或 R4 时）
  - 判定为 R1 时，本任务标记为跳过，并在 evidence/36/ 写明跳过依据。
  - 子任务与 33.3 决策表对应：36.1 为 R2b，36.2 为 R3，36.3 为 R4。R2 无独立子任务。
  - 不复用 EAF 专有项：整帧 memset（EFC 的 1:1 显示路径无逐帧整帧清零，B3）与 RLE 展开（决策表 R2 行）。B4 的双重解码不得重演，EFC 只调用一次 `lv_efc_set_src_data`，且不紧随 `lv_efc_play`。
  - 改动后，33.1–33.2 的测量与 32、35 的语义核对须重跑（推定，设计未明文）。
  - _Requirements: 2.10, 2.11_
  - [ ] 36.1 R2b：解码移出 LVGL 任务
    - 改用解码器 API：独立解码任务加双缓冲，LVGL 只切换数据指针（选型段第 2、4 条）。
    - 设计估计约 150–250 行并需重新测试。键控 RGB565A8 发布、帧间隔与局部失效行为须保持。
    - 仅 33.3 判定 R2b 时执行。
    - _Requirements: 2.10, 2.11_
  - [ ] 36.2 R3 渲染/刷屏受限（R_P95 + F_P95 超预算）
    - 先核对 `cf=RGB565`（加载日志）。
    - 不由宿主预计算差异包围盒。核对 `invalidate_shown` 的失效区是否只覆盖变化外接框（design 选型段第 1 条）。仅当结论为「不一致」时才改代码，并收紧到外接框。
    - 最后评估 LVGL 9.6 PPA draw unit（`CONFIG_LV_USE_PPA`），启用前单独做 30 分钟稳定性验证。
    - _Requirements: 2.10, 2.11_
  - [ ] 36.3 R4 460 仍不达标（素材尺寸须用户裁定）
    - 原方案：素材改为 230 等比版本（源分辨率选项已丢弃，见决策表），解码后用 `esp_driver_ppa` 的 `ppa_do_scale_rotate_mirror` 在 LVGL 之外放大到 460×460 RGB565 缓冲（PSRAM），LVGL 绘制未缩放图像。
    - 裁定前不生成素材：230 不是 4 的倍数（design 分辨率约束），且 2.14 预检要求 460×460。
    - 插值观感在检查点 10 由用户眼见确认。
    - _Requirements: 2.9, 2.10, 2.11_
  - [ ] 36.4 复测与资源对照
    - 按 33.1–33.2 的口径（每状态至少 60 s，n ≥300）重跑 23 段 × 两场景。对比 cputop 与 33.4 的内部 RAM 对照。
    - 结论写入 evidence/36/，并判定是否满足 2.10、2.11。
    - _Requirements: 2.10, 2.11, 3.16_

状态：已并入 tasks.md。门禁项须经用户确认后才执行。本稿的检查均未执行，文中只写通过判据，不记录结果。

依据：design.md 的执行记录（第 13–19 行）、PBT 套件 [15]–[18]（第 665–681 行）、需用户确认事项（第 711–730 行），以及 Integration Tests 表的相关行（经 grep 读取）。bugfix.md 的条款 ID 经 grep 核对，本稿引用的 ID 均存在。

术语：EFC 指 esp_efc_codec 1.1.0 的 `.efc` 格式与 `lv_efc` 播放对象。键控指 EFC 颜色键透明，为默认方案；不透明版只作对照。

顺序：37 → 38.2 → 38.3 → 38.4 → 38.1 → 38.5 → 39，编号不变。38.2 需要 37 的用户结论；38.3 与 38.4 的勘误注指向 38.2 的批次记录，故 38.2 先写。

选择说明：
- 37 以 EFC 联系表 `evidence/source/efc_460/contact_sheet_848_over_bg.png`（design 的 2.9 行）替代旧的 contain 与 cover 对比。EFC 的缩放在 PC 侧完成，旧 9.2（JPEG）与 9.4（PPA）的条件不再适用，改为检查任务 36 新增的片段。
- 37 的 RST 与冷上电次数沿用旧 10；RTS 复位改为引用任务 32 的 `reboot_soak.py 10 45 --face`（design 的 2.7 行）。
- 38 的映射：旧 24.5 → 38.1；旧 24.1 → 38.2 的批次记录；旧 24.2、24.3 → 38.2 的 §0 与 §47 更新；旧 24.4 → 38.3（§40/§42/§44）与 38.4（§46）；docs/09 第 16、43 行 → 38.5（新增）。
- 39 保留旧 25 的全部检查，[18] 换成 EFC 版。旧的"若任务 9 触发则含 [19]"改为"若任务 36 触发则含 [19]"。

门禁：
- 37 全部需用户在场。B6 已采纳（2026-11-15）。B10 已决（2026-11-15，底色圆维持隐藏）。
- 38.2 依赖 37 的用户结论（docs/19 §47 #3）。
- 39 无用户门禁；[19] 仅在任务 36 触发时执行。

与 design 的偏离或冲突（请用户确认）：
1. design.md 全文没有 [19] 的定义（grep 无命中）。本稿只写其触发条件，内容待定义。
2. design.md 未命名 38.1 与 38.2。38.1 按用户指定为 docs/06；38.2 合并旧 24.1–24.3，为 docs/19 的批次记录与 §0、§47 更新。38.3–38.5 与 design 的命名一致。

未决（不阻塞本批任务）：
- 任务 36 是否触发，取决于 33.3 的判定。37 的 clip 项与 39 的 [19] 项都依赖此结果。
- `.efc` 与 MANIFEST 是否入库、原始 GIF 是否入库（design 用户事项 a、b）仍未决。许可已裁决，但不决定这两项。
- 38.2 沿用旧 24.1–24.3 的条目（E0b、E5、E6，docs/19 §0 的 T36、T44、T45、T48，§47 #11），本次未读取 docs/19 核对。

- [ ] 37. 表情眼见确认（需用户在场）
  - 用户按 RST 键 ≥3 次、拔插 USB 冷上电 ≥2 次。与任务 32 中 `reboot_soak.py 10 45 --face` 的 10 次 RTS 复位合计 ≥10 次，每次 splash 结束后都立即出现持续推进的表情。
  - 用户确认：splash 无明显卡顿；表情铺满 460×460 圆屏、构图居中、无中央小矩形、无播放器画布底色外露；面板滑出与触摸手势响应正常。
  - 向用户展示测试内置 4 段的画面，并与板上照片对比。候选池其余片段不在本次检查范围。
  - B6 已采纳（2026-11-15）。本项只检查板上实际显示的 4 段，以及未内置片段的回退行为。
  - B10 已决：底色圆维持隐藏（2026-11-15）。本项不再决定 B10。
  - 若任务 36 触发并新增了片段，一并确认该片段的观感；未新增时本项跳过。
  - 用户结论（可附照片或文字描述）存 `evidence/37/`，有疑问时询问用户。
  - _Requirements: 2.7, 2.8, 2.9, 3.9_

- [ ] 38. 更新文档（docs/06、docs/19、docs/09）
  - _Requirements: 1.2, 1.6, 2.5, 2.9, 2.10, 2.11, 2.13, 3.13_
  - [ ] 38.1 docs/06：§0 工具列表、§1 分层图与 §9.5 的 EFC 管线
    - §9.5：改写缩放限制与 460 素材说明为 EFC 管线（PC 侧等比缩放到 460×460、100 ms 采样、键控、tile 4、zlib、PRED、`lv_efc` 播放）。删去 EAF 的 alpha/opaque 管线与 lint 描述，保留内对齐 CENTER 的说明。
    - §0 tools 列表加入 `efc_build.py`、`efc_verify.py`（替换 `gif2efc.py`）、`serial_excl.py`、`liveness_watch.py`、`test_freeze_watch.py`、`face_semantics_check.py`、`legal_ops_check.py` 与 `tools/testdata/`。
    - §1 分层图复核已含 `sysdiag`，并与 `tools/check_layers.py` 的 `LAYERS` 登记 `"sysdiag": 0` 一致（任务 17.1）。
    - _Requirements: 2.9_
  - [ ] 38.2 docs/19：批次执行记录、§0 进度表与 §47 开放问题
    - 批次记录接在 §48 之后，内容包括：观察窗 3 复核结论（主机诱发 ROM 下载模式）；E0/E0b/E2/E3/E5/E6 与条件 E4 结果；注入矩阵汇总；浸泡汇总（有效时长、空闲时长、事件分类、人工复位 0）；表情修复（键控 EFC 替代 alpha 全为 0 的 EAF 素材、等比适配、splash 暂停、去重复解码、热载预检）；帧耗时数据（片段、场景、n、D/R/F/T 的 P50/P95/max、cputop）；决策规则结论；资源对比（HP 内部堆、最大块、RTCRAM、`.bss`）；任务 37 的用户眼见结论；与 design.md 的偏差（百分位函数改放 L5 纯 C、`freeze_watch` 命令中继）。
    - §0 进度表：将"(附加)EAF 表情 bring-up"行改为 EFC 路线（EAF 已退役）；更新 T36（460 性能回归结论）、T44（正式基线数据）、T45（现场保留路线改为 sysdiag，删去"证伪→JTAG"）、T48（冻结阻塞解除条件）及"需用户配合项"。
    - §47 开放问题：更新 #1 至 #4 的现状、证据与下一步；#3 记录任务 37 的用户结论；#11 改为 JTAG 不再是冻结根治的必经路径（保留为可选调试手段）。表格仍为唯一权威汇总。
    - _Requirements: 2.5, 2.9, 2.10, 2.11_
  - [ ] 38.3 勘误 §40、§42、§44
    - 保留原文，按文档既有格式追加"勘误（日期，批次号）"注，指向任务 38.2 的批次记录。
    - §40 第 5 条与 §42 第 3 条的"硬件级自停确认""JTAG 为唯一根治路径"撤回。依据为 `/tmp/freeze_watch.log` 末尾的 ROM 下载模式行，以及 IDF `components/esp_system/int_wdt.c` 的 `reconfigure_ticks()` 中 stage1 的 `WDT_STAGE_ACTION_RESET_SYSTEM`。
    - §44 的"LVGL9 对 RAW 不能缩放"归因不成立：`lv_eaf.c` 与 `lv_efc.c` 都发布 RGB565/RGB565A8 图像，EAF 黑屏的直接原因是素材 alpha，不是缩放。该句改为"未证实，现设计不依赖 LVGL 变换"，依据为 `lv_eaf.c` 与 LVGL 9.6 的 `src/draw/sw/lv_draw_sw_transform.c`。
    - _Requirements: 1.2, 1.6_
  - [ ] 38.4 勘误 §46 相关句
    - 保留原文，追加勘误注，指向任务 38.2 的批次记录。
    - §46 的"逐帧解码阻塞致黑屏"改为：黑屏根因为素材 alpha 全为 0（整帧透明，证据为 `eaf_codec.py`、`eaf_has_alpha_palette()`、`esp_eaf_block_decode()`）；卡顿主因为 splash 期间定时器仍在 LVGL 任务内逐帧解码。
    - _Requirements: 1.2, 1.6_
  - [ ] 38.5 docs/09：第 16 行与第 43 行改写 SD 规划与 flash 口径
    - 第 16 行与第 43 行改为：候选池 23 段（键控，合计 206,846 字节，最大 44,565 字节，`asleep`），测试内置 4 段（合计 29,641 字节）；SD 只承载用户覆盖素材（`face/idle.efc` 与 `face/<name>.efc` 热载）。
    - 原规划的 flash 余量约 0.8 MB 标为规划口径，以实际镜像复核（对照 `idf.py size` 与 MANIFEST，B7）。
    - 注明不透明版（1,149,908 字节）超出该余量；若用户改选，需另行决定。
    - _Requirements: 2.13, 3.13_

### 第二部分：冻结线（#1）

- [x] 11. 编写主机侧假象 Bug Condition 探索测试
  - **Property 1: Bug Condition** - 主机诱发下载模式被误判为静默冻结、串口可被多进程争用
  - **CRITICAL**: 该测试在未修复代码上必须失败，失败即证实缺陷存在
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: 测试编码的是期望行为（设计 Property 1 的工具部分），任务 13.5 复跑同一测试验证修复
  - **GOAL**: 证实 §40 观察窗 3 的"冻结"是主机诱发的 ROM 下载模式，且现有工具允许第二个进程打开同一端口
  - **Scoped PBT Approach**: 重放用确定性夹具；分类器用随机日志流（固定种子 ≥1000 例）
  - Bug Condition：`X.host_event ∈ {DTR_RTS_DOWNLOAD_RESET, PORT_CONTENTION, BRIDGE_STALL}`
  - 期望行为：`tools_classify(X.serial_log) = HOST_INDUCED` 且 `port_open_by_others = EBUSY`
  - 新建 `tools/test_freeze_watch.py`：
    - 重放：`freeze_watch.classify_stream()` 读入 `tools/testdata/freeze_win3_tail.log`，须给出 `HOST_DOWNLOAD_MODE`，且不得给出 `SILENT_STOP`
    - 分类 PBT：在随机正常日志流中插入 ROM 下载模式行、ROM 复位行（如 `rst:0x7`、`rst:0xc`）、panic + Backtrace、≥15s 静默、`sysdiag:` 启动报告片段 → 事件类别正确；ROM 行与下载模式永不被判为 `SILENT_STOP`
    - 独占：用 `os.openpty()` 的从端路径，经 `serial_excl.open_exclusive()` 打开后，第二次打开须得到 EBUSY
  - 在 `tools/host_tests.sh` 追加 `[附] freeze_watch selftest`（`python3 tools/test_freeze_watch.py`）
  - 板上具体用例（旧工具）：后台运行旧版 `freeze_watch.py 300`，窗口内执行 `python3 tools/p4_cli.py help`，同时 `journalctl -k -f`（无权限时用 `sudo` 或 `dmesg -w`）记录 CH343（`1a86:`）事件；记录第二次打开是否成功、第一个进程日志中是否出现 ROM 复位行；若板子进入下载模式，用 RTS 脉冲（DTR 保持释放）复位恢复
  - Run test on UNFIXED code
  - **EXPECTED OUTCOME**: 测试失败（`classify_stream`、`serial_excl` 不存在；旧报告 `caught=silence-freeze` 对应的日志末尾是 `boot:0x307` 与 `waiting for download`）
  - 证据：`evidence/11/` 下测试输出、板上两进程日志、USB 事件日志、反例摘要
  - _Requirements: 1.1, 1.4, 2.5_

- [x] 12. 编写串口工具 Preservation 测试（BEFORE implementing fix）
  - **Property 2: Preservation** - 串口工具既有行为
  - **IMPORTANT**: 遵循 observation-first 方法
  - 观察旧工具（¬C：无主机端口争用、无下载模式）：`python3 tools/p4_cli.py help`、`cputop`、`baseline 5` 的输出格式；`python3 tools/reboot_soak.py 5 45` 每轮 RTS 复位、`got_ip=True`、报告字段（`got_ip/abort/assert/guru/hang/err`）；旧 `freeze_watch.py` 的 panic 判定（`PANIC_PAT` 任一匹配最近 4000 字符）与运行态标记（`alive`、`mixer: health`、`lv_efc`、`got ip`）。输出原文存 `evidence/12/`
  - 在 `tools/test_freeze_watch.py` 新增 `preserve` 组：把当前 panic 判定规则原样复制为预言机；对随机日志流（不含 ROM 行与下载模式行，≥1000 例），被测实现的 PANIC 判定须与预言机逐例一致。F 上以预言机自身作被测实现运行以建立基线，F' 上换成新分类器
  - Run tests on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS（确认待保持的基线行为）
  - _Requirements: 3.4, 3.17_

- [x] 13. 消除主机侧假象（串口独占、监听器分类、命令中继、存活旁证工具）
  - [x] 13.1 新建 `tools/serial_excl.py`
    - `open_exclusive(port, baud, dtr=False, rts=False)`：先构造未打开的 `serial.Serial` 并设好 `dtr`/`rts`，再 `open()`，随后 `fcntl.ioctl(fd, termios.TIOCEXCL)`；端口被占时报 EBUSY 并提示用 `fuser -v` 查看占用者
    - _Bug_Condition: X.host_event = PORT_CONTENTION_
    - _Expected_Behavior: port_open_by_others = EBUSY_
    - _Preservation: Property 2 工具行为（任务 12）_
    - _Requirements: 2.5, 3.17_
  - [x] 13.2 迁移现有串口工具
    - `p4_cli.py`、`reboot_soak.py`（仍按原逻辑用 RTS 复位）、`p4_boot_capture.py`、`serial_log.py` 改用 `serial_excl`
    - `p4_cli.py` 遇 EBUSY 且 `freeze_watch` 命令中继存在时，自动改走中继发送，并从 `freeze_watch` 实时日志截取该命令之后的输出（注入与浸泡期间端口由 `freeze_watch` 独占，design.md 的 `p4_cli.py "inject …"` 写法依赖这一点）
    - _Requirements: 3.4, 3.17_
  - [x] 13.3 重写 `tools/freeze_watch.py`
    - 整个浸泡期间端口只打开一次，不再每窗重开；只在 `SerialException`（设备消失）时重开并记 `USB_REENUM` 事件；启动时设备不存在则轮询等待其出现（供冷上电抓取）
    - `classify_stream()` 事件类别：`PANIC`、`ROM_RESET(<rst 文本>)`、`HOST_DOWNLOAD_MODE`（`boot:0x3..(DOWNLOAD` 或 `waiting for download`）、`SILENT_STOP`（≥15s 无输出且无 ROM 行）、`AUTO_RECOVERED`（随后出现 `sysdiag:` 启动报告）；静默不退出、持续记录
    - 解析 `sysdiag:` 行与 ROM 横幅中的 `Saved PC`，给出 `riscv32-esp-elf-addr2line -pfiaC -e firmware/build-p4c5/esp_cube.elf <pc>` 结果
    - 命令中继：命名管道 `/tmp/freeze_watch.cmd`，写入的每行原样发往串口并记 `CMD` 事件（含发送时间戳，供计算注入到复位的间隔）；伪命令 `!rst` 发 RTS 脉冲（等同 RST 键）
    - 输出：窗口日志 `/tmp/freeze_watch_<时间戳>.log`、事件 `/tmp/freeze_watch_events.jsonl`、汇总 JSON（累计有效观察时长、各类事件数；含 `HOST_DOWNLOAD_MODE` 的窗口作废、不计时长）；`--out <目录>` 直接写入 `evidence/`；`--replay <log>` 离线分类
    - `--usb-events`：旁路运行 `journalctl -k -f`，过滤 CH343（`1a86:`）与 `ttyACM` 事件写入 `usb_events.log`
    - _Bug_Condition: X.host_event ∈ {DTR_RTS_DOWNLOAD_RESET, PORT_CONTENTION, BRIDGE_STALL}_
    - _Expected_Behavior: tools_classify(X.serial_log) = HOST_INDUCED；ROM 行永不判为 SILENT_STOP_
    - _Preservation: PANIC 判定与预言机一致（任务 12）_
    - _Requirements: 1.1, 1.4, 2.5_
  - [x] 13.4 新建 `tools/liveness_watch.py`
    - 每 2s `ping -c1 -W1 <设备IP>`，写 `/tmp/liveness.jsonl`（时间戳、RTT 或超时）；可选 `--mgmt` 用现有 mTLS 客户端证书调用管理通道只读端点作第二旁证，不新增未鉴权端点
    - _Requirements: 2.4, 3.3_
  - [x] 13.5 验证主机侧探索测试现已通过
    - **Property 1: Expected Behavior** - 主机诱发事件被正确归类且端口独占
    - **IMPORTANT**: 复跑任务 11 的同一组测试与同一板上用例，不要另写新测试
    - `python3 tools/test_freeze_watch.py` 全部通过；`python3 tools/freeze_watch.py --replay tools/testdata/freeze_win3_tail.log`（evidence/1.1 原文件已丢失，见 evidence/1.1/README，2026-11-16 更正）给出 `HOST_DOWNLOAD_MODE`；板上：新版 `freeze_watch.py` 运行时直接打开端口的第二进程得到 EBUSY，板子未复位，`p4_cli.py help` 经中继正常返回
    - **EXPECTED OUTCOME**: Tests PASS（确认主机侧假象已消除）
    - _Requirements: 1.1, 2.5_
  - [x] 13.6 验证串口工具保持
    - **Property 2: Preservation** - 串口工具既有行为
    - **IMPORTANT**: 复跑任务 12 的同一组测试，不要另写新测试
    - `preserve` 组新分类器与预言机逐例一致；`p4_cli.py help`/`cputop`/`baseline 5` 输出格式与 `evidence/12/` 一致；`reboot_soak.py 5 45` 每轮 `got_ip=True`、报告字段不变
    - **EXPECTED OUTCOME**: Tests PASS（确认无回归）
    - _Requirements: 3.4, 3.17_
  - [x] 13.7 p4_cli.py 读取增强（等待 CLI 就绪，空闲结束读取）
    - 目的：任务 13.6 中 `baseline 5` 的表格抓不到。读取窗口在回显后约 0.25 秒结束，而表格约 2.5 秒后才打印。另外，每次打开串口都会复位板子（任务 1.5、13.5 实测），命令发出时板子可能仍在启动。
    - 新增 `--ready-wait S`（默认 30）：发送命令前，每秒发一次 `help`，直到收到 CLI 回复；超时则报错退出，不发送命令。
    - 新增 `--settle S`（默认 2.0）与 `--max S`（默认 15）：发送命令后持续读取，直到连续 S 秒没有新字节，或达到 max 秒。
    - 保持不变：`help`、`cputop`、`baseline` 的输出格式；EBUSY 时的中继路径改用同样的 settle 读取；用法文本只在末尾追加新选项说明，不改已有行。
    - 验证（宿主）：pty 上的读取测试（设备 2.5 秒后输出表格，能完整抓到）；`py_compile`。
    - 验证（板上，需用户在场）：`python3 tools/p4_cli.py baseline 5` 抓到表格（三行：internal free、largest block、psram free）；`help`、`cputop` 输出与 evidence/13.6/ 一致；`reboot_soak.py` 不受影响。
    - 产物：evidence/13.7/。
    - _Requirements: 3.17_

- [ ] 14. 实验 E0：LP SRAM 保留区跨复位存活性（最小探针，sysdiag 之前） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - [ ] 14.1 临时探针 `firmware/main/e0_probe.c`
    - `RTC_NOINIT_ATTR` 16B 记录 `{magic, seq, ~seq, crc32}`；`app_main()` 首行调用，输出 `e0: raw0=0x.. raw1=0x.. hint=<esp_reset_reason> rec=valid|invalid seq=N`，然后 `seq+1` 回写
    - 每次启动把 `(raw0, hint, rec_valid, seq)` 追加进 NVS 环（最近 16 条），CLI `e0 hist` 打印。冷上电时主机打开端口的 DTR 毛刺可能紧跟着再复位一次，导致冷启动那一行被漏抓，NVS 历史可补回
    - CLI 追加 `reboot`（`esp_restart()`，长期保留）
    - 探针在任务 17.5 删除；除 16B 保留区外不新增静态缓冲
    - _Requirements: 1.3, 2.3_
  - [ ] 14.2 构建、烧录并确认版本（按执行约定）
    - _Requirements: 3.18_
  - [ ] 14.3 EN 复位与软件重启
    - 经 `serial_excl` 做 RTS 脉冲复位 ×5（与 RST 键同一 EN 电路）、CLI `reboot` ×5；逐次记录 `e0:` 行与 ROM 行（`rst:` 码、是否带 `Saved PC`）
    - 证据：`evidence/14/` 下复位类型 × 记录有效性对照表、`e0 hist` 原文
    - _Requirements: 1.3, 2.3_

- [ ] 15. 检查点（需用户在场）：E0 冷上电 （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - 执行者先启动等待设备出现的抓取（新版 `freeze_watch.py --out evidence/15`），请用户拔下 USB ≥5s 后插回，重复 2 次；随后执行 `e0 hist`
  - 预期：冷上电那次启动 `rec=invalid`。结合 14.3 判定 EN 复位后保留区是否存活：存活则启动报告可区分"人工 RST/串口 DTR-RTS（EN_RESET）"与"冷上电"；不存活则启动报告只能标注"上电或 EN 复位（POWERON_OR_EN）"
  - 不存活时向用户说明这一硬件限制并请其确认接受；结论写入 `evidence/15/conclusion.md`，供任务 16、17 的分类表措辞使用
  - _Requirements: 2.3_

- [ ] 16. 编写 sysdiag Bug Condition 探索测试（宿主，套件 [15]–[17]） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - **Property 1: Bug Condition** - 复位来源不可归类、无现场记录、业务停滞不可检出、日志生产者会阻塞
  - **CRITICAL**: 该测试在未修复代码上必须失败，失败即证实缺陷存在
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: 三个套件编码设计 Property 5/6/7 的期望行为，sysdiag 实现后同一测试通过即验证修复
  - **GOAL**: 把疑点 (a)（TWDT 只看 idle）、`esp_reset_reason()` 合并硬件看门狗、同步日志阻塞写者这三类缺陷固化为可执行反例
  - **Scoped PBT Approach**: 每个测试文件含一个 F 模型对照组（编译开关 `-DSYSDIAG_F_MODEL_ONLY` 可单独运行），用具体轨迹展示 F 的检出失败；主组用随机轨迹（xorshift 固定种子 ≥1000 例）对 `sysdiag_core` 断言期望行为
  - Bug Condition：`isBugCondition_freeze(X) ∧ X.host_event = NONE`，覆盖 `IRQ_OFF_BOTH`、`BUS_STALL`（F 会复位但不报告来源、不留现场）、`idle_running ∧ {KEY_TASK_STALL, LOG_PATH_BLOCKED}`
  - 新建 `firmware/components/sysdiag/include/sysdiag_core.h`（纯 C 契约：记录布局与编解码/校验、`classify()`、ms → RWDT tick 换算、监督策略单步函数、LP WDT 武装决策、日志环 API），只写声明
  - `firmware/components/sysdiag/test/test_sysdiag_record.c`（[15]，Property 5）：随机记录 → 随机位翻转、截断、全随机字节 → `validate()` 判无效或字段逐字节相等；随机（原始码 0–0x3F × 提示 × 记录状态 × 注入标记）→ 类别确定且唯一、输出串只来自固定词表与数值字段；分类表逐行用例（`CHIP_POWER_ON` 行的措辞按 `evidence/15/conclusion.md`）；ms → tick 换算含 efuse `WDT_DELAY_SEL` 移位语义的上下界。F 模型：只用 `esp_reset_reason()` 分类，MWDT/RWDT/Super WDT 全部合并为 `ESP_RST_WDT`，唯一性被破坏
  - `firmware/components/sysdiag/test/test_sysdiag_supervisor.c`（[16]，Property 6）：随机心跳轨迹（抖动 ≤阈值、busy 窗口、≤0.5s flash 停顿、10s 启动宽限）→ 每周期都喂 LP WDT；随机 t₀ 处切断 UI/MIXER/LOGTX → 触发时刻 ∈ [t₀+阈值, t₀+阈值+1s]，含 LOGTX 两段式（15s 直写探针 + 10s）总上界 ≤30s；切断 CLI/NET → 不触发；调试器附着时 LP WDT 武装决策为否（3.19）。F 模型：TWDT 只订阅 IDLE，UI/MIXER 停滞而 idle 正常时永不检出
  - `firmware/components/sysdiag/test/test_sysdiag_logring.c`（[17]，Property 7）：单线程随机交错模拟多生产者与随机停顿或永久阻塞的消费者 → 生产调用步数有上界、输出是生产序列去掉丢弃项后的有序子序列、丢弃计数精确、无撕裂行、超长行截断并带标记；另加 pthread 压力用例（`-pthread`）。F 模型：同步阻塞写，消费者永久阻塞时生产者无限等待
  - 在 `tools/host_tests.sh` 追加 `[15/18]`、`[16/18]`、`[17/18]`（gcc + ASan/UBSan，链接 `sysdiag_core.c`；构建失败与运行失败分开判定）
  - Run test on UNFIXED code
  - **EXPECTED OUTCOME**: [15]–[17] 构建失败（`sysdiag_core.c` 不存在）；`-DSYSDIAG_F_MODEL_ONLY` 单独运行三个 F 模型组，打印具体反例轨迹
  - 证据：`evidence/16/` 下构建失败输出与三条反例轨迹
  - _Requirements: 1.2, 1.3, 1.4, 2.1, 2.2, 2.3, 2.4, 3.6, 3.15, 3.19_

- [ ] 17. sysdiag 阶段 A：现场记录、启动报告、心跳与注入（监督器只记录不复位，常驻 LP WDT 不武装） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - [ ] 17.1 组件骨架与分层登记
    - 先改 docs/06 §1 分层图加入 `sysdiag`（L0），再在 `tools/check_layers.py` 的 `LAYERS` 加 `"sysdiag": 0`（该文件注释要求先改文档再改层表）
    - 新建 `firmware/components/sysdiag/`：`include/sysdiag.h`、`src/sysdiag_core.c`、`src/sysdiag_esp.c`、`Kconfig`、`CMakeLists.txt`
    - Kconfig：`ESP_CUBE_SYSDIAG`（`default y if IDF_TARGET_ESP32P4`）、各阈值、`ESP_CUBE_SYSDIAG_ENFORCE`（监督触发与常驻 LP WDT 总开关，阶段 A 默认 n）、`ESP_CUBE_SYSDIAG_ASYNC_LOG`（阶段 A 默认 n）、`ESP_CUBE_SYSDIAG_INJECT`（开发默认 y）
    - _Requirements: 3.2_
  - [ ] 17.2 `sysdiag_core.c`：记录编解码、分类、换算与监督策略
    - 记录：magic `'RSTD'`、`ver`、`size`、`boot_seq`/`boot_seq_inv`、头 CRC32；live 区（每核 tick 计数/last_ms/任务名前 8 字节，UI/MIXER/CLI/NET/LOGTX/SUPV 的 last_ms/site/state，日志计数，`con_pin_ok`、`usj_flips`，`trigger`/`trigger_ms`/`inject_kind`）；sealed 区（live 快照 + `uptime_ms` + `epoch_s` + 心跳摘要 + CRC32）；全部字段为对齐 32 位存储，总大小 ≤512B
    - 分类表按 design.md"启动报告与分类"逐行实现；原始码用自有数值常量，`sysdiag_esp.c` 以 `_Static_assert` 对照 IDF `soc/reset_reasons.h` 的 esp32p4 枚举
    - 监督策略单步函数与 LP WDT 武装决策（含 `esp_cpu_dbgr_is_attached()` 输入）
    - 运行 [15]、[16]：应 PASS（[17] 待阶段 B）
    - _Bug_Condition: F 不报告、不归类复位来源，无跨复位现场（1.3）_
    - _Expected_Behavior: 设计 Property 5、Property 6_
    - _Preservation: 输出只含固定词表与数值，不含自由文本（3.15）_
    - _Requirements: 1.3, 2.2, 2.3, 3.15, 3.19_
  - [ ] 17.3 `sysdiag_esp.c`：设备胶水（观测模式）
    - `RTC_NOINIT_ATTR` 记录；`esp_register_freertos_tick_hook_for_cpu` 每核每 8 tick 写一次 live 区
    - `sysdiag_hb()`、`sysdiag_busy()`/`sysdiag_idle()`、`sysdiag_register_probe()`；1s 周期 `esp_timer` 监督器（复用 esp_timer 任务，不新增任务栈）：读心跳年龄、日志计数、GPIO37/38 的 IO_MUX 与矩阵选择 + UART0 时钟分频签名、`usb_serial_jtag_is_connected()` 翻转；每 1s 封存记录、每 10s 更新心跳摘要；`ENFORCE=n` 时只记录"本应触发"计数与原因，不复位
    - `sysdiag_early_init()` 只初始化记录；LP WDT HAL 封装（`wdt_hal_init(&ctx, WDT_RWDT, 0, false)`，tick 按 `rtc_clk_slow_freq_get_hz()` 换算）供注入命令临时武装
    - _Requirements: 1.4, 2.3, 2.4_
  - [ ] 17.4 启动报告
    - `sysdiag_boot_report()` 在 `nvs_flash_init()` 之后、其余初始化之前执行：输出 `sysdiag: fw=… elf=… built=…`、`boot#… reset=<类别> raw0=… raw1=… abn_total=…`、上次运行时长与 trigger/inject、两核 tick 与任务名、各任务心跳、日志计数、心跳摘要（格式按 design.md 示例）
    - 记录无效时只输出 `sysdiag: 无有效现场记录(冷上电或校验失败)`；异常复位时把累计计数写 NVS（命名空间 `sysdiag`，键 `abn`/`last_cls`/`last_up`），只在异常启动时写一次；报告文本保留一份 PSRAM 副本供 `rstinfo`；报告后记录重置为本次启动
    - _Requirements: 1.3, 2.3, 3.15, 3.18_
  - [ ] 17.5 心跳接线与删除 E0 探针
    - `main.c`：`app_main()` 首行 `sysdiag_early_init()`、`nvs_flash_init()` 后 `sysdiag_boot_report()`、`startup complete` 后 `sysdiag_start()`；注册 NET 探针（`tcpip_try_callback()` 投递只调 `sysdiag_hb(SYSDIAG_T_NET, 1)` 的回调）；不新增任何按复位原因分支的业务逻辑
    - `components/app_audio/audio_mixer.c`（`CMakeLists.txt` 加 `PRIV_REQUIRES sysdiag`）：主循环顶部 `sysdiag_hb(SYSDIAG_T_MIXER, 1)`，I2S 写后 `site=2`
    - `cli.c`：每轮 `uart_read_bytes`（200ms）上报 CLI 心跳；`handle_line()` 前后 `sysdiag_busy(SYSDIAG_T_CLI, deadline)`/`sysdiag_idle()`（`wget`/`ncm` 300s，其余 60s）
    - `ui_port_esp.c`：UI 启动后在 LVGL 锁内建 500ms `lv_timer` 调 `sysdiag_hb(SYSDIAG_T_UI, 1)`
    - `main/CMakeLists.txt` 的 `main_priv_reqs` 加 `sysdiag`；删除 `e0_probe.c` 及其调用（保留 `reboot` 命令）
    - _Requirements: 2.3, 2.4, 3.14_
  - [ ] 17.6 CLI 诊断与注入命令（仅 UART CLI）
    - `rstinfo`（重印本次启动报告）、`diag`（各心跳年龄、日志计数、LP WDT 状态、USJ 翻转、引脚签名、"本应触发"计数）、`diag corrupt`（INJECT 打开时破坏记录 CRC）
    - `inject <kind> [core] CONFIRM`（受 `ESP_CUBE_SYSDIAG_INJECT` 门控，执行前先写 `inject_kind`）：`irqoff <0|1>`、`irqoff both`、`irqoff both nowdt`（临时以 12s 武装 LP WDT 后禁用 TIMG0/TIMG1 WDT）、`stall`、`task ui`、`task mixer`、`logblock`、`spin <0|1>`，实现按 design.md 注入表
    - 检索 `pair_server.c` 与管理端点路由表，确认未注册任何注入路由
    - _Requirements: 2.6_
  - [ ] 17.7 构建、审计与烧录阶段 A 冻结诊断固件
    - 三目标构建退出码均为 0；`check_layers.py` 全绿；`host_tests.sh` 原 14 套件、[15]、[16]、`[附] freeze_watch selftest` PASS，[17] 仍按预期失败（[18] 按表情线所处阶段的预期判定）
    - `idf.py size` 与 1.2 对比：DRAM `.bss` 累计增量 ≤128B，`rtc_noinit` ≤512B，RTCRAM 区缩小量单列
    - NFR-03 审计：新增日志格式串与记录字段只含枚举、数值、任务名前缀
    - 按执行约定全量烧录，核对 `app_init: ELF file SHA256` 与 `sysdiag: fw=… elf=…` 两行；该固件即冻结线的 F
    - _Requirements: 3.1, 3.2, 3.15, 3.16, 3.18_

- [ ] 18. 编写冻结类 Bug Condition 探索测试（板上，阶段 A 固件） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - **Property 1: Bug Condition** - 现有 WDT 盲区与业务/日志停滞无自动恢复
  - **CRITICAL**: 该测试在未修复代码上必须失败，失败即证实缺陷存在
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: 用例与断言即期望行为，任务 21.1 复跑同一用例验证修复
  - **GOAL**: 确认或推翻疑点 (a)/(b)，验证 E0 在 MWDT/RWDT 复位下的保留区存活性（E0b）
  - **Scoped PBT Approach**: 确定性注入，限定到具体用例，每项 ≥3 次；全程新版 `freeze_watch.py --out evidence/18 --usb-events` 独占端口，命令经中继发送，`liveness_watch.py` 并行
  - 断言（expectedBehavior_freeze）：故障后 ≤30s 自动复位；下次启动类别与注入一致、记录有效且指向注入的核心/任务；复位后 got_ip、SNTP、`mixer: health` 出现
  - X6/E2：`inject irqoff both CONFIRM` → 预期 ≤1.5s 出现 ROM 复位行，原始码为 MWDT 类，启动报告类别 `MWDT_HW`（若不复位则推翻疑点 (b)，H2 重新上升，暂停冻结线并向用户报告）
  - `inject stall CONFIRM` → 预期同 X6
  - X7：`inject task ui CONFIRM` → 预期 30s 内无任何复位，`diag` 显示 UI 心跳年龄持续增长、idle 正常、串口照常（反例）；随后 `inject irqoff both CONFIRM` 触发 MWDT 复位，下次启动记录的 UI 心跳停在注入时刻
  - `inject task mixer CONFIRM` → 同 X7 方法
  - E6：`inject logblock CONFIRM` → 预期串口静默（CLI 的 printf 同样被 stdout 锁挡住）、ping 持续通、60s 内无复位（反例）；以 `!rst` 恢复，按 `evidence/15/conclusion.md` 选择证据：保留区跨 EN 复位存活时看记录中 `log.enter ≠ log.exit` 与 UI 心跳持续，否则以 ping 记录为准
  - X8/E0b：X6 的 MWDT 复位与 `inject irqoff both nowdt CONFIRM`（LP WDT 复位，原始码 RWDT 类）后，记录是否仍有效、ROM 横幅是否带 `Saved PC`
  - Run test on UNFIXED code
  - **EXPECTED OUTCOME**: X7、`task mixer`、E6 断言失败（无自动恢复，证实 TWDT 盲区）；X6/stall 预期出现硬件复位，作为疑点 (b) 的实证
  - 证据：`evidence/18/` 下 `freeze_watch_events.jsonl`（CMD 到 ROM 行的间隔）、启动报告原文、`diag` 截取、`liveness.jsonl`、注入结果对照表
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 2.2, 2.6_

- [ ] 19. 编写非故障输入 Preservation 测试（板上，阶段 A 固件，BEFORE implementing fix） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - **Property 2: Preservation** - 正常运行、合法长操作与现有 WDT 路径
  - **IMPORTANT**: 遵循 observation-first 方法；输入均满足 `isBugCondition_freeze(X) = false`
  - 启动与联网：`python3 tools/reboot_soak.py 30 45` 全部 `got_ip`、无 abort/assert/guru/hang；`pair_client.py` 配对正例与负例、`mgmt_client.py` 无证书返回 401；启动日志 `vol=`、电台关、安静时段行为（3.3、3.4、3.14）
  - 现有 WDT 路径：`inject spin 1 CONFIRM`、`inject irqoff 0 CONFIRM` 出现 `Task watchdog` / `Interrupt wdt timeout on CPU0` panic、双核回溯并自动重启，panic 原文存证作为格式基线（3.5）
  - 合法长操作（监督器观测模式）：新建 `tools/legal_ops_check.py --seed <n>`，以随机顺序执行 SRP 全链配对、`wget` 1MB 到 SD、`memtest` 连续写 50 条、`sdretry`、`uimood` 切换 100 次、`face` 热载 20 次、断开 AP 触发一次 WiFi 重连；每步后读 `diag`，"本应触发"计数须为 0（3.6）。若出现本应触发，记录原因与时长，在阶段 B 前调整阈值并重跑
  - 写入期间复位：`memtest` 循环写入中经中继 `!rst` ×10，每次启动后 `memstat` 读回正常（3.7）
  - 资源与工具：`baseline 20`、`heapstat`、`cputop`、`idf.py size` 与 1.2/1.3 对比；`p4_cli.py help` 正常（3.16、3.17）；启动版本行（3.18）；GPIO24/25 未占用（3.19）
  - Run tests on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS（确认待保持的基线行为）
  - 证据：`evidence/19/` 下 soak 报告 JSON、panic 原文、`legal_ops_check` 报告、`memstat` 读回、资源对比表
  - _Requirements: 3.3, 3.4, 3.5, 3.6, 3.7, 3.14, 3.16, 3.17, 3.18, 3.19_

- [ ] 20. 冻结修复（阶段 B：非阻塞日志 + 常驻 LP WDT + 监督器触发） （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - [ ] 20.1 日志环核心（`sysdiag_core.c`）
    - 64 槽 × 256B 槽环：生产者原子递增取槽，槽未消费即丢弃并计数，永不等待；超长行截断并标记
    - 运行 [17]：应 PASS
    - _Bug_Condition: idle_running ∧ fault = LOG_PATH_BLOCKED；console_path = LOCK_HELD_BLOCKED_
    - _Expected_Behavior: 设计 Property 7；故障期间日志生产者阻塞 ≤1ms_
    - _Preservation: 输出为有序子序列、无撕裂行（3.17）_
    - _Requirements: 2.4, 3.17_
  - [ ] 20.2 logtx 与 vprintf 钩子（`ESP_CUBE_SYSDIAG_ASYNC_LOG`，P4 默认改为 y）
    - `esp_log_set_vprintf()` 钩子把每条日志 `vsnprintf` 进槽环；槽环用 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`（16KB）
    - `logtx` 任务：`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`，栈 3KB，优先级 3，core0；按槽整行 `fwrite(stdout)` + `fflush`，每写满约 115 字节 `vTaskDelay(10ms)`；记录单行写出耗时直方图（供 E5）与 LOGTX 心跳
    - CLI 的 `printf` 保持同步直写；panic 输出走 ROM 直写不受影响
    - _Bug_Condition: 日志通路阻塞导致所有记日志的任务依次阻塞（H1a）_
    - _Expected_Behavior: 任一控制台通路不可写时日志输出不阻塞其他功能（2.4）_
    - _Preservation: CLI 交互即时、`cputop`/`baseline` 输出不变（3.17）_
    - _Requirements: 2.4, 3.17_
  - [ ] 20.3 常驻 LP WDT
    - `sysdiag_early_init()` 武装启动保护（stage0 60s `WDT_STAGE_ACTION_RESET_SYSTEM`）；`sysdiag_start()` 改为 stage0 12s `RESET_SYSTEM`、stage1 再 4s `RESET_RTC`，监督器每 1s 在全部对象健康时 `wdt_hal_feed()`
    - `esp_cpu_dbgr_is_attached()` 为真时不武装并打印一行提示；不改 `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE`
    - _Bug_Condition: fault ∈ {CPU_BOTH_STOPPED, HP_CLOCK_STALL}（H3）_
    - _Expected_Behavior: HP 全停 ≤13s 复位，复位码为 RWDT 类_
    - _Preservation: 3.6 合法长操作不误复位；3.19 调试器附着时不武装_
    - _Requirements: 2.1, 2.2, 3.6, 3.19_
  - [ ] 20.4 监督器触发（`ESP_CUBE_SYSDIAG_ENFORCE`，P4 默认改为 y）
    - UI 10s → `trigger=UI_STALL` → `esp_restart()`；MIXER 5s → `MIXER_STALL`；LOGTX 有待发数据但 15s 无进展 → 经 `esp_rom_output_putc` 直写 UART0 一行 `[sysdiag] console stall`，再 10s 仍无进展 → `LOG_STALL`；CLI、NET 只记录；`sysdiag_start()` 后 10s 宽限；重启路径卡住时由 LP WDT 12s 兜底
    - 构建后确认 `build-p4c5/config/sdkconfig.h` 中 `CONFIG_ESP_CUBE_SYSDIAG_ENFORCE 1` 与 `CONFIG_ESP_CUBE_SYSDIAG_ASYNC_LOG 1`；若本地 sdkconfig 沿用阶段 A 写入的旧值，按执行约定原地改该行（不复制文件）
    - _Bug_Condition: idle_running ∧ fault ∈ {KEY_TASK_STALL, LOG_PATH_BLOCKED}_
    - _Expected_Behavior: expectedBehavior_freeze：UI ≤12s、LOGTX ≤26s 复位，类别 SUPERVISOR(...)，记录指向停滞任务_
    - _Preservation: 3.6 合法长操作零触发；3.14 不按复位原因分支业务行为_
    - _Requirements: 2.1, 2.2, 2.6, 3.6, 3.14_
  - [ ] 20.5 验证宿主 sysdiag 探索测试现已通过
    - **Property 1: Expected Behavior** - 复位分类唯一、监督限时检出、日志环非阻塞
    - **IMPORTANT**: 复跑任务 16 的同一组测试，不要另写新测试
    - `host_tests.sh` 的 [15]、[16]、[17] 全部 PASS
    - **EXPECTED OUTCOME**: Tests PASS（确认宿主侧缺陷已修复）
    - _Requirements: 2.1, 2.2, 2.3, 2.4_
  - [ ] 20.6 构建、审计与烧录 F'
    - 三目标构建退出码均为 0；`check_layers.py` 全绿；`host_tests.sh` 原 14 套件、[15]–[17]、`[附] freeze_watch selftest` PASS（[18] 按表情线所处阶段的预期判定，两线完成后由任务 39 全量确认）；`idf.py size` 对比 1.2（`.bss` 累计增量 ≤128B）；NFR-03 审计新增格式串；检索确认业务代码不读取复位类别
    - 按执行约定全量烧录并核对两行版本标识
    - _Requirements: 3.1, 3.2, 3.14, 3.15, 3.16, 3.18_

- [ ] 21. 冻结修复板上 Fix Checking 与 Preservation Checking （条件任务：仅在 22.1 复现冻结后执行，未复现则不实施）
  - [ ] 21.1 验证注入矩阵现已自动恢复
    - **Property 1: Expected Behavior** - 冻结类故障 ≤30s 自动复位且现场可辨识
    - **IMPORTANT**: 复跑任务 18 的同一组注入用例，不要另写新测试
    - 每项 ≥3 次，`freeze_watch.py --out evidence/21 --usb-events` 常驻、命令经中继、`liveness_watch.py` 并行。预期类别与上界：`irqoff 0/1` → `INT_WDT`（panic）；`irqoff both`、`stall` → `MWDT_HW`（≤1.5s）；`irqoff both nowdt` → `LP_WDT`（≤13s）；`task ui` → `SUPERVISOR(UI_STALL)`（≤12s）；`task mixer` → `SUPERVISOR(MIXER_STALL)`（≤7s）；`logblock` → `SUPERVISOR(LOG_STALL)`（≤26s，主机可见 `[sysdiag] console stall` 直写行）；`spin 0/1` → `TASK_WDT`
    - 每次启动报告类别后带 `(inject=<kind>)`，记录指向注入的核心/任务，`abn_total` 递增；复位后 got_ip、SNTP、`mixer: health` 出现
    - **EXPECTED OUTCOME**: Tests PASS（确认 1.1–1.3 已修复）
    - 证据：`evidence/21/` 下事件 JSONL（CMD 到 ROM 行间隔）、每次启动报告原文、注入矩阵汇总表
    - _Requirements: 2.1, 2.2, 2.3, 2.6_
  - [ ] 21.2 启动报告与存活旁证
    - 启动报告：RTS 复位、`reboot`、每种注入各一次的 `sysdiag:` 行原文；`diag corrupt` 后重启显示"无有效现场记录"
    - 旁证（E6 复跑）：`inject logblock CONFIRM` 期间 ping 全程通；复位后记录显示 UI/mixer 心跳持续到复位前而 logtx 停滞、`drops` 增长；≤26s 复位
    - E0b 复核：RWDT `RESET_SYSTEM` 后记录有效性与 `Saved PC` 保留情况；有 `Saved PC` 时 `freeze_watch` 输出 addr2line 结果
    - _Requirements: 2.3, 2.4_
  - [ ] 21.3 合法长操作无误复位与写入期间复位
    - 复跑 `tools/legal_ops_check.py`（任务 19 的同一组种子）：全程 `diag` 无触发、无复位
    - `memtest` 循环写入时注入 `inject irqoff both nowdt CONFIRM` ×10：每次启动后 `memstat` 读回正常；宿主 [4/14] 双槽协议全绿
    - 每次自动复位后核对电台关、`vol=` 与人工复位后一致
    - _Requirements: 2.1, 3.6, 3.7, 3.14_
  - [ ] 21.4 验证非故障输入行为保持
    - **Property 2: Preservation** - 正常运行、合法长操作与现有 WDT 路径
    - **IMPORTANT**: 复跑任务 19 的同一组测试，不要另写新测试
    - `reboot_soak.py 30 45` 全部 `got_ip`、无 abort/assert/guru/hang；`spin 1`/`irqoff 0` 的 panic 格式与 `evidence/19/` 一致（F' 额外打印 sysdiag 启动报告）；配对正负例与 401；`baseline 20`/`heapstat`/`cputop`/`idf.py size` 对比表（HP 内部堆 min/P50、最大连续块不低于基线，RTCRAM 区单列）；`p4_cli.py help`；版本行；GPIO24/25 未占用、调试器附着分支由 [16] 覆盖（OpenOCD 实测已由任务 1.5 完成（方案 A））
    - **EXPECTED OUTCOME**: Tests PASS（确认无回归）
    - _Requirements: 3.3, 3.4, 3.5, 3.16, 3.17, 3.18, 3.19_

- [ ] 22. 长窗浸泡 E3（累计 ≥4h，含 ≥2h 完全空闲）与条件实验 E4/E5
  - [ ] 22.1 长时浸泡（无调试器，≥4 小时，含完全空闲）
    - 固件：最终面板固件 F'（任务 31.6 烧录），不接调试器。
    - 监测：13.3 的浸泡监测与 13.4 的存活旁证同时运行，串口独占，打开时不拉 DTR/RTS，不做任何 DTR/RTS 复位。
    - 判定：静默事件数为 0 且人工复位次数为 0，记为"未复现"；出现静默，记为复现并进入条件任务 14–21 与 JTAG 抓取。
    - 依赖：31.6、13.3、13.4、1.5 完成。
    - _Requirements: 2.1, 2.4, 2.5（条件需求，仅在复现冻结后实施）_
  - [ ] 22.2 （条件任务：仅在 22.1 复现冻结后执行）E5 USJ 副控制台排查（依据浸泡数据）
    - J2 未接线时若 `usj_flips > 0` 或 logtx 单行写出出现 ≥50ms 峰：在 `sdkconfig.p4c5` 原地改为 `CONFIG_ESP_CONSOLE_SECONDARY_NONE`（改前后记录 sha256，只 diff `build-p4c5/config/sdkconfig.h` 相关项），同步写入入库的 `sdkconfig.defaults.esp32p4` 以便复现；重跑 21.4 与 ≥1h 浸泡。否则保持现状并记录数据
    - _Requirements: 2.4, 3.19, 3.20_
  - [ ] 22.3 （条件任务：仅在 22.1 复现冻结后执行）E4 PSRAM 降频对照（仅当 22.1 捕获 ≥1 次 H3 或 H2 类事件；否则记录"未触发"）
    - 记录 `sdkconfig.p4c5` sha256 → 原地把 `CONFIG_SPIRAM_SPEED_200M` 切到 `CONFIG_SPIRAM_SPEED_80M` → 构建到 `firmware/build-p4c5-psram80` → 立即改回 → 重新构建 `build-p4c5` 并确认 sha256 与改前一致；`diff build-p4c5/config/sdkconfig.h build-p4c5-psram80/config/sdkconfig.h` 只含 SPIRAM 速率项
    - 80MHz 固件浸泡时长 ≥ max(4h, 2 × 200MHz 下平均事件间隔)；事件消失则判 PSRAM 时序相关，再评估 120/160MHz 档或降频的性能代价；表情性能验收一律在 200MHz 下做
    - _Requirements: 2.1, 2.5, 3.20_

- [ ] 23. 检查点（需用户在场）：冻结结论确认（浸泡结果与是否启动条件任务）
  - 用户确认 22.1 的结论；若判定为未复现，记录为未复现并关闭冻结线；若复现，确认是否进入条件任务 14–21。
  - _Requirements: 2.3, 2.5（条件需求，仅在复现冻结后实施）_

### 第三部分：收尾

- 已退役：原任务 24（文档，由 38 取代）、原任务 25（检查点，由 39 取代）。

- [ ] 39. 表情线与冻结线全量检查点
  - 本任务的检查均未执行，以下为通过判据。
  - `tools/host_tests.sh`：原 14 套件、[15]–[17]、[18]（EFC 版，替换 EAF 版内容）、`[附] freeze_watch selftest` 与 `[附]` 分层检查全部通过。[19] 仅在任务 36 触发时加入（EFC 解码等价套件）；未触发则记为未触发。
  - [18] 的判据：`python3 tools/efc_verify.py --contract` 通过；每个 `.efc` 经 `efc_py.decode_frames` 解码后的逐帧 CRC32 与 MANIFEST 一致；PBT 每项 ≥1000 例通过；`face_efc_precheck` 与 `face_run_state` 的 ASan/UBSan 用例通过；`test_face_perf_stats` 保留并通过。
  - 三目标（S31、P4C5、sim）构建退出码均为 0；板上运行版本与最终构建一致（两行版本标识）。
  - 凭据隔离：`git check-ignore -v firmware/sdkconfig.p4c5` 有输出；`git status --porcelain --ignored firmware` 中出现未被忽略的本地 sdkconfig 派生文件（如 `sdkconfig.p4c5.old`）时补进 `.gitignore`；若执行者按项目惯例提交，提交前确认 `git diff --cached --name-only` 不含 `sdkconfig`、`sdkconfig.p4c5`、`sdkconfig.s31`、`sdkconfig.old`，且暂存内容不含 WiFi 密码与 API key 类配置键。
  - `evidence/` 未被 git 跟踪。
  - 全部检查通过后，有疑问时询问用户。
  - _Requirements: 2.13, 2.14, 3.1, 3.2, 3.18, 3.20_

## Notes

- **决定记录（2026-11-16）**：冻结很可能为误报；JTAG 采用方案 A（USB-JTAG，J2 第 33/34 脚，不改 eFuse）；2.1–2.6 为条件需求；14–21 与 22.2–22.3 为条件任务；22.1 为无调试器长时浸泡。
- **决定记录（续，2026-11-16）**：
  - 13.4 的 --mgmt 默认关闭；调用端点 /v1/mgmt/creds 只返回凭据名与版本，不返回值。
- **决定记录（2026-11-15）**：
  - D1（MANIFEST）：采用 A。扩展 `tools/efc_build.py`，由它直接写出 MANIFEST.json（见 30.2）。
  - D2（F0 基线）：采用 A。任务 28 不烧录，只核对开机日志并由用户眼见确认；任务 29 取消。
  - D3（顺序）：先执行冻结线任务 11–13（仅宿主）。任务 28 须先于任何烧录（14.2、31.6）。
  - D4（删除清单）：同意。`face_perf_screen` 暂不删除，先做任务 30.5 的用途分析。
  - D5（任务 31.0）：B2 采用 lv_efc；B6 采用提议映射；B7 测试内置 4 段（`smile_static`、`laugh`、`investigate`、`shocked`，29,641 字节），23 段候选池按全部内置计算预算（206,846 字节）；未内置片段回退 `smile_static` 并告警（2.15、3.22）。
- **决定记录（续，2026-11-15）**：
  - D-LIT：`lit_share_pct` 采用 design Glossary 的定义（表情圆内 32×32 采样点中亮度 ≥128 的比例）。撤回 27 记录的非键占比近似。
  - D-FRAME0：键占比与 lit 记录第 0 帧（`frame0_key_permille`、`frame0_lit_share_pct`）与全帧平均（仅供信息）。板上 alpha0 校验与屏幕自检以第 0 帧为准。
  - D-B10：底色圆维持隐藏。
  - D-36：36 的 R 规则与 230 px 问题，等待任务 33 的测量结果后再定。
  - D-GIT：23 个 `.efc` 与 MANIFEST.json 提交进 git（见 30.2）。
  - D-VENV：编码器环境迁至 `~/.venvs/efc`，不再使用 `/tmp/efc-venv`。
- **与 design.md 的偏差（执行时在 design.md 对应位置加注）**：
  - 百分位统计函数改放 `firmware/main/face_perf_stats.{c,h}`（L5 纯 C），宿主测试并入 [18]，不放 `sysdiag_core.c`，原因是表情线先于 sysdiag 推进（任务 4.1）。
  - design.md 的集成测试写法是 `freeze_watch.py` 常驻记录时用 `p4_cli.py "inject …"` 发命令，与串口独占（`TIOCEXCL`）冲突。本计划在 `freeze_watch.py` 增加命名管道命令中继与 `!rst` 伪命令，`p4_cli.py` 遇 EBUSY 时自动走中继（任务 13.2、13.3）。
  - 实验 E0 拆为两段：E0a（RTS/EN 复位、软件重启、冷上电）用 sysdiag 之前的最小探针完成（任务 14、15），以满足"先 E0、再 sysdiag"的顺序；E0b（MWDT/RWDT 复位后的保留区与 `Saved PC`）依赖注入命令，放在任务 18、21.2。
  - 阶段 A/B 的切换用新增 Kconfig 总开关 `ESP_CUBE_SYSDIAG_ENFORCE` 实现（design.md 只描述了行为，未命名开关）。
- **新增宿主套件编号**：[15] `test_sysdiag_record`、[16] `test_sysdiag_supervisor`、[17] `test_sysdiag_logring`、[18] 表情素材管线（EFC 往返、`.efc` 集 lint、`test_face_precheck`、`test_face_perf_stats`、`face_perf_screen`）、`[附] freeze_watch selftest`；[19] 仅在任务 36 触发时加入（EFC 解码等价套件）；未触发则记为未触发。
- **需要用户在场的步骤**：任务 37（表情眼见确认）、任务 15、任务 23，以及任务 39（有疑问时）；任务 36 触发时，其眼见确认并入任务 37。
- **可能中止并回报用户的条件**：任务 28 的结果与 B1/B2 不符；任务 32 表情不可见；任务 18 的 X6 两核关中断后不复位；任务 34 的热载峰值超限；任务 22.1 出现无法自动恢复的设备侧停机。
- **范围外事项（已调整）**：OpenOCD 实测：方案 A 已接线并完成连通（任务 1.5，2026-11-16）。git 提交（30.2 的 `.efc` 与 MANIFEST 提交除外；其余如按项目惯例提交，遵守任务 39 的凭据检查）。
- **许可**：用户于 2026-11-15 裁决，个人 DIY 非商业用途；素材与 esp_efc_codec 按此使用；仓库若对外公开需复核。
- **待用户确认（尚未决定）**：36 的 R 规则与 230 px 问题（33 之后）；30.5 的 face_perf_screen 去留（分析后决定）；未提交 EAF 改动的备份分支（未答复）。
- **Python 环境**：IDF 环境的 python3 缺少 PIL 与 numpy。需要时由用户批准安装，或指定独立 venv；在批准前，依赖它们的步骤写为未执行。
