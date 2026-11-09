/*
 * drv_audio.c —— ES8311 + I2S 驱动实现
 *
 * 实现要点:
 *   1. I2S 全双工: TX/RX 同一控制器同一 LRCK(ES8311 单接口约束),
 *      播放 48k;录音通道读出后由 app_voice 降采样到 16k 喂 esp-sr;
 *   2. esp_codec_dev 抽象层: 统一音量/静音/模式管理, 与乐鑫官方 BSP 同构;
 *   3. PA 上电时序: codec open → 设置音量(数字静音) → PA enable → 解除静音,
 *      避免 NS4150B 上电瞬间的直流阶跃产生爆音。
 *
 * ⚠ I2C/I2S 引脚为【待核对】占位值, 见 board_pins.h 与接线指南。
 */
#include "drv_audio.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "board_pins.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "drv_audio";

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static drv_audio_cfg_t s_cfg;
static bool s_pa_on = false;

esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg)
{
    if (s_codec != NULL) {
        return ESP_OK;                          /* 幂等 */
    }
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg NULL");
    s_cfg = *cfg;
    if (s_cfg.channels <= 0) s_cfg.channels = 2;

    /* ================= 1. I2S 全双工通道 ================= */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 特大 DMA 缓冲抗 WiFi/BT 抖动: 6 buf × 240 帧 */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)s_cfg.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_PIN_CODEC_MCLK,
            .bclk = BOARD_PIN_CODEC_SCLK,
            .ws   = BOARD_PIN_CODEC_WS,
            .dout = BOARD_PIN_CODEC_DOUT,
            .din  = BOARD_PIN_CODEC_DIN,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init tx");
    /* RX 共享同一时钟配置(全双工同 LRCK) */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable rx");

    /* ================= 2. codec 数据/控制接口(esp_codec_dev v2 API) ================= */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = s_rx,
        .clk_src = 0,                           /* 0 = 默认时钟源 */
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    /* I2C 控制(ES8311; v2 组件要求外部先建好 I2C master bus) */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_CODEC_I2C_SDA,
        .scl_io_num = BOARD_PIN_CODEC_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 原型期免外部上拉 */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c bus");

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .addr = BOARD_CODEC_I2C_ADDR,           /* 8位形式(内部>>1 得7位地址) */
        .bus_handle = i2c_bus,
        .clock_speed_hz = 0,                    /* 0 = 默认速率 */
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);

    /* ================= 3. ES8311 codec 设备(v2: 硬件分块配置) ================= */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = NULL,                        /* 无额外 GPIO 控制 */
        /* ESP32-S31 为 I2S master → codec 为 slave;使用外部 MCLK */
        .sys_cfg = { .is_master = false, .no_mclk = false },
        .adc_cfg = { .digital_mic = false },    /* 模拟麦克风输入(板载) */
        .dac_cfg = { .ref_enable = false, .ref_dac_ch = -1,
                     .real_adc_data_ch = -1, .headphone = AUDIO_HW_HEADPHONE_AUTO },
        .pa_cfg = { .pa_pin = (BOARD_PIN_AMP_EN >= 0) ? BOARD_PIN_AMP_EN : -1,
                    .pa_active_low = false },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_FAIL, TAG, "codec dev new failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = (uint32_t)s_cfg.sample_rate,
        .channel = (uint8_t)s_cfg.channels,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec, &fs), TAG, "codec open");
    esp_codec_dev_set_out_vol(s_codec, 0);          /* 先静音, 由 mixer 控制实际音量 */

    ESP_LOGI(TAG, "ES8311 ready: %dHz/%dch (PA on GPIO%d)", s_cfg.sample_rate,
             s_cfg.channels, BOARD_PIN_AMP_EN);
    return ESP_OK;
}

int drv_audio_output(const int16_t *samples, int frames)
{
    if (s_codec == NULL || samples == NULL || frames <= 0) {
        return 0;
    }
    /* esp_codec_dev_write 为阻塞语义: DMA 环满时等待 —— 即混音器节拍 */
    size_t bytes = (size_t)frames * s_cfg.channels * sizeof(int16_t);
    int ret = esp_codec_dev_write(s_codec, (void *)samples, bytes);
    return ret == ESP_CODEC_DEV_OK ? frames : 0;
}

int drv_audio_mic_read(int16_t *buf, int frames)
{
    if (s_codec == NULL || buf == NULL || frames <= 0) {
        return 0;
    }
    size_t bytes = (size_t)frames * s_cfg.channels * sizeof(int16_t);
    int ret = esp_codec_dev_read(s_codec, buf, bytes);
    return ret == ESP_CODEC_DEV_OK ? frames : 0;
}

esp_err_t drv_audio_set_volume(int vol)
{
    if (s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return esp_codec_dev_set_out_vol(s_codec, vol);
}

esp_err_t drv_audio_pa_set(bool on)
{
    if (s_codec == NULL || s_pa_on == on) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    if (on) {
        /* PA 使能由 esp_codec_dev 的 pa_pin 管理仅在 open/close 时;
         * 运行时切换: 直接拉 GPIO(高有效, 与 NS4150B SD 引脚约定一致) */
        if (BOARD_PIN_AMP_EN >= 0) {
            gpio_set_direction(BOARD_PIN_AMP_EN, GPIO_MODE_OUTPUT);
            gpio_set_level(BOARD_PIN_AMP_EN, 1);
        }
    } else {
        if (BOARD_PIN_AMP_EN >= 0) {
            gpio_set_level(BOARD_PIN_AMP_EN, 0);
        }
    }
    s_pa_on = on;
    ESP_LOGI(TAG, "PA %s", on ? "ON" : "OFF");
    return ret;
}
