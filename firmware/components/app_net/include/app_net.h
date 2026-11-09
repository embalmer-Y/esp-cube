/*
 * app_net.h —— WiFi STA 管理
 *
 * 原型阶段: SSID/密码来自 Kconfig(编译期);M3 里程碑改为 BLE/SoftAP 配网。
 * 职责: 初始化 esp_netif/wifi, 自动重连(指数退避), 通过事件总线上报状态。
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化并异步连接(不阻塞调用方) */
esp_err_t app_net_start(void);

#ifdef __cplusplus
}
#endif
