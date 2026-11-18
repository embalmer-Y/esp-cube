/*
 * app_prov.h —— SoftAP 首配窗口(P3a; docs/17 §9.1 / docs/18 v2 P3a)
 *
 * 官方 network_provisioning 协议: protocomm SoftAP 传输 + Security 2
 * (SRP-6a + AES-GCM)。与 P3b 自研 App 及官方 ESP Provisioning app 互通。
 * 窗口语义: CLI "prov start" 开窗 → 屏外通道(串口/后续屏显)出示 PIN →
 * 手机连 SoftAP → app 输入 PIN + 投递 WiFi 凭据 → 成功收窗/超时回落。
 */
#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 开窗(瞬态任务执行, 立即返回)。timeout_sec: 0=用默认 180s。
 * 重复开窗返回 ESP_ERR_INVALID_STATE。 */
esp_err_t app_prov_start(int timeout_sec);

/* 手动收窗(超时之外的提前退出; 成功路径不必调用)。 */
esp_err_t app_prov_stop(void);

/* 窗口是否进行中 */
bool app_prov_active(void);

#ifdef __cplusplus
}
#endif
