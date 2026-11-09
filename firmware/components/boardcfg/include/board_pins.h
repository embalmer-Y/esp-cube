/*
 * board_pins.h —— EspCube 全部引脚定义(全项目唯一引脚登记处)
 *
 * ⚠ 接线核对清单(详见 docs/03_接线与验证指南.md):
 *   [已确定] 外接屏幕: QSPI 引脚由本项目自由分配, 选用 J2 排针的
 *            SDIO 组(GPIO20-25, 疑似 GP-SPI IOMUX 域, 信号质量优);
 *            避开 strapping 引脚(GPIO33/34=USB, 36/37, 60/61)。
 *   [待核对] 板载音频(ES8311/功放/麦克风)的 GPIO 由开发板内部走线决定,
 *            以下带 ⚠ 的值基于乐鑫官方 ESP32-S31-Function-CoreBoard-1
 *            用户指南的常见布线习惯给出, 首次点亮前必须对照官方用户指南
 *            原理图核对后修改! I2C 总线可通过 i2c_scanner 快速验证。
 */
#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== QSPI AMOLED(鱼鹰光电 2寸 460x460, CO5300) ===================== */
#define BOARD_LCD_HOST           SPI2_HOST        /* GP-SPI2 */
#define BOARD_PIN_LCD_CLK        GPIO_NUM_20      /* QSPI 时钟 */
#define BOARD_PIN_LCD_CS         GPIO_NUM_21      /* QSPI 片选 */
#define BOARD_PIN_LCD_D0         GPIO_NUM_22      /* QSPI 数据 0 (IO0) */
#define BOARD_PIN_LCD_D1         GPIO_NUM_23      /* QSPI 数据 1 (IO1) */
#define BOARD_PIN_LCD_D2         GPIO_NUM_24      /* QSPI 数据 2 (IO2) */
#define BOARD_PIN_LCD_D3         GPIO_NUM_25      /* QSPI 数据 3 (IO3) */
#define BOARD_PIN_LCD_RST        GPIO_NUM_38      /* 面板复位(高有效运行) */
#define BOARD_PIN_LCD_TE         GPIO_NUM_39      /* Tearing Effect 同步(可选) */

/* 触摸(带触摸批次启用, CST9220 族, I2C) */
#define BOARD_PIN_TP_SDA         GPIO_NUM_41
#define BOARD_PIN_TP_SCL         GPIO_NUM_42
#define BOARD_PIN_TP_RST         GPIO_NUM_40
#define BOARD_PIN_TP_INT         GPIO_NUM_35

/* ===================== 板载音频(ES8311 + NS4150B + 模拟麦克风) =====================
 * ✅ 已核对: 来自乐鑫官方 esp32-s31-function-coreboard-1-schematics.pdf(2026-05-13, v1.0)
 *    网络标签 → GPIO 映射已按符号几何关系逐一确认:
 *      PA_CTRL=IO57  I2S_DSDIN=IO56  I2S_LRCK=IO55  I2S_ASDOUT=IO54
 *      I2S_SCLK=IO53 I2S_MCLK=IO52   ESP_I2C_SDA=IO51  ESP_I2C_SCL=IO50
 *    麦克风: J6 驻极体模拟麦(MICP/MICN 差分, 交流耦合) → ES8311 MIC1P/MIC1N。
 */
#define BOARD_PIN_CODEC_I2C_SCL  GPIO_NUM_50      /* ESP_I2C_SCL → ES8311 */
#define BOARD_PIN_CODEC_I2C_SDA  GPIO_NUM_51      /* ESP_I2C_SDA → ES8311 */
#define BOARD_PIN_CODEC_MCLK     GPIO_NUM_52      /* I2S_MCLK */
#define BOARD_PIN_CODEC_SCLK     GPIO_NUM_53      /* I2S_SCLK (BCLK) */
#define BOARD_PIN_CODEC_WS       GPIO_NUM_55      /* I2S_LRCK (WS) */
#define BOARD_PIN_CODEC_DOUT     GPIO_NUM_56      /* I2S_DSDIN (ESP TX → codec DAC) */
#define BOARD_PIN_CODEC_DIN      GPIO_NUM_54      /* I2S_ASDOUT (codec ADC → ESP RX) */
#define BOARD_PIN_AMP_EN         GPIO_NUM_57      /* PA_CTRL → NS4150B(高有效) */

/* I2C 地址(注意: esp_codec_dev v2 采用 8 位地址形式, 内部右移1位得7位地址) */
#define BOARD_CODEC_I2C_ADDR     0x30             /* ES8311 7位地址0x18 的 8位形式 */

#ifdef __cplusplus
}
#endif
