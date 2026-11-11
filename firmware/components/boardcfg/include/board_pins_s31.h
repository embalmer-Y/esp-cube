/*
 * board_pins_s31.h —— EspCube 核心板(ESP32-S31-WROOM-3)引脚与板级常量
 *
 * ✅ 2026-11-10 全面重核(官方 J2 表 + 原理图 PDF, 用户实物核对触发):
 *   - 板上唯一 GPIO 排针 = J2(CON20X2, 40P)。GPIO20-25 是 SDIO 复用脚,
 *     丝印为 D0/D1/D2/D3/CLK/CMD(非数字), 且连着板上 SD 卡座 → 弃用,
 *     改用完全空闲且丝印为纯数字的 GPIO40/42/46-49 + 38/39。
 *   - 板载音频 GPIO50-57 为 Codec 专线(不在排针上), 实机已验证, 不动。
 *   - U0TXD=GPIO58 / U0RXD=GPIO59 / BOOT=GPIO61 / RGB LED=GPIO60(勿占用)。
 *   详见 docs/03_接线与验证指南.md §2。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 板级能力 ---- */
#define BOARD_NAME              "EspCube-S31"
#define BOARD_LCD_IF_QSPI       1               /* 显示走 QSPI(GP-SPI2) */
#define BOARD_FLASH_SIZE_MB     16
#define BOARD_PSRAM_SIZE_MB     16              /* 封装内 Octal */

/* AM200Q460460FLS1 有效显示区域在 CO5300 GRAM 中的偏移 */
#define BOARD_LCD_X_GAP         10
#define BOARD_LCD_Y_GAP         0

/* ===================== QSPI AMOLED(鱼鹰光电 2寸 460x460, CO5300) =====================
 * 8 根信号全部集中在 J2 排针(pins 11/13/14/16/19/20/21/22), 丝印即数字 */
#define BOARD_LCD_HOST           SPI2_HOST        /* GP-SPI2, 经 GPIO 矩阵路由 */
#define BOARD_PIN_LCD_CLK        GPIO_NUM_42      /* J2-20(丝印 42) QSPI 时钟 */
#define BOARD_PIN_LCD_CS         GPIO_NUM_40      /* J2-19(丝印 40) QSPI 片选 */
#define BOARD_PIN_LCD_D0         GPIO_NUM_46      /* J2-16(丝印 46) 数据 IO0 */
#define BOARD_PIN_LCD_D1         GPIO_NUM_47      /* J2-13(丝印 47) 数据 IO1 */
#define BOARD_PIN_LCD_D2         GPIO_NUM_48      /* J2-14(丝印 48) 数据 IO2 */
#define BOARD_PIN_LCD_D3         GPIO_NUM_49      /* J2-11(丝印 49) 数据 IO3 */
#define BOARD_PIN_LCD_RST        GPIO_NUM_38      /* J2-22(丝印 38) 面板复位 */
#define BOARD_PIN_LCD_TE         GPIO_NUM_39      /* J2-21(丝印 39) TE 同步(可选) */

/* 触摸(带触摸批次启用, CST9220 族, I2C —— 占位: 屏幕批次无触摸;
 * 注意避开 LCD 已占用的 40/42, 届时用空闲 GPIO 重排) */
#define BOARD_PIN_TP_SDA         GPIO_NUM_43
#define BOARD_PIN_TP_SCL         GPIO_NUM_44
#define BOARD_PIN_TP_RST         GPIO_NUM_45
#define BOARD_PIN_TP_INT         GPIO_NUM_2

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
