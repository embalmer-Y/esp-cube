/*
 * audio_mic.c —— 16k 单声道麦克风服务实现
 *
 * 数据路径: drv_audio_mic_read(48k 立体声, 与播放同 LRCK)
 *           → 逐样本 3:1 均值抽取(左声道) → 16k 单声道
 *
 * 实现要点:
 *   - 抽取状态(sum3/cnt3)跨调用保留, 块尾余数不丢帧;
 *   - ÷3 用乘法倒数替代((x*21845)>>16, 误差 ≤1 LSB);
 *   - 缓冲一次分配终身复用(约 1.9KB, 内部 RAM);
 *   - 暂停期(所有权让渡)I2S 仍在跑, 恢复后首先读到的是残留旧数据
 *     (一次块大小, ~10ms) —— 唤醒检测对这点陈旧不敏感, 不做冲刷。
 */
#include "audio_mic.h"

#include <string.h>
#include <stdlib.h>
#include "drv_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC48_CHUNK_FRAMES 480            /* 一次拉取 10ms@48k 立体声 */

static int16_t *s_buf48;                  /* MIC48_CHUNK_FRAMES*2 交错样本 */
static int s_n, s_pos;                    /* 本块样本数 / 消费游标 */
static int32_t s_sum3;                    /* 3 样本均值累加器(跨调用保留) */
static int s_cnt3;

static int inj_read(int16_t *mono, int frames);   /* 文件尾测试注入口 */
static volatile bool s_inj_on;                    /* 注入开关(文件尾定义 */

int audio_mic_read_16k(int16_t *mono, int frames)
{
    if (mono == NULL || frames <= 0) return 0;
    if (s_inj_on) return inj_read(mono, frames);   /* 测试注入优先 */
    if (s_buf48 == NULL) {
        s_buf48 = malloc(MIC48_CHUNK_FRAMES * 2 * sizeof(int16_t));
        if (s_buf48 == NULL) return 0;
    }
    int out = 0;
    while (out < frames) {
        if (s_pos >= s_n) {
            int got = drv_audio_mic_read(s_buf48, MIC48_CHUNK_FRAMES);
            if (got <= 0) break;          /* 欠载: 交还已产出部分 */
            s_n = got * 2;
            s_pos = 0;
        }
        while (out < frames && s_pos < s_n) {
            if ((s_pos & 1) == 0) {       /* 左声道(板载单麦) */
                s_sum3 += s_buf48[s_pos];
                if (++s_cnt3 == 3) {
                    mono[out++] = (int16_t)((s_sum3 * 21845) >> 16);
                    s_sum3 = 0;
                    s_cnt3 = 0;
                }
            }
            s_pos++;
        }
    }
    return out;
}


/* ================= 测试注入口(自动化验证用) =================
 * arm 后 read_16k 不再读真实麦克, 而是按实时节奏(20ms/320帧)供给
 * 注入 PCM(TTS 合成的"人声"), 耗尽自动回落真实麦克 —— 唤醒(WakeNet)
 * 与聆听(ASR)两条读路径都经过本函数, 一处注入两处可测。
 * 注入缓冲由调用方持有(PSRAM), 耗尽前不得释放。 */
static const int16_t *s_inj;
static volatile size_t s_inj_len, s_inj_pos;
static volatile bool s_inj_on;

void audio_mic_test_inject(const int16_t *pcm16k_mono, size_t frames)
{
    if (pcm16k_mono == NULL || frames == 0) return;
    s_inj = pcm16k_mono;
    s_inj_len = frames;
    s_inj_pos = 0;
    s_inj_on = true;
    printf("mic inject armed: %u frames (%.2fs)\n",
           (unsigned)frames, (double)frames / 16000.0);
}

static int inj_read(int16_t *mono, int frames)
{
    int out = 0;
    while (out < frames && s_inj_pos < s_inj_len) {
        mono[out++] = s_inj[s_inj_pos++];
    }
    if (s_inj_pos >= s_inj_len) s_inj_on = false;  /* 耗尽回落真实麦克 */
    /* 实时节奏: 16k 单声道 → frames/16 毫秒(VAD/ASR 时序才真实) */
    vTaskDelay(pdMS_TO_TICKS((frames + 15) / 16));
    return out;
}
