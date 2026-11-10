/*
 * app_ui.c —— LVGL 集成与显示绑定
 *
 * esp_lvgl_port 负责: LVGL 任务(固定 core1)/tick/flush 到 esp_lcd 面板。
 * 本文件只做装配;页面内容在 ui_pages.c。
 */
#include "app_ui.h"
#include "ui_pages.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "drv_display.h"
#include "board_pins.h"

static const char *TAG = "ui";

esp_err_t app_ui_start(void)
{
    /* ---- 1. LVGL 运行环境 ----
     * v2.9 的任务参数在结构体里(Kconfig 无效): 绑核1(渲染与音频分核),
     * 栈 16KB(FFT/canvas 绘制), 优先级 4(低于混音器, 不抢占实时音频)。 */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = 4;
    lvgl_cfg.task_stack = 16384;
    lvgl_cfg.task_affinity = 1;         /* core1: 与 core0 的音频/蓝牙分核 */
    lvgl_cfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init");

    /* ---- 2. 显示设备绑定: 全帧双缓冲(设计文档 §4) ----
     * full_refresh=1: LVGL 每次提交整帧(坐标天然偶对齐, 满足 CO5300 约束);
     * swap_bytes=1: SPI/QSPI 面板按大端发送 RGB565, 端口层负责字节序;
     * 缓冲 2×460×460×2B≈846KB 落 PSRAM(S31 PSRAM 专用控制器, 不与 LCD 争带宽)。 */
    const int w = CONFIG_ESP_CUBE_LCD_H_RES;
    const int h = CONFIG_ESP_CUBE_LCD_V_RES;
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = drv_display_get_io(),
        .panel_handle = drv_display_get_panel(),
        .buffer_size = (uint32_t)w * h,         /* 全帧像素(RGB565) */
        .double_buffer = true,
        .hres = (uint32_t)w,
        .vres = (uint32_t)h,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .buff_dma = false,
            .swap_bytes = true,                 /* 帧序异常时优先核对此项 */
            .full_refresh = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "add disp");

    /* ---- 3. 页面构建 ---- */
    lvgl_port_lock(0);
    ui_pages_create(w, h);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL UI started (%dx%d, full-frame double buffer in PSRAM)", w, h);
    return ESP_OK;
}
