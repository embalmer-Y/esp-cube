/*
 * mp3dec.h —— helix 兼容 API 垫片头(minimp3 CC0 后端)
 *
 * 背景(docs/19 §55.19/§55.20): 旧 vendored mp3_helix 为 RealNetworks RPSL
 * 许可, 与本项目 AGPL-3.0 不兼容(FSF 判定)——用户裁决替换为 lieff/minimp3
 * (CC0 公有领域)。本组件对外暴露 app_player 既用的 helix 五函数子集,
 * 调用面零改动; 语义映射见 mp3dec_shim.c 头注。
 */
#ifndef MINIMP3_HELIX_SHIM_H
#define MINIMP3_HELIX_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HMP3Decoder_s *HMP3Decoder;   /* 不完整类型, 实现在 .c */

/* 错误码(helix 语义子集; app_player 只区分 NONE/两类 UNDERFLOW) */
enum {
    ERR_MP3_NONE = 0,
    ERR_MP3_INDATA_UNDERFLOW = -1,
    ERR_MP3_MAINDATA_UNDERFLOW = -2,
    ERR_MP3_NOSYNC = -3,
};

typedef struct {
    int samprate;
    int nChans;
    int bitsPerSample;
    int outputSamps;
    int bitrateKbps;
} MP3FrameInfo;

HMP3Decoder MP3InitDecoder(void);
void MP3FreeDecoder(HMP3Decoder dec);
int MP3FindSyncWord(const uint8_t *buf, int len);
/* 成功/坏帧: pp/plen 两指针按 minimp3 frame_bytes 前移;
 * 数据不足: 不前移并返回 INDATA_UNDERFLOW(等更多数据)。 */
int MP3Decode(HMP3Decoder dec, uint8_t **pp, int *plen, int16_t *pcm,
              int use_size);
void MP3GetLastFrameInfo(HMP3Decoder dec, MP3FrameInfo *info);

#ifdef __cplusplus
}
#endif
#endif /* MINIMP3_HELIX_SHIM_H */
