/*
 * app_bt.h —— 蓝牙音频(A2DP Sink + AVRCP 控制)
 *
 * ESP32-S31 原生经典蓝牙(BR/EDR): 手机连接 EspCube 即把音频流推送过来。
 * 数据路径: Bluedroid 内置 SBC 解码 → PCM(44.1k/2ch/16bit)
 *           → cube_resampler(44.1k→48k) → 混音器 AUDIO_SRC_BT 通道。
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化蓝牙控制器/协议栈并进入可连接模式(异步, 回调上报连接事件) */
esp_err_t app_bt_start(void);

/* 是否已连接手机 */
bool app_bt_is_connected(void);

#ifdef __cplusplus
}
#endif
