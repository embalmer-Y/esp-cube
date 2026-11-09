/*
 * app_player.c —— ICY/HTTP 网络流播放器实现
 *
 * 管线: esp_http_client(流式读) → esp_audio_codec(MP3/AAC 解码)
 *       → cube_resampler(→48k) → audio_mixer(AUDIO_SRC_NET 通道)
 *
 * 关键点:
 *   1. ICY 元数据: 请求头带 "Icy-MetaData: 1";IDF 的 esp_http_client 不提供
 *      响应头查询, 通过 HTTP_EVENT_ON_HEADER 事件捕获 icy-metaint 与
 *      Content-Type(大小写不敏感按协议头原样上报);
 *   2. 解码循环遵循 esp_audio_codec 语义: raw.consumed 推进输入,
 *      out.needed_size 提示输出缓冲不足(本实现缓冲 2048 帧已覆盖 MP3/AAC 最大帧);
 *   3. 断流自动重连(线性退避, ≤5 次)。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE              /* strcasestr(GNU 扩展, 头部嗅探用) */
#endif

#include "mp3dec.h"             /* libhelix-mp3 定点解码(chmorgan/esp-libhelix-mp3)。
                                 * 选型依据(GitHub 调研 2026-11-09):
                                 * - esp_audio_codec 的 MP3 即闭源 minimp3 派生,
                                 *   S31 实测 23ms/帧(仅1.1x实时, 必卡顿)且吞帧66%;
                                 * - S31 USE_ASM 汇编库不覆盖 MP3(esp-adf-libs #85);
                                 * - minimp3 上游存在未初始化 scratch 进 Huffman(#141)、
                                 *   嵌入式栈过大静默崩溃(#117) 等已知问题;
                                 * - libhelix 定点实现, 社区实测 ESP32 44.1k/2ch 实时无压力 */
#include "app_player.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include "esp_check.h"
#include "esp_log.h"
#include <esp_timer.h>
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_types.h"
#include "cubecore.h"
#include "audio_mixer.h"
#include "icy_stream.h"

static const char *TAG = "player";

#define PLAYER_MAX_RETRIES      5       /* 单次启播最大重连次数 */
#define PLAYER_RETRY_STEP_MS    1000    /* 重连退避步进(线性: step×次数) */

#define FEED_BUF_SIZE    4096                    /* 网络读块 */
#define PCM_BUF_FRAMES   2048                    /* 解码输出缓冲(帧, 覆盖MP3/AAC最大帧) */

static struct {
    char url[512];
    int  playlist_idx;                          /* 当前播放列表下标(-1=单次指定) */
    bool last_read_error;                       /* 上次结束是否为错误(n<0) */
    app_player_state_t state;
    TaskHandle_t task;
    volatile bool stop_req;
    audio_source_handle_t src;
    /* HTTP 响应头捕获(HTTP_EVENT_ON_HEADER 上下文写入) */
    char content_type[64];
    int  icy_metaint;
} s_player;

/* ================= HTTP 头捕获 ================= */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            strncpy(s_player.content_type, evt->header_value,
                    sizeof(s_player.content_type) - 1);
        } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_player.icy_metaint = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

/* ================= 解码器封装 ================= */
typedef struct {
    esp_audio_dec_handle_t dec;
    esp_audio_dec_info_t info;
    bool info_valid;
    cube_resampler_t rs;
    bool resample_needed;
    bool is_mp3;            /* MP3: libhelix 定点解码路径 */
    bool is_raw;            /* RAW PCM 直通(无解码) */
    HMP3Decoder mp3;        /* libhelix 解码器句柄 */
    int  frame_cnt;
    size_t total_decoded;   /* 解码器累计输出字节 */
} decoder_t;

/* 按 Content-Type/URL 选择解码器类型 */
static esp_audio_type_t pick_decoder_type(const char *content_type, const char *url)
{
    if (content_type[0]) {
        if (strcasestr(content_type, "aac") || strcasestr(content_type, "aacp")) {
            return ESP_AUDIO_TYPE_AAC;
        }
        if (strcasestr(content_type, "mpeg") || strcasestr(content_type, "mp3")) {
            return ESP_AUDIO_TYPE_MP3;
        }
    }
    /* URL 兜底嗅探 */
    if (strstr(url, ".aac") || strstr(url, "-aac")) return ESP_AUDIO_TYPE_AAC;
    return ESP_AUDIO_TYPE_MP3;                      /* 电台主流为 MP3 */
}

/* 打开解码器: MP3 → libhelix; 其余(AAC等) → esp_audio_codec */
static esp_err_t decoder_open(decoder_t *d, esp_audio_type_t type)
{
    memset(d, 0, sizeof(*d));
    d->is_mp3 = (type == ESP_AUDIO_TYPE_MP3);
    if (d->is_mp3) {
        d->mp3 = MP3InitDecoder();
        ESP_RETURN_ON_FALSE(d->mp3 != NULL, ESP_ERR_NO_MEM, TAG, "MP3InitDecoder");
        return ESP_OK;
    }
    esp_audio_dec_cfg_t cfg = {
        .type = type,
        .cfg = NULL,                            /* 流式解码用默认配置 */
        .cfg_sz = 0,
    };
    ESP_RETURN_ON_FALSE(esp_audio_dec_open(&cfg, &d->dec) == ESP_AUDIO_ERR_OK,
                        ESP_FAIL, TAG, "dec open failed (type=%d)", type);
    return ESP_OK;
}

/* ================= MP3 帧边界工具 ================= */
/* MPEG1 Layer3 帧长 = 144*bitrate/samplerate + padding;本项目的源(电台/文件)均为此格式 */
static int mp3_frame_len(const uint8_t *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return -1;
    int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    static const int brs[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
    static const int srs[4]  = { 44100,48000,32000,0 };
    int bi = (h[2] >> 4) & 0xF, si = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver != 3 || layer != 1 || bi == 0 || bi == 15 || si == 3) return -1;
    return 144 * brs[bi] * 1000 / srs[si] + pad;
}

/* 解码原始块 → (重采样到混音主时钟) → 推入混音器
 * pending 缓冲: 网络读取块尾部的"半帧"数据必须保留到下一块, 否则解码器
 * 流错位 → 周期性 "Not supported format" 与杂音(实测问题) */
static uint32_t s_pending_drops;    /* pending 溢出丢弃计数(数据损失可见化) */
static int64_t s_dec_us_total;      /* 解码耗时累计(诊断: esp解码器疑似~40ms/帧) */
static uint32_t s_dec_calls;
static int64_t s_dec_us_max;
static uint8_t s_pending[FEED_BUF_SIZE * 2];
static size_t  s_pending_len;
static int16_t s_res_out[PCM_BUF_FRAMES * 2];   /* 重采样独立输出(原地升采样会自覆盖) */

/* 解码输出 → (重采样到主时钟) → 写入混音器 */
static void output_pcm(decoder_t *d, int16_t *pcm, size_t frames)
{
    if (frames == 0) return;
    if (d->resample_needed) {
        if (d->info.channel == 1) {
            frames = cube_resampler_process_mono_to_stereo(
                    &d->rs, pcm, frames, s_res_out, PCM_BUF_FRAMES);
        } else {
            frames = cube_resampler_process_stereo(
                    &d->rs, pcm, frames, s_res_out, PCM_BUF_FRAMES);
        }
        audio_mixer_source_write(s_player.src, s_res_out,
                                 frames * 2 * sizeof(int16_t), 0);
    } else {
        audio_mixer_source_write(s_player.src, pcm,
                                 frames * 2 * sizeof(int16_t), 0);
    }
}


static void helix_feed(decoder_t *d);
static void decoder_feed(void *ctx, const void *data, size_t len);

/* icy_audio_sink_fn(ctx-last) → decoder_feed(ctx-first) 形参顺序适配 */
static void decoder_feed_sink(const void *data, size_t len, void *ctx)
{
    decoder_feed(ctx, data, len);
}

static void decoder_feed(void *ctx, const void *data, size_t len)
{
    decoder_t *d = (decoder_t *)ctx;
    static int16_t pcm_out[PCM_BUF_FRAMES * 2];

    size_t fed_frames_before = d->frame_cnt;
    if (len > sizeof(s_pending) - s_pending_len) {
        /* 异常积压(解码器长期不消费): 丢弃最旧数据保持流前进 */
        s_pending_drops++;
        ESP_LOGW(TAG, "pending RESET #%u, discarding %uB", (unsigned)s_pending_drops,
                 (unsigned)s_pending_len);
        s_pending_len = 0;
    }
    memcpy(s_pending + s_pending_len, data, len);
    s_pending_len += len;

    /* RAW PCM 直通: pending 即 PCM, 按帧输出, 4字节尾差保留 */
    if (d->is_raw) {
        size_t frames = s_pending_len / 4;
        if (frames > 0) {
            output_pcm(d, (int16_t *)s_pending, frames);
        }
        size_t used = frames * 4;
        if (used > 0) {
            memmove(s_pending, s_pending + used, s_pending_len - used);
            s_pending_len -= used;
        }
        return;
    }

    if (d->is_mp3) {
        /* libhelix: 自带同步字定位, 半帧返回 UNDERFLOW 且不消耗输入 */
        helix_feed(d);
        return;
    }

    /* esp_audio_codec 路径(AAC 等): 帧边界感知, 只喂到最后一个完整帧结尾 */
    size_t feed_len = s_pending_len;
    {
        size_t p = 0, last_ok = 0;
        while (p + 4 <= s_pending_len) {
            if (s_pending[p] == 0xFF && (s_pending[p + 1] & 0xE0) == 0xE0) {
                int flen = mp3_frame_len(&s_pending[p]);
                if (flen <= 0) break;
                if (p + flen <= s_pending_len) { last_ok = p + flen; p += flen; }
                else break;
            } else p++;
        }
        if (last_ok > 0) feed_len = last_ok;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = s_pending,
        .len = (uint32_t)feed_len,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = (uint8_t *)pcm_out,
        .len = (uint32_t)sizeof(pcm_out),
    };
    while (raw.len > 0) {
        int64_t t0 = esp_timer_get_time();
        esp_audio_err_t ret = esp_audio_dec_process(d->dec, &raw, &out);
        int64_t dt = esp_timer_get_time() - t0;
        s_dec_us_total += dt;
        s_dec_calls++;
        if (dt > s_dec_us_max) s_dec_us_max = dt;
        if (ret != ESP_AUDIO_ERR_OK) {
            /* 坏帧: 解码器未消费输入时, 按MP3同步字(0xFF Ex)扫描重同步,
             * 防止同一坏帧反复触发错误导致供流卡死 */
            if (raw.consumed == 0) {
                size_t skip = 1;
                while (skip + 1 < raw.len &&
                       !((raw.buffer[skip] == 0xFF) && (raw.buffer[skip + 1] & 0xE0) == 0xE0)) {
                    skip++;
                }
                raw.consumed = skip;
                out.decoded_size = 0;
            }
        }
        /* 关键钳制: 解码器内部缓存半帧时, consumed 可能大于本次提供的输入
         * (实测bug源), 严禁让 raw.len 下溢 */
        if (raw.consumed > raw.len) {
            raw.consumed = raw.len;
        }
        if (out.decoded_size > 0) {
            /* 首次成功解码: 记录流参数并配置重采样 */
            if (!d->info_valid) {
                esp_audio_dec_get_info(d->dec, &d->info);
                d->info_valid = true;
                cube_resampler_init(&d->rs, (float)d->info.sample_rate,
                                    (float)CONFIG_ESP_CUBE_MIXER_RATE);
                d->resample_needed = (d->info.sample_rate != (uint32_t)CONFIG_ESP_CUBE_MIXER_RATE)
                                     || (d->info.channel == 1);
                ESP_LOGI(TAG, "decoded: %luHz %dbit %dch", (unsigned long)d->info.sample_rate,
                         d->info.bits_per_sample, d->info.channel);
            }
            size_t frames = out.decoded_size /
                            (2u * (d->info.channel ? d->info.channel : 2));
            /* 前10帧逐帧幅度探针: 验证解码输出电平与位深解释是否正确 */
            d->frame_cnt++;
            d->total_decoded += out.decoded_size;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
            if (d->frame_cnt <= 10) {
                int16_t pk = 0;
                for (size_t i = 0; i < out.decoded_size / 2; i++) {
                    int16_t a = pcm_out[i] < 0 ? (int16_t)-pcm_out[i] : pcm_out[i];
                    if (a > pk) pk = a;
                }
                ESP_LOGI(TAG, "dec frame#%d: pk=%d size=%u consumed=%u",
                         d->frame_cnt, pk, out.decoded_size, raw.consumed);
            }
#endif
            if (d->resample_needed) {
                if (d->info.channel == 1) {
                    frames = cube_resampler_process_mono_to_stereo(
                            &d->rs, pcm_out, frames, s_res_out, PCM_BUF_FRAMES);
                } else {
                    frames = cube_resampler_process_stereo(
                            &d->rs, pcm_out, frames, s_res_out, PCM_BUF_FRAMES);
                }
                audio_mixer_source_write(s_player.src, s_res_out,
                                         frames * 2 * sizeof(int16_t), 0);
            } else {
                audio_mixer_source_write(s_player.src, pcm_out,
                                         frames * 2 * sizeof(int16_t), 0);
            }
        }
        if (raw.consumed == 0 && out.decoded_size == 0) {
            break;                                  /* 剩余不足以构成一帧 */
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    /* 保留尾部(半帧)到下一次: consumed 相对 feed_len(=s_pending_len) */
    size_t consumed_total = feed_len - raw.len;
    size_t tail = s_pending_len - consumed_total;
    if (tail > 0 && tail < s_pending_len) {
        memmove(s_pending, s_pending + consumed_total, tail);
    }
    s_pending_len = tail;
    if (d->frame_cnt == fed_frames_before && consumed_total > 1024) {
        /* 自愈: 消费了数据却0帧输出 = 解码器状态异常, 复位后从同步字重新工作 */
        ESP_LOGW(TAG, "decoder stall, reset (consumed %uB)", (unsigned)consumed_total);
        esp_audio_dec_reset(d->dec);
    }
}

/* MP3 路径: libhelix 逐帧解码。
 * 半帧处理: MP3Decode 需要完整帧在缓冲内, 不足时返回 INDATA_UNDERFLOW 且
 * 不消耗输入 —— 半帧自然留在 pending, 下次凑齐再解(无需预扫描)。 */
static void helix_feed(decoder_t *d)
{
    static int16_t mp3pcm[1152 * 2];
    size_t off = 0;
    for (;;) {
        int left = (int)(s_pending_len - off);
        if (left < 8) break;                        /* 不足一个最小帧头+数据 */

        int sync = MP3FindSyncWord(s_pending + off, left);
        if (sync < 0) {
            off = s_pending_len;                    /* 无同步字: 剩余全是垃圾 */
            break;
        }
        off += (size_t)sync;
        left = (int)(s_pending_len - off);

        uint8_t *rp = s_pending + off;
        int64_t t0 = esp_timer_get_time();
        int rc = MP3Decode(d->mp3, &rp, &left, mp3pcm, 0);
        int64_t dt = esp_timer_get_time() - t0;
        s_dec_us_total += dt;
        s_dec_calls++;
        if (dt > s_dec_us_max) s_dec_us_max = dt;
        if (rc == ERR_MP3_INDATA_UNDERFLOW || rc == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;                                  /* 半帧: 保留 pending 等更多数据 */
        }
        if (rc != ERR_MP3_NONE) {
            off++;                                  /* 坏帧: 跳过此同步字重同步 */
            continue;
        }

        size_t consumed = (size_t)(rp - (s_pending + off));
        off += consumed;

        if (!d->info_valid) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(d->mp3, &fi);
            memset(&d->info, 0, sizeof(d->info));
            d->info.sample_rate = (uint32_t)fi.samprate;
            d->info.channel = (uint8_t)(fi.nChans ? fi.nChans : 1);
            d->info_valid = true;
            cube_resampler_init(&d->rs, (float)fi.samprate, (float)CONFIG_ESP_CUBE_MIXER_RATE);
            d->resample_needed = ((uint32_t)fi.samprate != (uint32_t)CONFIG_ESP_CUBE_MIXER_RATE)
                                 || (fi.nChans == 1);
            ESP_LOGI(TAG, "decoded: %dHz %dch (libhelix)", fi.samprate, fi.nChans);
        }

        d->frame_cnt++;
        d->total_decoded += consumed;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
        if (d->frame_cnt <= 10) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(d->mp3, &fi);
            int16_t pk = 0;
            for (int i = 0; i < fi.outputSamps; i++) {
                int16_t a = mp3pcm[i] < 0 ? (int16_t)-mp3pcm[i] : mp3pcm[i];
                if (a > pk) pk = a;
            }
            ESP_LOGI(TAG, "dec frame#%d: pk=%d bytes=%u", d->frame_cnt, pk, (unsigned)consumed);
        }
#endif
        /* outputSamps = 总交错样本数(帧数 = outputSamps/channels) */
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d->mp3, &fi);
        int ch = fi.nChans ? fi.nChans : 1;
        output_pcm(d, mp3pcm, (size_t)(fi.outputSamps > 0 ? fi.outputSamps / ch : 0));
    }
    if (off > 0 && off <= s_pending_len) {
        memmove(s_pending, s_pending + off, s_pending_len - off);
        s_pending_len -= off;
    } else if (off > s_pending_len) {
        s_pending_len = 0;
    }
}

/* ================= 播放列表(原型: 局域网HTTP曲目轮播) =================
 * 最终产品由 app_ai/电台管理模块下发列表; 局域网源保证供给≥10×实时 */
/* PCM 直通素材(48k/16bit/stereo, 与混音引擎同规格, 零解码零重采样):
 * 用于验证输出链路与听感; MP3 解码器攻坚(M1.5)完成后切回压缩流 */
static const char *s_playlist[] = {
    "http://192.168.2.91:8000/song.mp3",
    "http://192.168.2.91:8000/song2.mp3",
    "http://192.168.2.91:8000/song3.mp3",
    "http://192.168.2.91:8000/song4.mp3",
};
#define PLAYLIST_NUM (sizeof(s_playlist) / sizeof(s_playlist[0]))

/* ================= 播放任务 ================= */
#if CONFIG_ESP_CUBE_TEST_TONE
/* 440Hz 正弦测试音: 直接生成 PCM 注入混音器, 用于验证 DAC→功放→喇叭输出链路。
 * 幅度 12000(约-8.7dBFS), 双声道 48k, 与混音引擎实时节奏阻塞同步。 */
/* 440Hz@48k: 每样本相位推进 440/48000 周期 → 50样本恰好22整周期(无漂移),
 * 用50点LUT + 整数相位步进(22), 查表代替逐样本sinf(实测sinf软浮点仅575ms/s,
 * 供流速度低于实时消耗会导致测试音本身周期性断流) */
#define TONE_LUT_N   50
#define TONE_STEP    22
static int16_t s_tone_lut[TONE_LUT_N];

static void test_tone_init(void)
{
    for (int n = 0; n < TONE_LUT_N; n++) {
        s_tone_lut[n] = (int16_t)(12000.0f * sinf(2.0f * 3.14159265f * 440.0f
                                                  * (float)n / 48000.0f));
    }
}

static void test_tone_loop(void)
{
    static int16_t tone[4800 * 2];                  /* 100ms @48k(批量写, 排除每调用开销) */
    static int idx;
    test_tone_init();
    ESP_LOGI(TAG, "TEST TONE mode: 440Hz @-8.7dBFS (Kconfig ESP_CUBE_TEST_TONE)");
    uint32_t loops = 0, partial = 0;
    int64_t last_report = esp_timer_get_time();
    for (;;) {
        for (int i = 0; i < 4800; i++) {
            int16_t v = s_tone_lut[idx];
            tone[i * 2] = v;
            tone[i * 2 + 1] = v;
            idx += TONE_STEP;
            if (idx >= TONE_LUT_N) idx -= TONE_LUT_N;
        }
        size_t w = audio_mixer_source_write(s_player.src, tone, sizeof(tone),
                                            pdMS_TO_TICKS(50));
        if (w != sizeof(tone)) partial++;
        loops++;
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 2000000) {         /* 每2秒报告生产速率 */
            ESP_LOGI(TAG, "tone producer: %lu loops/2s (%lums audio/2s), partial=%u",
                     (unsigned long)loops, (unsigned long)loops * 100,
                     (unsigned)partial);
            loops = 0; partial = 0;
            last_report = now;
        }
    }
}
#endif

