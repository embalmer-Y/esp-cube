/*
 * drv_audio.h —— 音频驱动层(ES8311 codec + I2S)
 *
 * 职责:
 *   - I2S 全双工通道(TX=播放 48k/2ch, RX=录音同 LRCK 48k, 由上层降采样到 16k);
 *   - ES8311 初始化(esp_codec_dev 抽象), 音量/静音/功放(PA)时序;
 *   - 对 app_audio(混音器)提供阻塞式读写 —— 混音器以写入为节拍。
 *
 * 板载链路(ESP32-S31-Function-CoreBoard-1):
 *   ESP32-S31 I2S → ES8311(DAC) → NS4150B(3W D类) → 4Ω 喇叭
 *   ES8311(ADC) ← 板载麦克风
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;         /* 播放采样率(混音引擎主时钟, 默认 48000) */
    int channels;            /* 2 */
    int mic_sample_rate;     /* 录音采样率(与播放共享 LRCK; 默认与 sample_rate 相同) */
} drv_audio_cfg_t;

/* 初始化 codec + I2S(幂等); 成功后即可以 drv_audio_output 播放 */
esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg);

/* 阻塞式输出 PCM(交错 int16, 帧数 frames; 返回实际写入帧数)
 * 混音器以本函数为节拍: DMA 缓冲欠载时阻塞, 天然形成 10ms  pacing */
int drv_audio_output(const int16_t *samples, int frames);

/* 录音读取(交错 int16 立体声, 与播放同 LRCK=48k; 返回帧数 0=失败)。
 * 16k 单声道降采样由语音前端自理(app_voice/ai_session)。 */
int drv_audio_mic_read(int16_t *buf, int frames);

/* 音量(0-100): codec 数字音量 + PA 增益预留 */
esp_err_t drv_audio_set_volume(int vol);

/* 功放使能: 上电顺序 = codec 就绪 → 音量静音位设置 → PA on(爆音抑制) */
esp_err_t drv_audio_pa_set(bool on);

#ifdef __cplusplus
}
#endif
