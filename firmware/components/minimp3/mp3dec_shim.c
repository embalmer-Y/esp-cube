/*
 * mp3dec_shim.c —— helix→minimp3 语义映射(docs/19 §55.20, CC0 后端)
 *
 * 映射表(逐条对齐 app_player.c helix_feed 的消费语义):
 *   minimp3 samples>0, frame_bytes>0  → ERR_MP3_NONE, *pp 前移 frame_bytes
 *   minimp3 samples=0, frame_bytes>0  → ERR_MP3_NOSYNC(垃圾已吞), *pp 前移
 *                                       (app: 非 NONE → off++ 重同步, 兼容)
 *   minimp3 frame_bytes=0             → ERR_MP3_INDATA_UNDERFLOW, 不前移
 *                                       (app: break 留 pending 等数据, 兼容)
 * 比特池(bit reservoir)由 minimp3 内部维护——helix 的 MAINDATA_UNDERFLOW
 * 情形并入 frame_bytes=0 分支。
 * 输出: mp3d_sample_t 默认 int16_t, 零转换。
 */
#define MINIMP3_IMPLEMENTATION
/* 符号避撞: esp_audio_codec(乐鑫闭源)内部嵌有另一份 minimp3, 其
 * libesp_audio_codec.a 已导出 mp3dec_* —— 本副本全量前缀重命名
 * (§55.20 链接期实测 multiple definition 修复)。 */
#define mp3dec_init         espcube_mp3dec_init
#define mp3dec_decode_frame espcube_mp3dec_decode_frame
#define mp3dec_f32_to_s16   espcube_mp3dec_f32_to_s16
#include "../minimp3.h"

#include "mp3dec.h"

#include <stdlib.h>
#include <string.h>

/* 真实解码器状态(opaque 后面是真体, 避免 shim 头暴露 minimp3 类型) */
struct HMP3Decoder_s {
    void *opaque;
    mp3dec_t dec;
    mp3dec_frame_info_t fi;
    int samples;
};

HMP3Decoder MP3InitDecoder(void)
{
    struct HMP3Decoder_s *d = calloc(1, sizeof(*d));
    if (d != NULL)
        mp3dec_init(&d->dec);
    return (HMP3Decoder)d;
}

void MP3FreeDecoder(HMP3Decoder dec)
{
    free(dec);
}

int MP3FindSyncWord(const uint8_t *buf, int len)
{
    /* MPEG 帧头同步扫描: 11 位 sync + 版本/层非保留值。
     * 解码器会再验证, 这里只需够准的候选定位。 */
    for (int i = 0; i + 1 < len; i++) {
        uint8_t b1 = buf[i + 1];
        if (buf[i] != 0xFF || (b1 & 0xE0) != 0xE0)
            continue;
        if (((b1 >> 3) & 3) == 1)   /* 版本 01 保留 */
            continue;
        if (((b1 >> 1) & 3) == 0)   /* 层 00 保留(helix 同口径) */
            continue;
        return i;
    }
    return -1;
}

int MP3Decode(HMP3Decoder dec, uint8_t **pp, int *plen, int16_t *pcm,
              int use_size)
{
    (void)use_size;
    struct HMP3Decoder_s *d = (struct HMP3Decoder_s *)dec;
    mp3dec_frame_info_t fi;

    d->samples = mp3dec_decode_frame(&d->dec, *pp, *plen, pcm, &fi);
    d->fi = fi;
    if (fi.frame_bytes > 0) {
        *pp += fi.frame_bytes;
        *plen -= fi.frame_bytes;
        return d->samples > 0 ? ERR_MP3_NONE : ERR_MP3_NOSYNC;
    }
    return ERR_MP3_INDATA_UNDERFLOW;
}

void MP3GetLastFrameInfo(HMP3Decoder dec, MP3FrameInfo *info)
{
    struct HMP3Decoder_s *d = (struct HMP3Decoder_s *)dec;
    if (info == NULL)
        return;
    memset(info, 0, sizeof(*info));
    info->samprate = d->fi.hz;
    info->nChans = d->fi.channels;
    info->bitsPerSample = 16;
    info->bitrateKbps = d->fi.bitrate_kbps;
    info->outputSamps = d->samples * d->fi.channels;
}
