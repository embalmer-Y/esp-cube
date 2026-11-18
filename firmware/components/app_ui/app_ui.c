/*
 * app_ui.c —— LVGL 集成与显示绑定(L4 表现层装配)
 *
 * esp_lvgl_port 负责: LVGL 任务(固定 core1)/tick/flush 到 esp_lcd 面板。
 * 本文件只做装配; 页面内容在 ui/ui_pages.c(平台无关, 只依赖 ui_model.h)。
 * 显示句柄由 main 注入(docs/06 §2.1: L4 不依赖 drv_display)。
 */
#include "app_ui.h"
#include "ui_pages.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "cube_tasks.h"
#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "ui";

esp_err_t app_ui_start(const app_ui_boot_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && cfg->io_handle != NULL &&
                        cfg->panel_handle != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "boot cfg");

    /* ---- 1. LVGL 运行环境 ----
     * v2.9 的任务参数在结构体里(Kconfig 无效): 绑核1(渲染与音频分核),
     * 栈 16KB(FFT/canvas 绘制), 优先级 4(低于混音器, 不抢占实时音频)。 */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = CUBE_TASK_LVGL_PRIO;
    lvgl_cfg.task_stack = CUBE_TASK_LVGL_STACK;
    lvgl_cfg.task_affinity = CUBE_TASK_LVGL_CORE;  /* 与 core0 音频/蓝牙分核 */
    /* §52: EFC 解码用 PIE 协处理器, 其上下文保存区 carve 在任务栈内,
     * 而 esp.vst 不能写 LP SRAM(RTCRAM) → 栈必须落 HP SRAM。DMA caps
     * 不含 RTCRAM(Kconfig 明示 "without DMA"), 精确排除之 */
    lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    lvgl_cfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init");

    /* ---- 2. 显示设备绑定(接口方式由装配层注入) ---- */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = (esp_lcd_panel_io_handle_t)cfg->io_handle,
        .panel_handle = (esp_lcd_panel_handle_t)cfg->panel_handle,
        .buffer_size = (uint32_t)cfg->h_res * cfg->v_res,  /* 全帧像素(RGB565) */
        .double_buffer = true,
        .hres = (uint32_t)cfg->h_res,
        .vres = (uint32_t)cfg->v_res,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .buff_dma = false,
        },
    };

    lv_display_t *disp = NULL;
    if (cfg->iface == UI_DISP_IF_DSI) {
        /* MIPI-DSI DPI 面板(P4C5): 直接模式画进 DPI 自身帧缓冲, 换帧与
         * vsync 对齐(Kaleido dsi-lvgl V-35R 实测口径: avoid_tearing 必须
         * 搭配 direct_mode, 否则 flush 死等信号量); 并行总线无字节序问题 */
        disp_cfg.flags.swap_bytes = false;
        disp_cfg.flags.direct_mode = true;
        lvgl_port_display_dsi_cfg_t dsi_cfg = {
            .flags = { .avoid_tearing = true },
        };
        disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    } else {
        /* QSPI 面板(S31): full_refresh=1 每次提交整帧(坐标天然偶对齐,
         * 满足 CO5300 约束); swap_bytes=1 QSPI 按大端发送 RGB565;
         * 缓冲 2×460×460×2B≈846KB 落 PSRAM(S31 PSRAM 专用控制器,
         * 不与 LCD 争带宽)。 */
        disp_cfg.flags.swap_bytes = true;       /* 帧序异常时优先核对此项 */
        disp_cfg.flags.full_refresh = true;
        disp = lvgl_port_add_disp(&disp_cfg);
    }
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "add disp");

    /* ---- 3. 页面构建 ---- */
    lvgl_port_lock(0);
    ui_pages_create(cfg->h_res, cfg->v_res);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL UI started (%dx%d, full-frame double buffer in PSRAM)",
             cfg->h_res, cfg->v_res);
    return ESP_OK;
}
