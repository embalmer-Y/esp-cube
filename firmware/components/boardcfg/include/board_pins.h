/*
 * board_pins.h —— 板卡引脚映射分派器(全项目唯一引脚登记处入口)
 *
 * 同一代码库双目标: 按 Kconfig choice ESP_CUBE_BOARD 转发到具体板级头
 *   - board_pins_s31.h  : EspCube 核心板(ESP32-S31-WROOM-3, QSPI AMOLED)
 *   - board_pins_p4c5.h : 鱼鹰 OSPTEK ESP32-P4C5(MIPI-DSI AMOLED + C5 WiFi)
 * 消费方只 include 本头; 板级差异(接口类型/怪癖)通过 BOARD_LCD_IF_* /
 * BOARD_AMP_MANUAL_PA 等能力宏表达。
 */
#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

#if CONFIG_ESP_CUBE_BOARD_P4C5
#include "board_pins_p4c5.h"
#else
#include "board_pins_s31.h"
#endif
