/*
 * drv_display.c —— CO5300 QSPI AMOLED 驱动实现
 *
 * 要点(对应设计文档 §4):
 *   1. QSPI 4-line 面板: esp_lcd_panel_io_spi_config 由 CO5300 组件提供的
 *      CO5300_PANEL_IO_QSPI_CONFIG 宏构造(dc 引脚不存在, 命令/数据通过 QSPI 特定时序);
 *   2. 面板窗口坐标必须【偶数对齐】(CO5300 硬约束)—— M1 采用 LVGL 全帧双缓冲,
 *      刷新区域恒为整屏(0..459), 天然满足对齐; 后续切部分刷新时需在 flush 前对齐;
 *   3. 亮度走寄存器 0x51(0-255), 无 PWM 背光。
 */
#include "drv_display.h"

#include <string.h>
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"          /* espressif/esp_lcd_co5300 组件 */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "drv_display";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static drv_display_cfg_t s_cfg;

esp_err_t drv_display_init(const drv_display_cfg_t *cfg)
{
    if (s_panel != NULL) {
        return ESP_OK;                  /* 幂等 */
    }
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    s_cfg = *cfg;

    /* ---- 1. RST/TE GPIO 初始化 ---- */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT_OD,    /* 开漏, 外部上拉, 面板不接时也不损伤 */
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BOARD_PIN_LCD_RST, 1);

    /* ---- 2. SPI 总线 + QSPI 面板 IO ----
     * CO5300 组件提供两组宏: 总线配置(CO5300_PANEL_BUS_QSPI_CONFIG)与
     * 面板 IO 配置(CO5300_PANEL_IO_QSPI_CONFIG, 默认40MHz, 此后覆盖时钟)。 */
    spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
            BOARD_PIN_LCD_CLK, BOARD_PIN_LCD_D0, BOARD_PIN_LCD_D1,
            BOARD_PIN_LCD_D2, BOARD_PIN_LCD_D3,
            s_cfg.h_res * 8);               /* 单次传输8行(7.3KB): 吞吐~74fps且内部DMA
                                             * 私有缓冲小, 避开BT/WiFi初始化期的内存竞争 */
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(
            BOARD_PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = cfg->pixel_clock_hz;   /* 覆盖默认40MHz(点亮稳定后提80MHz) */
    /* 关键: LVGL 全帧缓冲在 PSRAM —— 打开 PSRAM 直接 DMA(GDMA-AXI 可达 PSRAM),
     * 零拷贝;否则每笔传输要从内部 DMA 堆临时分配 bounce 缓冲,
     * 在 BT/WiFi 初始化后会因内部内存碎片化而失败(实测)。 */
    io_cfg.flags.psram_dma_direct = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
                                                 &io_cfg, &s_io), TAG, "new panel IO failed");

    /* ---- 3. 面板驱动对象(组件内置厂商初始化序列) ---- */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,           /* RGB565 */
    };
    /* 厂商配置: 声明 QSPI 接口模式(组件按此切换命令集) */
    static co5300_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    panel_cfg.vendor_config = &vendor_cfg;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel),
                        TAG, "new co5300 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "CO5300 QSPI AMOLED ready: %dx%d @%dHz",
             cfg->h_res, cfg->v_res, cfg->pixel_clock_hz);
    return ESP_OK;
}

esp_lcd_panel_io_handle_t drv_display_get_io(void)
{
    return s_io;
}

esp_lcd_panel_handle_t drv_display_get_panel(void)
{
    return s_panel;
}

/* CO5300 亮度寄存器 0x51(Brightness), 0-255 —— 与 SH8601 族一致, 带上电时序由面板 init 保证 */
esp_err_t drv_display_set_brightness(int percent)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t val = (uint8_t)(percent * 255 / 100);
    /* TODO(点亮核对): 若带显示异常, 对照鱼鹰 CO5300 datasheet 确认亮度命令码 */
    return esp_lcd_panel_io_tx_param(s_io, 0x51, (const uint8_t[]){ val }, 1);
}

esp_err_t drv_display_sleep(bool sleep)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 深度待机: AMOLED 全黑且内部 boost 关闭, 电流降至 µA 级 */
    return esp_lcd_panel_io_tx_param(s_io, sleep ? 0x28 : 0x29, NULL, 0);  /* 28h=Display Off 29h=On */
}
