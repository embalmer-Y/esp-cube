/*
 * drv_display_qspi.c —— CO5300 QSPI AMOLED 驱动实现(EspCube S31 板后端)
 *
 * 要点(对应设计文档 §4):
 *   1. QSPI 4-line 面板: esp_lcd_panel_io_spi_config 由 CO5300 组件提供的
 *      CO5300_PANEL_IO_QSPI_CONFIG 宏构造(dc 引脚不存在, 命令/数据通过 QSPI 特定时序);
 *   2. 面板窗口坐标必须【偶数对齐】(CO5300 硬约束)—— M1 采用 LVGL 全帧双缓冲,
 *      刷新区域恒为整屏(0..459), 天然满足对齐; 后续切部分刷新时需在 flush 前对齐;
 *   3. 亮度走寄存器 0x51(0-255), 无 PWM 背光。
 * P4C5 板走 MIPI-DSI 后端(本文件不参与编译, 见 drv_display_dsi.c)。
 */
#include "drv_display.h"

#include <limits.h>
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"          /* espressif/esp_lcd_co5300 组件 */
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "board_config.h"
#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "drv_display";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

/* 鱼鹰 AM200Q460460FLS1 官方初始化序列(2026-11-10 首亮攻坚):
 * 组件默认序列按乐鑫自家 480x480 屏写(CASET 6..477/PASET 0..465),
 * 与本面板不符。以下逐字节照搬鱼鹰 ESP32-S3 QSPI 例程:
 * gitee.com/osptek/display-amoled-2inch-460x460 examples/
 * esp32s3-idf5-co5300-qspi-lvgl-adapter-lvgl9/main/co5300_init_cmds.h */
static const co5300_lcd_init_cmd_t s_osptek_init[] = {
    {0xFE, (const uint8_t[]){0x00}, 1, 0},
    {0x11, NULL, 0, 120},                   /* SLPOUT */
    {0xFE, (const uint8_t[]){0x00}, 1, 0},
    {0xC4, (const uint8_t[]){0x80}, 1, 0},
    {0x3A, (const uint8_t[]){0x55}, 1, 0},  /* RGB565 */
    {0x53, (const uint8_t[]){0x20}, 1, 0},  /* 显控开(无此命令屏不亮) */
    {0x63, (const uint8_t[]){0xFF}, 1, 0},
    {0x2A, (const uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},   /* CASET */
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},   /* PASET */
    {0x29, NULL, 0, 60},                    /* DISPON */
    {0x51, (const uint8_t[]){0x00}, 1, 0},  /* 亮度(例程原值 0, UI 接管后设置) */
    {0x58, (const uint8_t[]){0x07}, 1, 10},
};

