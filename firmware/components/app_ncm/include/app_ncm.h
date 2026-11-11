/*
 * app_ncm.h —— 网易云音乐游客客户端(L2 云服务协议库)
 *
 * 能力(游客态, 无需登录): 关键词搜索单曲 / 取歌曲播放地址(标准音质)。
 * 加密: weapi(AES-128-CBC×2 + RSA-1024 裸模幂), 接口口径与
 * NeteaseCloudMusicApiEnhanced/api-enhanced 一致(2026-11 裸调真机验证)。
 * 播放地址为 HTTP MP3 渐进流(vuutv 追踪参数已剥离), 直接交给
 * app_player_start(url) 单曲播放(由 L5 装配层编排)。
 *
 * 全部接口为同步阻塞(HTTP ~1-3s), 不得在事件循环/LVGL 任务上下文调用。
 * VIP/登录(cookie/NVS/屏上二维码)为第二期, 本组件预留 cookie 注入位。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NCM_MAX_RESULTS   8
#define NCM_STR_LEN       64

typedef struct {
    uint64_t id;                        /* 歌曲 id */
    char name[NCM_STR_LEN];             /* 歌名(UTF-8, 超长截断) */
    char artist[NCM_STR_LEN];           /* 第一位歌手 */
    int fee;                            /* 0=版权异常 1=VIP 4=需购买 8=免费可播 */
    int duration_ms;
} ncm_song_t;

typedef struct {
    ncm_song_t songs[NCM_MAX_RESULTS];
    int count;
} ncm_search_result_t;

/* 关键词搜索单曲(type=1), 结果写入 out; limit 上限 NCM_MAX_RESULTS */
esp_err_t app_ncm_search(const char *keywords, int limit,
                         ncm_search_result_t *out);

/* 取播放地址(游客=标准 128k MP3); out_url 剥离 query(vuutv 防盗链对策),
 * out_br 可空; URL 有效期约 20 分钟, 现取现播 */
esp_err_t app_ncm_get_url(uint64_t song_id, char *out_url, size_t out_len,
                          int *out_br);

/* ---- 二期预留: 登录 cookie 注入(MUSIC_U; 不设=游客态) ---- */
esp_err_t app_ncm_set_cookie(const char *cookie);

#ifdef __cplusplus
}
#endif