static void player_task(void *arg)
{
    static uint8_t feed[FEED_BUF_SIZE];
    decoder_t dec = { 0 };
    int retries = 0;

#if CONFIG_ESP_CUBE_TEST_TONE
    audio_mixer_set_source_active(s_player.src, true);
    s_player.state = PLAYER_PLAYING;
    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
    test_tone_loop();
    vTaskDelete(NULL);
    return;
#endif

    esp_audio_dec_register_default();               /* 注册内置 MP3/AAC 等解码器 */

    s_player.state = PLAYER_CONNECTING;
    cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));

    while (!s_player.stop_req && retries < PLAYER_MAX_RETRIES) {
        esp_http_client_config_t http_cfg = {
            .url = s_player.url,
            .timeout_ms = 6000,
            .buffer_size = FEED_BUF_SIZE,
            .keep_alive_enable = true,
            .event_handler = http_event_cb,         /* 捕获响应头 */
        };
        esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
        if (http == NULL) { retries++; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        /* ICY 元数据请求头(响应头的 icy-metaint 经事件回调捕获) */
        esp_http_client_set_header(http, "Icy-MetaData", "1");
        s_player.content_type[0] = 0;
        s_player.icy_metaint = 0;
        esp_err_t err = esp_http_client_open(http, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed(%s), retry %d", esp_err_to_name(err), retries + 1);
            esp_http_client_cleanup(http);
            retries++;
            vTaskDelay(pdMS_TO_TICKS(PLAYER_RETRY_STEP_MS * 2 * retries));
            continue;
        }
        esp_http_client_fetch_headers(http);        /* 触发 ON_HEADER 事件流 */

        dec.is_raw = (strstr(s_player.url, ".raw") != NULL);
        esp_err_t derr = ESP_OK;
        if (dec.is_raw) {
            /* RAW: 无需解码器, 固定 48k/16bit/stereo(与混音引擎同规格) */
            memset(&dec.info, 0, sizeof(dec.info));
            dec.info.sample_rate = 48000;
            dec.info.channel = 2;
            dec.info_valid = true;
            dec.resample_needed = false;
        } else {
            derr = decoder_open(&dec, pick_decoder_type(s_player.content_type, s_player.url));
        }
        if (derr != ESP_OK) {
            esp_http_client_cleanup(http);
            s_player.state = PLAYER_ERROR;
            cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
            break;
        }
        audio_mixer_set_source_active(s_player.src, true);
        s_player.state = PLAYER_PLAYING;
        retries = 0;                                /* 连接成功, 重置重试 */
        cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
        ESP_LOGI(TAG, "playing %s (%s) metaint=%d",
                 s_player.url, s_player.content_type, s_player.icy_metaint);

        size_t total_read = 0;
        int read_calls = 0;
        /* ---- 主循环: 读网络 → ICY 切分(协议层) → 解码(音频层) → 推混音器 ---- */
        icy_state_t icy;
        icy_reset(&icy, s_player.icy_metaint);
        while (!s_player.stop_req) {
            int n = esp_http_client_read(http, (char *)feed, FEED_BUF_SIZE);
            total_read += (n > 0) ? n : 0;
            read_calls++;
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
            if (read_calls <= 30) {
                ESP_LOGI(TAG, "read#%d: n=%d total=%u", read_calls, n, (unsigned)total_read);
            }
#endif
            if (n <= 0) {
                /* n==0 且数据未收完 = 读取超时/停滞(非真EOF) → 重连同一URL;
                 * n==0 且数据收完 = 真EOF → 播放列表切下一首; n<0 → 错误重连 */
                bool complete = esp_http_client_is_complete_data_received(http);
                s_player.last_read_error = (n < 0) || !complete;
                ESP_LOGW(TAG, "read ended: total_read=%u n=%d complete=%d",
                         (unsigned)total_read, n, complete);
                break;
            }
            icy_feed_chunk(&icy, feed, (size_t)n, decoder_feed_sink, &dec);
#if CONFIG_ESP_CUBE_AUDIO_DEBUG
            {
                static int probe;
                if (++probe >= 40) {
                    probe = 0;
                    ESP_LOGI(TAG, "probe: dec_calls=%u dec_avg=%lldus pending=%u",
                             (unsigned)s_dec_calls,
                             s_dec_calls ? s_dec_us_total / s_dec_calls : 0,
                             (unsigned)s_pending_len);
                }
            }
#endif
        }

        /* ---- 清理本次连接 ---- */
        if (s_dec_calls > 0) {
            ESP_LOGD(TAG, "decode timing: calls=%u avg=%lldus max=%lldus",
                     s_dec_calls, s_dec_us_total / s_dec_calls, s_dec_us_max);
        }
        s_dec_us_total = 0; s_dec_calls = 0; s_dec_us_max = 0;
        ESP_LOGI(TAG, "stream end: frames=%u decoded=%uB",
                 (unsigned)dec.frame_cnt, (unsigned)dec.total_decoded);
        audio_mixer_set_source_active(s_player.src, false);
        if (dec.dec) {
            esp_audio_dec_close(dec.dec);
            dec.dec = NULL;
        }
        if (dec.mp3) {
            MP3FreeDecoder(dec.mp3);
            dec.mp3 = NULL;
        }
        dec.info_valid = false;
        s_pending_len = 0;
        esp_http_client_close(http);
        esp_http_client_cleanup(http);

        if (!s_player.stop_req) {
            /* EOF → 播放列表轮播下一首; 读取错误 → 原地重试同一URL */
            if (s_player.playlist_idx >= 0 && !s_player.last_read_error) {
                s_player.playlist_idx = (s_player.playlist_idx + 1) % PLAYLIST_NUM;
                strncpy(s_player.url, s_playlist[s_player.playlist_idx],
                        sizeof(s_player.url) - 1);
                ESP_LOGI(TAG, "playlist → [%d] %s", s_player.playlist_idx, s_player.url);
                retries = 0;
            } else {
                ESP_LOGW(TAG, "stream ended, reconnecting...");
                retries++;
            }
            vTaskDelay(pdMS_TO_TICKS(PLAYER_RETRY_STEP_MS * (retries > 0 ? retries : 1)));
        }
    }

    esp_audio_dec_unregister_default();

    if (!s_player.stop_req) {
        s_player.state = PLAYER_ERROR;
        cube_event_publish(CUBE_EVT_PLAYER_STATE, &s_player.state, sizeof(int));
    }
    s_player.task = NULL;
    vTaskDelete(NULL);
}