esp_err_t drv_display_init(const drv_display_cfg_t *cfg)
{
    if (s_panel != NULL) {
        return ESP_OK;                  /* 幂等 */
    }
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(cfg->h_res > 0 && cfg->h_res <= INT_MAX / 8 &&
                        cfg->h_res % 2 == 0 && cfg->v_res > 0 &&
                        cfg->v_res % 2 == 0 && cfg->pixel_clock_hz > 0 &&
                        (size_t)cfg->h_res <= SIZE_MAX / (size_t)cfg->v_res / sizeof(uint16_t),
                        ESP_ERR_INVALID_ARG, TAG, "invalid or unaligned display dimensions or clock");

    esp_err_t ret = ESP_OK;
    bool bus_initialized = false;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    uint16_t *test_fb = NULL;

    /* ---- 1. RST GPIO 初始化 ---- */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,       /* 推挽(首亮期比开漏驱动更可靠) */
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err, TAG, "configure reset GPIO");
    ESP_GOTO_ON_ERROR(gpio_set_level(BOARD_PIN_LCD_RST, 1), err, TAG, "release reset GPIO");

    /* ---- 2. SPI 总线 + QSPI 面板 IO ---- */
    spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
            BOARD_PIN_LCD_CLK, BOARD_PIN_LCD_D0, BOARD_PIN_LCD_D1,
            BOARD_PIN_LCD_D2, BOARD_PIN_LCD_D3,
            cfg->h_res * 8);                /* 约 4 行 RGB565, IDF 按 DMA 描述符取整 */
    ESP_GOTO_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                      err, TAG, "spi bus init");
    bus_initialized = true;

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(
            BOARD_PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = cfg->pixel_clock_hz;
    /* PSRAM 缓冲优先直接 DMA; 不满足对齐要求时 IDF 仍可能分配临时缓冲。 */
    io_cfg.flags.psram_dma_direct = 1;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
                                               &io_cfg, &io), err, TAG, "new panel IO failed");

    /* ---- 3. 面板驱动对象 ---- */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,           /* RGB565 */
    };
    /* 厂商配置: QSPI 接口 + 鱼鹰面板初始化序列(见文件头 s_osptek_init) */
    static co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_osptek_init,
        .init_cmds_size = sizeof(s_osptek_init) / sizeof(s_osptek_init[0]),
        .flags.use_qspi_interface = 1,
    };
    panel_cfg.vendor_config = &vendor_cfg;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_cfg, &panel),
                      err, TAG, "new co5300 panel failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), err, TAG, "panel reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), err, TAG, "panel init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP),
                      err, TAG, "panel gap");

    /* ---- 4. 首亮测试帧: 白/红/绿/蓝/黑/白 ---- */
    size_t fb_bytes = (size_t)cfg->h_res * cfg->v_res * sizeof(uint16_t);
    test_fb = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM);
    if (test_fb != NULL) {
        static const uint16_t bands[] = {
            0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0000, 0xFFFF,
        };
        for (int y = 0; y < cfg->v_res; y++) {
            size_t band = (size_t)((int64_t)y * 6 / cfg->v_res);
            uint16_t color = bands[band];
            uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
            for (int x = 0; x < cfg->h_res; x++) {
                test_fb[(size_t)y * cfg->h_res + x] = wire_color;
            }
        }
        ESP_GOTO_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, cfg->h_res, cfg->v_res,
                                                   test_fb), err, TAG, "first-light draw");
    } else {
        ESP_LOGW(TAG, "test frame alloc fail (%u bytes)", (unsigned)fb_bytes);
    }

    /* 显示开启命令会等待先前的异步彩条传输完成, 再释放帧缓冲。 */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), err, TAG, "panel display on");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, 80),
                      err, TAG, "panel brightness");
    ESP_LOGI(TAG, "panel brightness 80%%: ok");
    if (test_fb != NULL) {
        ESP_LOGI(TAG, "first-light color bars: transferred (%dx%d)", cfg->h_res, cfg->v_res);
#if CONFIG_ESP_CUBE_DISPLAY_DEBUG
        vTaskDelay(pdMS_TO_TICKS(3000));   /* 首亮观察窗(仅调试构建) */
#endif
        heap_caps_free(test_fb);
        test_fb = NULL;
    }

    s_io = io;
    s_panel = panel;
    ESP_LOGI(TAG, "CO5300 QSPI AMOLED ready: %dx%d @%dHz",
             cfg->h_res, cfg->v_res, cfg->pixel_clock_hz);
    return ESP_OK;

err:
    if (panel != NULL) {
        esp_err_t cleanup_err = esp_lcd_panel_del(panel);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "panel cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
    }
    if (io != NULL) {
        esp_err_t cleanup_err = esp_lcd_panel_io_del(io);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "panel IO cleanup failed; retaining DMA buffer and SPI bus: %s",
                     esp_err_to_name(cleanup_err));
            return ret;
        }
    }
    if (test_fb != NULL) {
        heap_caps_free(test_fb);
    }
    if (bus_initialized) {
        esp_err_t cleanup_err = spi_bus_free(BOARD_LCD_HOST);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
    }
    return ret;
}

esp_lcd_panel_io_handle_t drv_display_get_io(void)
{
    return s_io;
}

esp_lcd_panel_handle_t drv_display_get_panel(void)
{
    return s_panel;
}

esp_err_t drv_display_set_brightness(int percent)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "panel not initialized");
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return esp_lcd_panel_co5300_set_brightness(s_panel, (uint8_t)percent);
}

esp_err_t drv_display_sleep(bool sleep)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "panel not initialized");
    return esp_lcd_panel_disp_on_off(s_panel, !sleep);
}
