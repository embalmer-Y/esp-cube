/*
 * app_storage.h —— SD 卡存储服务(M3+ 记忆系统强制 SD, 2026-11-12 定案)
 *
 * 底板 TF 卡槽(microSD, SDMMC slot0, GPIO39-44)挂 FATFS, 挂载点
 * /sdcard; 上电即探测, 缺卡优雅降级(记忆回退 NVS / 素材回退 flash)。
 *
 * 目录规划(docs/09):
 *   /sdcard/mem/     情节记忆(episodes.jsonl) + 对话原文(raw/)
 *   /sdcard/face/    表情素材库(状态/情绪 .efc 片段)
 *   /sdcard/wake/    唤醒词模型(开源 KWS: onnx/tflite + 阈值)
 *   /sdcard/voice/   复刻参考音频(M5)
 *   /sdcard/etc/     人格/配置(persona.json —— app/web 配置页落点)
 *   /sdcard/logs/    运行日志/崩溃留痕
 */
#pragma once

#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 订阅 WiFi 就绪 → 延迟挂载(hosted DMA 内存优先); 幂等 */
esp_err_t app_storage_init(void);
/* 实际挂载(通常由 init 内部在 WiFi 就绪后调度; 幂等) */
esp_err_t app_storage_mount(void);
bool app_storage_ok(void);

/* 组绝对路径: snprintf 语义, 返回长度; 未挂载时仍生成路径(调用方自查) */
int app_storage_path(char *buf, size_t cap, const char *rel);

/* 读整个文件到堆(调用方 free; 不存在返回 NULL); *len 可空 */
char *app_storage_read(const char *rel, size_t *len);

/* 整文件写入(临时文件+rename 原子替换) */
esp_err_t app_storage_write(const char *rel, const void *data, size_t len);

/* 追加一行(记忆 jsonl; 失败静默记日志) */
esp_err_t app_storage_append(const char *rel, const char *line);

#ifdef __cplusplus
}
#endif
