/*
 * board_pins_p4c5.h —— 鱼鹰光电 OSPTEK ESP32-P4C5 开发板引脚与板级常量
 *
 * 依据: 底板原理图 V1.3 + Kaleido 26 号调研三重验证(同款板真机实测, 归档
 * project-archive/entelea-handoff-2026-11-08/Kaleido/docs/research/ 目录)
 * + radio-p4c5 独立工程全链点亮(2026-11: ES8311@48k → hosted 3.0.10 → HLS 电台)。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 板级能力 ---- */
#define BOARD_NAME              "OSPTEK-P4C5"
#define BOARD_LCD_IF_MIPI_DSI   1               /* 显示走 MIPI-DSI(片上 DPI) */
#define BOARD_FLASH_SIZE_MB     16              /* Boya, 烧写需 no-stub */
#define BOARD_PSRAM_SIZE_MB     32              /* 片内叠封 HEX @200MHz */

/* ===================== 2寸 AMOLED(AM200MT460460FLS2, CO5300 MIPI 版) =====================
 * 与 S31 的 QSPI 版(AM200Q…FLS1)同面板同分辨率(460x460), 接口换 MIPI-DSI:
 *   - DSI 1 data lane @330Mbps(RGB565), DPI 像素钟 16MHz(实测 36fps)
 *   - MIPI PHY 供电 = P4 内部 LDO 通道3 @2500mV(无外部引脚)
 *   - 模组无 RST 脚(-1); AMOLED 自发光无背光脚(亮度走 DCS 0x51)
 * 参数真机验证: Kaleido panel-2in(归档)+ osptek 官方例程
 * (display-amoled-2inch-460x460, esp32p4-idf6-co5300-mipi-*)。
 */
#define BOARD_LCD_DSI_LANE_MBPS     330
#define BOARD_LCD_DPI_CLK_MHZ       16
#define BOARD_LCD_DSI_LDO_CHAN      3           /* LDO_VO3 → VDD_MIPI_DPHY */
#define BOARD_LCD_DSI_LDO_MV        2500
#define BOARD_PIN_LCD_RST           (-1)        /* 模组无复位脚 */
#define BOARD_LCD_DSI_NUM_FBS       2

/* 触摸: 屏模组带 CST820(I2C 0x15), I2C0 与 ES8311 共总线(SDA=7/SCL=8);
 * 驱动 = kodediy esp_lcd_touch_cst820(Apache-2.0, 已 vendor 进 drv_touch) */
#define BOARD_HAS_TP_CST820         1
#define BOARD_PIN_TP_SDA            GPIO_NUM_7
#define BOARD_PIN_TP_SCL            GPIO_NUM_8
#define BOARD_PIN_TP_RST            GPIO_NUM_6
#define BOARD_PIN_TP_INT            GPIO_NUM_21

/* ===================== TF 卡槽(microSD, SDMMC slot0, 底板 V1.3) =====================
 * Kaleido 26 号调研三重验证: C5 SDIO 在 slot1(GPIO14-19), TF 在 slot0
 * (GPIO39-44) —— 两槽独立, WiFi 与 TF 可同时使用; DET=45 卡插拔检测 */
#define BOARD_PIN_TF_CLK            GPIO_NUM_43
#define BOARD_PIN_TF_CMD            GPIO_NUM_44
#define BOARD_PIN_TF_D0             GPIO_NUM_39
#define BOARD_PIN_TF_D1             GPIO_NUM_40
#define BOARD_PIN_TF_D2             GPIO_NUM_41
#define BOARD_PIN_TF_D3             GPIO_NUM_42
#define BOARD_PIN_TF_DET            GPIO_NUM_45

/* ===================== 板载音频(ES8311 + NS4150B, 底板 U13) =====================
 * 与 S31 核心板同款 codec/功放/模拟麦链(LMA3729 硅麦 → MIC1), 仅引脚不同。
 * I2C 与 in-cell 触摸共总线(SDA=7/SCL=8), 内部上拉兜底。 */
#define BOARD_PIN_CODEC_I2C_SDA     GPIO_NUM_7
#define BOARD_PIN_CODEC_I2C_SCL     GPIO_NUM_8
#define BOARD_PIN_CODEC_MCLK        GPIO_NUM_13     /* I2S0_MCLK → ES8311 MCLK */
#define BOARD_PIN_CODEC_SCLK        GPIO_NUM_12     /* I2S0_SCLK (BCLK) */
#define BOARD_PIN_CODEC_WS          GPIO_NUM_10     /* I2S0_LRCK (WS) */
#define BOARD_PIN_CODEC_DOUT        GPIO_NUM_9      /* I2S0_SDOUT: P4 → ES8311 DAC */
#define BOARD_PIN_CODEC_DIN         GPIO_NUM_11     /* I2S0_DSDIN: ES8311 ADC → P4 */
#define BOARD_PIN_AMP_EN            GPIO_NUM_53     /* NS4150B STD(高有效) */

/* 板级怪癖: es8311 驱动的 pa_pin 自动配置在本板不生效(Kaleido V-38 实证),
 * codec open 后需手工 gpio_config 推挽输出并拉高 */
#define BOARD_AMP_MANUAL_PA         1

/* I2C 地址(espressif/esp_codec_dev 1.6.2: ES8311_CODEC_DEFAULT_ADDR 即此) */
#define BOARD_CODEC_I2C_ADDR        0x30            /* ES8311 7位地址0x18 的 8位形式 */

/* I2S DMA 形状(8×480 ≈ 80ms@48k + auto_clear: 欠载时 DMA 默认循环重发
 * 尾缓冲(实测语音末字无限复读), 开自动清零则发静音 —— radio-p4c5/
 * Kaleido 同款板实证) */
#define BOARD_AUDIO_DMA_DESC        8
#define BOARD_AUDIO_DMA_FRAMES      480
#define BOARD_AUDIO_I2S_AUTO_CLEAR  1

/* 麦克风 ADC 增益(Kaleido V-38 同款板实证 30dB): ES8311 默认增益低,
 * 不设则 WakeNet/ASR 收音偏小 → 唤醒不灵敏(2026-11-11 板上首验教训) */
#define BOARD_AUDIO_MIC_GAIN_DB     36.0f

/* ===================== C5 共芯(SDIO, esp_hosted P4 默认值) =====================
 * CLK=18 CMD=19 D0..D3=14..17 —— 与 esp_hosted 在 P4 的预设默认完全一致,
 * app_net 无需 SDIO 引脚配置, 仅登记复位脚 */
#define BOARD_PIN_C5_SDIO_CLK       GPIO_NUM_18
#define BOARD_PIN_C5_SDIO_CMD       GPIO_NUM_19
#define BOARD_PIN_C5_SDIO_D0        GPIO_NUM_14
#define BOARD_PIN_C5_SDIO_D1        GPIO_NUM_15
#define BOARD_PIN_C5_SDIO_D2        GPIO_NUM_16
#define BOARD_PIN_C5_SDIO_D3        GPIO_NUM_17
#define BOARD_PIN_C5_RESET          GPIO_NUM_54     /* 高有效, host 每次启动复位 C5 */

#ifdef __cplusplus
}
#endif
