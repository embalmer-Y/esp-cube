/*
 * drv_touch.c —— CST820 触摸接入实现
 *
 * vendor 驱动(kodediy esp_lcd_touch_cst820, Apache-2.0)只依赖
 * esp_lcd_panel_io; 此处补 I2C panel-io 桥接与板级配置。
 * 屏为圆形 460×460, 触摸报告范围同为 460×460(FPC 同域)。
 */
#include "drv_touch.h"
#include "esp_lcd_touch_cst820.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "sdkconfig.h"
#include "board_pins.h"

static const char *TAG = "drv_touch";

static esp_lcd_touch_handle_t s_tp;

esp_err_t drv_touch_start(i2c_master_bus_handle_t bus)
{
#if BOARD_HAS_TP_CST820
    if (s_tp != NULL) return ESP_OK;
    if (bus == NULL) return ESP_ERR_INVALID_ARG;

    /* CST820 在共享 I2C 总线上的 panel-io(地址 0x15) */
    esp_lcd_panel_io_i2c_config_t io_cfg =
        ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    io_cfg.scl_speed_hz = 400000;   /* osptek lvgl-port demo 同款(CLI 版 100k) */

    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io),
                        TAG, "panel io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 460,
        .y_max = 460,
        .rst_gpio_num = BOARD_PIN_TP_RST,
        .int_gpio_num = BOARD_PIN_TP_INT,
        .levels = {
            .reset = 0,          /* RST 低有效复位 */
            .interrupt = 0,      /* INT 低有效(按下) */
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_err_t err = esp_lcd_touch_new_i2c_cst820(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cst820 probe failed: %s(屏模组可能无触摸批次)",
                 esp_err_to_name(err));
        esp_lcd_panel_io_del(io);
        return err;
    }
    ESP_LOGI(TAG, "cst820 up (rst=%d int=%d)",
             BOARD_PIN_TP_RST, BOARD_PIN_TP_INT);
    return ESP_OK;
#else
    (void)bus;
    return ESP_ERR_NOT_SUPPORTED;      /* 板级无触摸(S31 屏批次) */
#endif
}

esp_lcd_touch_handle_t drv_touch_handle(void)
{
    return s_tp;
}
