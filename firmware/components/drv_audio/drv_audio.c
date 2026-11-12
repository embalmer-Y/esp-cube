/*
 * drv_audio.c —— ES8311 + I2S 驱动实现(S31 / P4C5 双板同一份)
 *
 * 两板同款音频链(ES8311 + NS4150B + 模拟麦), 差异全部收敛在 boardcfg:
 *   - 引脚: board_pins_s31.h / board_pins_p4c5.h;
 *   - DMA 形状与 auto_clear: BOARD_AUDIO_DMA_* (S31 6×240 / P4C5 8×480);
 *   - PA 怪癖: P4C5 的 es8311 驱动 pa_pin 自动配置不生效(Kaleido V-38
 *     实证), BOARD_AMP_MANUAL_PA → codec open 后手工补配推挽输出;
 *   - esp_codec_dev 双目标统一 1.6.2(原 S31 用 2.0.0-beta5 = 语音 TLSF
 *     堆损坏头号嫌疑, 详见 main/idf_component.yml 注释)。
 *
 * 实现要点(沿袭):
 *   1. I2S 全双工: TX/RX 同一控制器同一 LRCK(ES8311 单接口约束),
 *      播放 48k;录音通道读出后由 app_voice 降采样到 16k 喂 esp-sr;
 *   2. esp_codec_dev 抽象层: 统一音量/静音/模式管理, 与乐鑫官方 BSP 同构;
 *   3. PA 上电时序: codec open → 设置音量(数字静音) → PA enable → 解除静音,
 *      避免 NS4150B 上电瞬间的直流阶跃产生爆音。
 */
#include "drv_audio.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "board_pins.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    /* DMA 形状按板(boardcfg): S31 6×240(30ms, 共存抖动实测) /
     * P4C5 8×480(80ms, Kaleido/radio 实证)+auto_clear(欠载静音防复读) */
    chan_cfg.dma_desc_num = BOARD_AUDIO_DMA_DESC;
    chan_cfg.dma_frame_num = BOARD_AUDIO_DMA_FRAMES;
#if BOARD_AUDIO_I2S_AUTO_CLEAR
    chan_cfg.auto_clear = true;
#endif
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

    /* ================= 2. codec 数据/控制接口 ================= */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = s_rx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    /* I2C 控制(ES8311; 两版组件均要求外部先建好 I2C master bus) */
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

    /* ================= 3. ES8311 codec 设备(esp_codec_dev 1.6.2 扁平配置,
     * radio-p4c5/Kaleido 同款 P4C5 板实证; S31 同芯片同款用法) ================= */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   /* 播放+录音(语音链) */
        .master_mode = false,                   /* ESP 为 I2S master → codec slave */
        .use_mclk = true,
        .digital_mic = false,                   /* 模拟麦 → ES8311 ADC */
        .pa_pin = BOARD_PIN_AMP_EN,
        .pa_reverted = false,
        .hw_gain = { .pa_voltage = 5.0 },
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
#ifdef BOARD_AUDIO_MIC_GAIN_DB
    /* 麦克风增益(板级常量; P4C5 硅麦链 30dB 实证, 见 board_pins_p4c5.h) */
    esp_codec_dev_set_in_gain(s_codec, BOARD_AUDIO_MIC_GAIN_DB);
#endif

#ifdef BOARD_AMP_MANUAL_PA
    /* P4C5 板怪癖(Kaleido V-38 实证): es8311 驱动对 pa_pin 的 gpio 配置在
     * 本板不生效(疑与启动期全脚 sleep 隔离冲突) → open 后手工补配推挽输出
     * 并保持低; PA 仍由 main 在混音任务就绪后 drv_audio_pa_set(true) 打开 */
    gpio_config_t pa_gpio = {
        .pin_bit_mask = 1ULL << BOARD_PIN_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    (void)gpio_config(&pa_gpio);
    gpio_set_level(BOARD_PIN_AMP_EN, 0);
#endif

    ESP_LOGI(TAG, "ES8311 ready: %dHz/%dch (board=%s, PA on GPIO%d)",
             s_cfg.sample_rate, s_cfg.channels, BOARD_NAME, BOARD_PIN_AMP_EN);
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


