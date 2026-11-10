# EspCube 模块化分层重构 + 代码质量/性能优化

## Context（为什么做）

全代码 review 发现：分层纪律总体好，但存在**三处架构债**和**约 10 个真实 bug**：

1. **UI 双轨分叉**：固件在编的是旧版 `app_ui/ui_pages.c`（直接 include app_player/app_bt/audio_mixer，UI 层硬耦合应用层）；验收通过的新氛围 UI `app_ui/ui/ui_pages.c`（干净边界）只有模拟器在编；设备侧后端 `ui_port_esp.c` 不存在。
2. **L3→L1 越层**：app_voice、app_ai_session 直接调 `drv_audio_mic_read`（且 `ai_session.c:185` 传参错误——320 帧立体声写进 320 样本缓冲 = **640B 栈溢出** + 喂给 ASR 的是 48k 立体声而非 16k 单声道，M2 未上机所以未暴露）。两个消费者还会互相抢麦克风数据。
3. **main.c 残留业务桥接**（唤醒→AI 会话应内聚在 ai_session）。

性能侧：新 UI 渲染热路径每帧 ~15 万次整数除法 + ~15 万次 `sqrtf` + 405KB 全画布 memset + 80 次 `powf` + 每句歌词 58KB malloc/free——直接威胁 30fps 红线。

## 目标分层（L0-L5，全层可依赖 L0，其余只准调下一层的公开 API）

| 层 | 组件 | 职责 |
|---|---|---|
| L0 基础 | cubecore、boardcfg | 事件总线/ringbuf/FFT/重采样、板级常量 |
| L1 驱动 | drv_audio、drv_display | codec+I2S、QSPI 面板 |
| L2 服务 | **app_audio**（混音+**麦克风服务**）、**app_net**（重分类：连接服务）、speech_provider | 音频汇聚点、WiFi、云协议库 |
| L3 应用 | app_bt、app_player、app_voice、app_ai_session | 只依赖 L2/L0，控制面走事件总线 |
| L4 表现 | app_ui（纯 ui_model 契约） | 不 include 任何 L1-L3 固件头 |
| L5 装配 | main（+ui_port_esp.c 后端） | 唯一允许横跨全部层的组装点 |

依赖规则由 `tools/check_layers.py` 机器校验（解析各组件 #include 对照层表，越层即报错退出）。

## 工作包（每包一个 commit，顺序执行）

### WP1 分层架构与代码规范立档
- 新增 `docs/06_分层架构与代码规范.md`：层表、依赖规则、命名前缀约定（cube_/board_/drv_/audio_/app_/ks_/ui_）、头文件布局（include/ 公开、src/ 私有）、错误处理规范（组件内 ESP_RETURN_ON_*、main 统一失败域策略）、注释/任务参数约定。
- 新增 `tools/check_layers.py`（include 图校验器）。
- 各组件 CMakeLists 头注释标注所属层级。

### WP2 麦克风服务下沉 L2 + 所有权协议（消灭两条 L3→L1 越层）
- app_audio 新增 `audio_mic.h`：`int audio_mic_read_16k(int16_t *mono, int frames)` —— 内部持久缓冲 + 3:1 均值抽取带进位（不丢帧），文档化单消费者协议。
- **mic 所有权状态机**：IDLE 时 app_voice 独占（唤醒检测）；进入 LISTENING 时 app_voice（订阅 CUBE_EVT_AI_STATE）暂停读取、ai_session mic_task 独占；SPEAKING 时 app_voice 恢复（保住 barge-in 再唤醒打断）。
- app_voice 改用新 API（顺带消灭每 30ms malloc/free churn、任务 OOM 改 `goto exit`+`vTaskDelete`）。
- ai_session 改用新 API（修复栈溢出+采样率错误）；TTS 环形缓冲改二次幂+掩码；`on_tts_pcm` 环满改"丢弃+计数"不再阻塞网络回调 2s；`vTaskDelay(800)` 魔法尾窗改为 TTS 排空事件（ring 空 && tts closed → 回 IDLE）；自行订阅 CUBE_EVT_WAKE_DETECTED（main 桥接删除）。

### WP3 UI 纯化与双轨合一（最大单项）
- `app_ui_start(const app_ui_boot_cfg_t *cfg)`：io/panel 句柄由 main 传入 → app_ui 不再依赖 drv_display/board_pins；REQUIRES 收敛为 cubecore+lvgl+esp_lvgl_port。
- 固件 CMakeLists 切编 `ui/ui_pages.c + 字体`；**删除旧版** `app_ui/ui_pages.c`、`include/ui_pages.h`、`include/app_ui_internal.h`（新 UI 已验收、旧 UI 与模拟器行为分叉，保留只会改错文件）。
- 新增 `main/ui_port_esp.c` 实现 ui_model.h 后端：事件→快照（字符串写入 lvgl_port_lock 互斥）、ui_ctl_*→audio_mixer/app_player/drv_display、ui_audio_take_pcm→audio_mixer_take_spectrum。
- app_player 补公开播放列表 API（`app_player_station_num/name/idx/play_index`）——电台列表单一来源（旧 UI 自带列表随旧文件删除）。
- ui_model.h 增加 `ai_state`/`ai_text` 字段 + 极简状态提示（聆听中/思考中…走 toast），mock_app 补演示脚本。
- `firmware/sdkconfig.defaults` 补 `CONFIG_LV_USE_SNAPSHOT=y`、CLIB malloc、`LV_FONT_FMT_TXT_LARGE=y`（设备侧编译新 UI 的三个前置）。

