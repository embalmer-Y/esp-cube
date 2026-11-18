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

/* BT 模式让出射频(M1.8.5): 停 WiFi(含重连抑制) / 恢复 WiFi 并重连。
 * 由 CUBE_EVT_BT_AUDIO_ACTIVE 自动驱动; 也可显式调用。 */
esp_err_t app_net_suspend(void);
esp_err_t app_net_resume(void);

/* STA 是否已获 IP(BT 恢复后播放器等网用) */
bool app_net_is_up(void);
esp_err_t app_net_wait_up(int timeout_ms);

/* WiFi 栈是否已就绪(app_net_start 完成; 配网窗口前置) */
bool app_net_ready(void);

/* P3a 配网窗口配套: 抑制/恢复 STA 自动重连(窗口期 provisioning manager
 * 接管射频状态机, 我方 handler 不得抢连/排重连)。app_prov 内部使用。 */
void app_net_prov_guard(bool on);

#ifdef __cplusplus
}
#endif
