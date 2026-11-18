/*
 * cube_tasks.h —— 全机任务参数单点登记表(L0)
 *
 * 优先级全局序(高→低, docs/06 §7):
 *   mixer(20) > voice(18) > player(12) > ai(5) > LVGL(4)
 * 音频实时 > 唤醒检测 > 流解码 > 云端会话 > UI 渲染。
 * 绑核策略: core0 = 音频实时/网络协议栈/BT; core1 = 解码/渲染/云任务。
 * 各组件创建任务一律引用本表宏, 调度关系改动只改这一处。
 *
 * 栈放置策略(docs/22 §3B, v6.1 板上实证口径):
 *   HP DRAM 静态 = mixer/voice/aisess/aremind(实时或大栈)
 *   RTCRAM 定置(.rtc.bss, 非 DMA/非 PIE/非实时) = cli/soul/think(P4 ~27KB)
 *   PSRAM(player 双槽静态/faceload/skpf/pstart/psoul/psnsd/aitts/aimic, WithCaps)
 *   内部动态(库创建/实证约束) = taskLVGL(DMA caps, PIE 不变量)/pairsrv
 *     (esp_task_stack_is_sane_cache_disabled 断言, 批次 20 实证禁 PSRAM)
 *   新增任务必须先入 docs/22 预算表再写创建代码(v6 教训: HP DRAM 每多
 *   1KB 静态 = dma 池少 1KB, 全栈入 DRAM 曾致 taskLVGL 创建失败 boot loop)。
 */
#pragma once

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

/* ---- 混音引擎(app_audio) ---- */
#define CUBE_TASK_MIXER_NAME   "mixer"
#define CUBE_TASK_MIXER_STACK  8192
#define CUBE_TASK_MIXER_PRIO   20
#define CUBE_TASK_MIXER_CORE   0

/* ---- 语音前端(app_voice; esp-sr 推理负载) ---- */
#define CUBE_TASK_VOICE_NAME   "voice"
#define CUBE_TASK_VOICE_STACK  8192
#define CUBE_TASK_VOICE_PRIO   18
#define CUBE_TASK_VOICE_CORE   0

/* ---- 网络播放器(app_player; 栈走 PSRAM 静态双槽) ---- */
#define CUBE_TASK_PLAYER_NAME      "player"
#define CUBE_TASK_PLAYER_STACK     12288
#define CUBE_TASK_PLAYER_PRIO      12
#define CUBE_TASK_PLAYER_CORE      1

/* ---- AI 会话编排(app_ai_session 三任务) ----
 * 14KB: LLM 调用链 8KB 溢出(2026-11-11)→12KB; M3 灵魂注入(system 拼接
 * + mem block)再把 12KB 顶穿(TTS 期 Stack protection fault 轮崩,
 * 2026-11-12 用户实测"语音没播完电台进来+爆音"真凶)→14KB, 大缓冲
 * 同步挪静态(system_full 1.4KB) */
#define CUBE_TASK_AISESS_NAME  "aisess"
#define CUBE_TASK_AISESS_STACK 14336
#define CUBE_TASK_AISESS_PRIO  5
#define CUBE_TASK_AISESS_CORE  0

#define CUBE_TASK_AITTS_NAME   "aitts"
#define CUBE_TASK_AITTS_STACK  6144
#define CUBE_TASK_AITTS_PRIO   5
#define CUBE_TASK_AITTS_CORE   1

#define CUBE_TASK_AIMIC_NAME   "aimic"
#define CUBE_TASK_AIMIC_STACK  6144
#define CUBE_TASK_AIMIC_PRIO   5
#define CUBE_TASK_AIMIC_CORE   0

/* ---- 串口 CLI(main/cli.c; 调试/演示命令, 后续由语音接管输入) ----
 * 栈 8KB: printf 族 ~2KB + weapi/http 调用链(实测 4KB 溢出: 搜索结果
 * 结构体 1.1KB 局部变量 + vfprintf 栈深) */
#define CUBE_TASK_CLI_NAME   "cli"
#define CUBE_TASK_CLI_STACK  8192
#define CUBE_TASK_CLI_PRIO   5
#define CUBE_TASK_CLI_CORE   0

/* ---- LVGL 渲染(app_ui; esp_lvgl_port 任务) ----
 * EFC 解码跑在 LVGL 任务上下文(esp_efc_codec 官方 emote_player 口径
 * 20KB+), 启用 EFC 时加大; 未启用保持 16KB(S31 内部 RAM 紧张) */
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
/* §52: pre_ui 时内部最大连续块实测 23552B(DRAM+RTCRAM), 24KB 确定性放
 * 不下; 官方 emote_player 例程口径 20KB, 降到 20480(PIE/FPU 保存区从
 * 栈底 carve 后有效 ~19.5KB) */
#define CUBE_TASK_LVGL_STACK   20480
#else
#define CUBE_TASK_LVGL_STACK   16384
#endif
#define CUBE_TASK_LVGL_PRIO    4
#define CUBE_TASK_LVGL_CORE    1