### WP4 代码 review bug 修复清单
- cube_fft.c：部分分配失败→NULL 写崩溃；工作区 PSRAM→内部 RAM；去掉 512/256 硬编码上限。
- audio_mixer.c：频谱抽头撕裂（读侧 idx 双检）；init 第二块失败泄漏第一块；2×1.9KB 频谱缓冲→内部 RAM。
- sp_dashscope.c：TTS calloc 失败路径漏 `sp_sess_give`（锁永久丢失）；TTS host/auth 栈缓冲改堆持有（与 ASR 对齐，防组件升级踩雷）。**不做 ws 骨架去重重构**（Kaleido 实测遗产，保守）。
- icy_stream.c `parse_title`：事件长度补 `\0`。
- ui_pages.c：timer/canvas 空指针守卫；`rand()` 改内部 xorshift32（lv_tick 播种）。
- main.c 失败域：`audio_mixer_start` 成功后才开 PA（abort 不再循环"噗"声）；voice/AI 失败降级不 abort。
- drv_display 3 秒彩条延时改 `CONFIG_ESP_CUBE_DISPLAY_DEBUG` 门控。
- CMake 卫生：drv_audio 去掉未用的 cubecore、app_voice 去掉 boardcfg、speech_provider 去重复 cjson。

### WP5 渲染热路径性能（30fps 红线服务）
- `line_px`：全部整数除法 → 倒数表 `(x * recip[na]) >> 16` + `>>8` 近似（误差 <0.4%，AA 不可见）。
- `line_disc`：半径恒为 9.0/3.5/2.2 → 启动时预渲染三张 alpha sprite，blit 替代逐像素 `sqrtf`（每帧省 ~15 万次开方）。
- 画布清屏：全画布 memset → 只清上一帧曲线占用脏带（y 范围跟踪）。
- `bands_update`：lo/hi 频带边界启动时预计算表（每帧省 80 次 powf）。
- 歌词 snapshot：`lv_snapshot_take` 每句 malloc/free → 双持久 draw_buf + `lv_snapshot_take_to_buf` 复用。
- 验证：改造前后 sim 无头模式帧耗时对比 + 截图回归（视觉不得变化）。

### WP6 结构规范化
- `ui_pages.c`（950 行）拆分：`ui_internal.h`（theme 常量 + 53 个散落 static 归组为 scene/lyric/gesture/hud 结构体）、`ui_render.c`（频谱带+直线光栅）、`ui_lyrics.c`（歌词飞行）、`ui_widget.c`（HUD/toast/手势/动画工具，抽公共 `anim_move`/`sync_label` 消三处重复）、`ui_pages.c`（构建/场景/主循环/splash，`ui_pages_create` 109 行拆小）。魔法数字全部收敛为命名常量。固件与 sim 两个 CMakeLists 同步。
- app_player：抽出同构的 `decoder_cleanup`；`decoder_feed`(151 行)/`hls_session_run`(180 行) 按会话打开/帧循环/清理三段拆 helper；AAC cfg 结构体去重。纯结构搬移不改逻辑。
- cubecore 新增 `cube_tasks.h`：全机任务名/栈/优先级/绑核集中一张表（mixer20 > voice18 > player12 > ai5 > LVGL4 的全局序目前无单点记录），各组件引用宏。

## 明确不做（控制风险）
mp3_helix（vendored）；sp_dashscope/minimax 大重构（实测遗产只修 bug）；FFT 加窗/换 dl_fft（会改变已验收频谱视觉）；电台列表 NVS 持久化；AVRCP 透传播放暂停（保持 M3 计划）。

## 验证
每个 WP 收尾跑全套：
1. **sim**：`./run.sh --demo` 截图与基线逐张比对（WP4/6 必须完全一致，WP3 的新 UI 行为已在模拟器验收过）+ ASAN 构建无告警。
2. **固件编译**：`idf.py build` 两遍——默认配置 + `ESP_CUBE_ENABLE_VOICE/AI=y` 全功能（真机无板，编译期验证 + 行为保持纪律兜底）。
3. `tools/check_layers.py` 全绿。
4. WP5 附前后帧耗时数据。

## 风险与对策
- 固件无法真机回归（S31 未接）→ 设备侧专属代码（app_player/app_bt/app_voice）只做机械拆移与枚举出的 bug 修复，每处有独立推理依据；共享代码全部走 sim 回归。
- 删除旧 UI 后设备端视觉状态直到屏幕修复才能看 → 新 UI 在模拟器已验收、固件编译通过即达标；显示失败仍走 audio-only 降级路径不变。