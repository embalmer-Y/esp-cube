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

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>
#include "store_crypt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 订阅 WiFi 就绪 → 延迟挂载(hosted DMA 内存优先); 幂等 */
esp_err_t app_storage_init(void);
/* 实际挂载(通常由 init 内部在 WiFi 就绪后调度; 幂等) */
esp_err_t app_storage_mount(void);
/* 台调入口: 手动重挂载(与自动路径独立, 失败可反复重试)。
 * mode 0=SDMMC slot0(4-bit) / 1=SDSPI(SPI2, 1-bit); 已挂载时先卸载 */
esp_err_t app_storage_mount_mode(int mode);
/* 台调诊断: DET/CLK/CMD/D0..D3 电平打印(卡座在位/走线判断; 未挂载时
 * 附带手动时钟下 D0 电平测试)。插拔卡各跑一次对比电平 */
void app_storage_probe(void);
bool app_storage_ok(void);

/* 组绝对路径: snprintf 语义, 返回长度; 未挂载时仍生成路径(调用方自查) */
int app_storage_path(char *buf, size_t cap, const char *rel);

/* 读整个文件到堆(调用方 free; 不存在返回 NULL); *len 可空 */
char *app_storage_read(const char *rel, size_t *len);

/* 整文件写入(临时文件+rename 原子替换) */
esp_err_t app_storage_write(const char *rel, const void *data, size_t len);

/* 追加一行(记忆 jsonl; 失败静默记日志) */
esp_err_t app_storage_append(const char *rel, const char *line);

/* ---- T14 透明加密(EPSCV2; docs/20 §5) ----
 * enable 后 write 全加密、read 自动识别(密文解密/旧明文透传)。
 * crypt_bind 须绑 RAW 层(由 enable 内部完成, 消费方勿直接调)。 */
struct store_crypt_t;
void app_storage_crypt_enable(void);
void app_storage_crypt_bind(store_crypt_t *c);
bool app_storage_crypt_active(void);
/* PSA 密码学端口(T17: credstore 等消费方复用, 勿自行实现第二份) */
const sc_port_t *app_storage_crypt_port(void);
char *app_storage_raw_read(const char *rel, size_t *len);
esp_err_t app_storage_raw_write(const char *rel, const void *data,
                                size_t len);

#ifdef __cplusplus
}
#endif