/* ================= 对外接口 ================= */
esp_err_t app_player_start(const char *url)
{
    if (url == NULL) {                          /* 无参: 从播放列表头开始轮播 */
        s_player.playlist_idx = 0;
        url = s_playlist[0];
    } else {
        s_player.playlist_idx = -1;             /* 显式指定: 不参与轮播 */
    }
    ESP_RETURN_ON_FALSE(strlen(url) > 8, ESP_ERR_INVALID_ARG, TAG, "bad url");
    if (s_player.src == NULL) {
        audio_source_cfg_t scfg = {
            .name = "net",
            .type = AUDIO_SRC_NET,
            .buffer_ms = 4000,      /* 4秒 PSRAM 环形缓冲(1.5MB): 吸收网络长时间停顿 */
            .prefill_ms = CONFIG_ESP_CUBE_NET_PREFILL_MS,
            .blocking = true,       /* 播放任务可阻塞, 按实时节奏供给混音器 */
        };
        s_player.src = audio_mixer_add_source(&scfg);
        ESP_RETURN_ON_FALSE(s_player.src != NULL, ESP_ERR_NO_MEM, TAG, "add source");
    }
    /* 请求切换: 先停旧任务(含等待退出, 避免两任务抢同一解码器) */
    app_player_stop();
    strncpy(s_player.url, url, sizeof(s_player.url) - 1);
    s_player.stop_req = false;
    /* 解码任务放 core1: core0 被 mixer(20)/WiFi(23)/BT(23+) 高优任务占据,
     * 解码(prio12)在 core0 抢不到 CPU → 实测供给仅 0.5×实时(40ms/帧) */
    if (xTaskCreatePinnedToCore(player_task, "player", 12288, NULL, 12,
                                &s_player.task, 1) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_player_stop(void)
{
    if (s_player.task != NULL) {
        s_player.stop_req = true;
        for (int i = 0; i < 20 && s_player.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_player.state = PLAYER_IDLE;
    return ESP_OK;
}

app_player_state_t app_player_get_state(void)
{
    return s_player.state;
}

const char *app_player_get_url(void)
{
    return s_player.url;
}
