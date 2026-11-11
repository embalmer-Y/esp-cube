/*
 * board_config.h —— 板级共性能力描述(与引脚解耦的功能性常量)
 * 板级差异常量(显示接口/PSRAM 容量/怪癖开关)在 board_pins_*.h 中登记。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 音频链路能力(两板同款: ES8311 codec + NS4150B 功放 + 模拟麦) */
#define BOARD_AUDIO_DAC        "ES8311"
#define BOARD_AUDIO_AMP        "NS4150B"   /* 3W @4Ω/5V, 喇叭座 1.25mm 4Ω */

/* 最终产品音频硬件(仅文档记录, 不参与编译)
 * 喇叭: Peerless TC5FC07-04 (1.5寸全频 4Ω/5W, 82.8dB)
 * DSP 升级候选: TAS5805M / AW88298 / ADAU1761 —— 见《01_技术方案.md》§1.7
 */

#ifdef __cplusplus
}
#endif
