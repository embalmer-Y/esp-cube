/*
 * app_assist.h —— 语音助理本地服务(M2: 琐事/提醒; 时间底座)
 *
 * 定位(docs 里程碑 M2, 产品重定位 2026-11-12: 音箱 → 语音助理):
 *   - SNTP 时间底座(WiFi 就绪后启动, UTC+8) —— 提醒/主动交互的时钟源;
 *   - 琐事(memo)与提醒(reminder)统一存储: NVS 持久化 + RAM 镜像,
 *     同一 id 空间, due==0 即琐事;
 *   - 提醒到点: 发布 CUBE_EVT_REMINDER_FIRED + 经 announcer 回调
 *     带外播报(L5 接 ai_session_announce —— 快线在位走 omni 音色)。
 *
 * 线程契约: 所有 API 线程安全(内部互斥); announcer 在调度任务上下文
 * 回调, 不得阻塞超过 ~1s。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASSIST_TEXT_LEN   128      /* 条目文本(UTF-8 中文约 42 字) */
#define ASSIST_MAX_ITEMS  24       /* 镜像容量(NVS 满时淘汰最旧) */

/* 单条记录(同时是 NVS blob 布局, 增字段必须 bump magic) */
typedef struct {
    uint32_t magic;                 /* 'A' 'S' 'T' '1' */
    uint32_t id;                    /* 递增 id(LLM 删除/取消用) */
    int64_t  due;                   /* unix 秒; 0 = 琐事(无时间) */
    int64_t  created;               /* unix 秒(淘汰最旧依据) */
    char     text[ASSIST_TEXT_LEN];
} assist_item_t;

/* 带外播报回调(提醒到点/主动交互出口; text 为 UTF-8 短句) */
typedef void (*assist_announcer_t)(const char *text);

esp_err_t app_assist_init(void);
void app_assist_set_announcer(assist_announcer_t fn);

/* ---- 时间 ---- */
bool     app_assist_time_synced(void);
int64_t  app_assist_now(void);      /* unix 秒(未同步时仍返回系统时钟) */
/* "YYYY-MM-DD HH:MM[:SS]" 本地时间(UTC+8) → unix 秒; 全串消费才算成 */
bool     app_assist_parse_localtime(const char *s, int64_t *out_unix);
/* unix 秒 → "N月N日 N点N分"(口播友好) / "MM-DD HH:MM"(紧凑) */
void     app_assist_fmt_speak(int64_t unix, char *out, size_t cap);
void     app_assist_fmt_compact(int64_t unix, char *out, size_t cap);

/* ---- 存储(due=0 → 琐事; >0 → 定时提醒) ---- */
esp_err_t app_assist_add(const char *text, int64_t due, uint32_t *id_out);
esp_err_t app_assist_remove(uint32_t id);        /* 不存在 ESP_ERR_NOT_FOUND */
esp_err_t app_assist_clear(void);
/* 快照到调用方缓冲(按 created 升序), 返回条数(>cap 只拷 cap 条) */
int      app_assist_list(assist_item_t *out, int cap);

#ifdef __cplusplus
}
#endif
