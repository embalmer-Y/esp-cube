/*
 * drv_display_dsi.c —— CO5300 MIPI-DSI AMOLED 驱动实现(鱼鹰 P4C5 板后端)
 *
 * 面板: 鱼鹰 AM200MT460460FLS2(2寸 460x460 AMOLED, CO5300, MIPI 版;
 * 与 S31 的 QSPI 版同面板同分辨率, UI 几何零差异)。
 *
 * 参数真机实证(同款板一次点亮): Kaleido panel-2in 归档 + osptek 官方例程
 * esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9:
 *   - DSI 1 data lane @330Mbps(RGB565), DPI 像素钟 16MHz(实测 36fps);
 *   - MIPI PHY 供电 = P4 内部 LDO 通道3 @2500mV(无外部引脚);
 *   - 模组无 RST 脚(BOARD_PIN_LCD_RST = -1); 无背光脚(亮度走 DCS 0x51);
 *   - 首亮亮度 5%(Kaleido 教训: 点亮瞬态是峰值电流主力, USB 供电时曾把
 *     3.3V 轨拉塌 → POWERON 复位循环; bring-up 稳定后再经 UI 调高)。
 */
#include "drv_display.h"

#include <limits.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"          /* espressif/esp_lcd_co5300 组件 */
#include "board_pins.h"

static const char *TAG = "drv_display";

static esp_lcd_dsi_bus_handle_t s_bus = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

/* AM200MT460460FLS2 官方初始化序列(osptek 例程逐字节, Kaleido 归档同款):
 * FE/00 页选; 3A 像素格式 RGB565; 53=0x20 亮度控制使能; 51 初值 0;
 * 63 HPM 限流; 2A/2B GRAM 窗口(列偏移 10 —— 480 列 GRAM 取 460 有效区);
 * 11 睡醒(120ms 规格等待); 29 开显 */
static const co5300_lcd_init_cmd_t s_osptek_mipi_init[] = {
    {0xFE, (const uint8_t[]){0x00}, 1, 0},
    {0x3A, (const uint8_t[]){0x55}, 1, 0},  /* RGB565 */
    {0x53, (const uint8_t[]){0x20}, 1, 0},  /* 亮度控制使能(DCS 0x51 可调) */
    {0x51, (const uint8_t[]){0x00}, 1, 0},  /* 亮度初值 0 */
    {0x63, (const uint8_t[]){0xFF}, 1, 0},  /* HPM 限流 */
    {0x2A, (const uint8_t[]){0x00, 0x0A, 0x01, 0xD5}, 4, 0},   /* CASET 偏移10 */
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0xCB}, 4, 0},   /* PASET */
    {0x11, NULL, 0, 120},                   /* SLPOUT(120ms 规格等待) */
    {0x29, NULL, 0, 0},                     /* DISPON */
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

    /* ---- 1. DSI PHY 供电(内部 LDO 通道, 见板级头) ---- */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_LCD_DSI_LDO_CHAN,
        .voltage_mv = BOARD_LCD_DSI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "ldo for DSI PHY");
    ESP_LOGI(TAG, "DSI PHY powered (LDO%d @%dmV)", BOARD_LCD_DSI_LDO_CHAN,
             BOARD_LCD_DSI_LDO_MV);

    /* ---- 2. DSI 总线: 单 data lane ---- */
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 1,
        .lane_bit_rate_mbps = BOARD_LCD_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus), TAG, "dsi bus");

    /* ---- 3. DBI 命令通道(DCS 命令经 DSI 总线下发) ---- */
    esp_lcd_dbi_io_config_t dbi_cfg = CO5300_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_bus, &dbi_cfg, &s_io),
                        TAG, "dbi io");

    /* ---- 4. DPI 面板(osptek 官方时序) ---- */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = BOARD_LCD_DSI_NUM_FBS,
        .video_timing = {
            .hsync_back_porch = 72,
            .hsync_pulse_width = 4,
            .hsync_front_porch = 32,
            .vsync_back_porch = 4,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 6,
            .h_size = (uint32_t)cfg->h_res,
            .v_size = (uint32_t)cfg->v_res,
        },
    };
    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_osptek_mipi_init,
        .init_cmds_size = sizeof(s_osptek_mipi_init) / sizeof(s_osptek_mipi_init[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi_cfg },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,   /* 模组无 RST(-1) */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,                  /* RGB565 */
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel),
                        TAG, "new co5300 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    /* ---- 5. 黑场防花屏: 清两块 DPI 帧缓冲(LVGL 随后接管) ---- */
    void *fbs[BOARD_LCD_DSI_NUM_FBS] = { 0 };
    esp_err_t gf_err = esp_lcd_dpi_panel_get_frame_buffer(s_panel,
                                                          BOARD_LCD_DSI_NUM_FBS,
                                                          &fbs[0], &fbs[1]);
    if (gf_err == ESP_OK) {
        size_t fb_bytes = (size_t)cfg->h_res * cfg->v_res * sizeof(uint16_t);
        for (int i = 0; i < BOARD_LCD_DSI_NUM_FBS; i++) {
            memset(fbs[i], 0, fb_bytes);
        }
    } else {
        ESP_LOGW(TAG, "get fb failed, skip black clear: %s",
                 esp_err_to_name(gf_err));
    }

    /* ---- 6. 亮度 5% 起步(Kaleido 供电教训, 见文件头); UI 手势再调高 ---- */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(s_panel, 5),
                        TAG, "panel brightness");
    ESP_LOGI(TAG, "CO5300 MIPI-DSI AMOLED ready: %dx%d DPI %dMHz lane %dMbps "
             "(brightness 5%% for bring-up)", cfg->h_res, cfg->v_res,
             BOARD_LCD_DPI_CLK_MHZ, BOARD_LCD_DSI_LANE_MBPS);
    return ESP_OK;
}

esp_lcd_panel_io_handle_t drv_display_get_io(void)
{
    return s_io;                        /* DBI 命令通道句柄 */
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
