/*
 * display_selftest.c —— CO5300 单线(3-wire 9-bit)点亮自检
 *
 * 用途(docs/05 §2 P1/P5): 屏幕故障面收敛 —— 已知"单线控制正常、
 * 四线刷新失败"。本自检【不经 GPSPI】, 用 GPIO 直接位压 3-wire 9-bit
 * 波形(上电默认模式, 不发 0xC4 0x80 进 QSPI), 点亮 6 色彩条:
 *   · 彩条出现   → D0/CLK/CS/RST 四根线 + 面板供电/时序全部正常,
 *                  故障面收敛到 D1/D2/D3 或 S31 四线发送侧;
 *   · 彩条不出   → 上述任一环节断(杜邦线/接触/面板), 与四线无关。
 *
 * 波形: 每字节 9 个时钟, 先发 D/C 位(0=命令/1=数据), 再 8bit MSB;
 *       一条命令+参数在一个 CS 低窗口内完成, 半周期 1us(~500kHz,
 *       对杜邦线宽容)。初始化序列逐字节镜像鱼鹰例程(s_osptek_init)
 *       但【剔除 0xC4 0x80】, 面板停留 3-wire 模式。
 */
#include "drv_display.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "board_pins.h"

static const char *TAG = "selftest";

/* ---- 位压原语(pins 全部 ≥32 → GPIO.out1 寄存器组) ---- */
#define PIN_MASK(pin)   (1ULL << ((pin) - 32))

static inline void clk_high(void) { GPIO.out1_w1ts.val = PIN_MASK(BOARD_PIN_LCD_CLK); }
static inline void clk_low(void)  { GPIO.out1_w1tc.val = PIN_MASK(BOARD_PIN_LCD_CLK); }
static inline void dat_high(void) { GPIO.out1_w1ts.val = PIN_MASK(BOARD_PIN_LCD_D0); }
static inline void dat_low(void)  { GPIO.out1_w1tc.val = PIN_MASK(BOARD_PIN_LCD_D0); }
static inline void cs_low(void)   { GPIO.out1_w1tc.val = PIN_MASK(BOARD_PIN_LCD_CS); }
static inline void cs_high(void)  { GPIO.out1_w1ts.val = PIN_MASK(BOARD_PIN_LCD_CS); }

/* 半周期 ~1us: esp_rom_delay_us(1) + 寄存器写开销, 对杜邦线宽容 */
#define HALF_BIT()     esp_rom_delay_us(1)

/* 发送 9 bit: D/C(1bit) + byte(8bit MSB first) */
static void bit9_write(int dc, uint8_t byte)
{
    uint16_t word = (uint16_t)((dc ? 0x100 : 0x000) | byte);
    for (int i = 8; i >= 0; i--) {
        if ((word >> i) & 1) dat_high(); else dat_low();
        clk_high();
        HALF_BIT();
        clk_low();
        HALF_BIT();
    }
}

/* 命令包: 一个 CS 低窗口内 DC=0 命令 + DC=1 参数序列 */
static void cmd_packet(uint8_t cmd, const uint8_t *params, int n)
{
    cs_low();
    bit9_write(0, cmd);
    for (int i = 0; i < n; i++) {
        bit9_write(1, params[i]);
    }
    cs_high();
    HALF_BIT();
}

static void fill_bands(int h_res, int v_res)
{
    static const uint16_t bands[] = {
        0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0000, 0xFFFF,
    };
    const int nb = (int)(sizeof(bands) / sizeof(bands[0]));
    cs_low();
    bit9_write(0, 0x2C);                    /* RAMWR: 数据相位 CS 保持低 */
    for (int y = 0; y < v_res; y++) {
        uint16_t c = bands[y * nb / v_res];
        uint16_t wire = (uint16_t)((c << 8) | (c >> 8));   /* 大端线序 */
        if ((y & 63) == 0) {
            printf("  fill row %d/%d\n", y, v_res);
            taskYIELD();                /* 喂 IDLE(修看门狗) */
        }
        for (int x = 0; x < h_res; x++) {
            bit9_write(1, (uint8_t)(wire >> 8));
            bit9_write(1, (uint8_t)(wire & 0xFF));
        }
    }
    cs_high();
}

esp_err_t drv_display_single_lane_test(int h_res, int v_res)
{
    ESP_LOGI(TAG, "===== 单线(3-wire 9-bit)自检开始 =====");
    ESP_LOGI(TAG, "波形: CLK=%d D0=%d CS=%d RST=%d, ~500kHz, 不发 0xC4(停留 3-wire)",
             BOARD_PIN_LCD_CLK, BOARD_PIN_LCD_D0, BOARD_PIN_LCD_CS,
             BOARD_PIN_LCD_RST);

    /* ---- 1. 引脚全部 GPIO 输出(D1-D3 不驱动: 3-wire 模式面板不用) ---- */
    const uint64_t mask = (1ULL << BOARD_PIN_LCD_CLK) |
                          (1ULL << BOARD_PIN_LCD_CS) |
                          (1ULL << BOARD_PIN_LCD_D0) |
                          (1ULL << BOARD_PIN_LCD_RST);
    const gpio_config_t io_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio config");

    clk_low();
    cs_high();
    dat_low();

    /* ---- 2. 硬复位 ---- */
    gpio_set_level(BOARD_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "reset done");

    /* ---- 3. 初始化序列(镜像鱼鹰序列, 剔除 0xC4 0x80) ---- */
    int64_t t0 = esp_timer_get_time();
    cmd_packet(0xFE, (const uint8_t[]){0x00}, 1);      /* 页选 0 */
    cmd_packet(0x11, NULL, 0);                          /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd_packet(0xFE, (const uint8_t[]){0x00}, 1);
    cmd_packet(0x3A, (const uint8_t[]){0x55}, 1);      /* RGB565 */
    cmd_packet(0x53, (const uint8_t[]){0x20}, 1);      /* 显控开(缺此屏不亮) */
    cmd_packet(0x63, (const uint8_t[]){0xFF}, 1);
    cmd_packet(0x2A, (const uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4);  /* CASET */
    cmd_packet(0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4);  /* PASET */
    cmd_packet(0x29, NULL, 0);                          /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(60));
    cmd_packet(0x51, (const uint8_t[]){0xB0}, 1);      /* 亮度(便于观察) */
    cmd_packet(0x58, (const uint8_t[]){0x07}, 1);
    ESP_LOGI(TAG, "init seq sent (%lld ms) —— 若以上任一命令被误解, 面板无反应",
             (esp_timer_get_time() - t0) / 1000);

    /* ---- 4. 彩条填屏(慢速全帧, 预计 8-10s) ---- */
    t0 = esp_timer_get_time();
    fill_bands(h_res, v_res);
    ESP_LOGI(TAG, "fill done (%lld ms): 白/红/绿/蓝/黑/白 六条横带",
             (esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, ">>> 看屏: 有彩条=D0/CLK/CS/RST 全通(故障在 D1-D3/四线侧);"
             " 无彩条=单线也断(杜邦线/接触/面板)");
    return ESP_OK;
}
