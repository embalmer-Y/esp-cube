/* 宿主回归桩: 事件总线最小面(真实 cubecore.h 拉 FreeRTOS/FFT 等重依赖) */
#pragma once
#include <stddef.h>
#include "esp_err.h"

/* glibc 未声明 strlcpy(IDF newlib 有); 桩统一给出原型,
 * 实现由各测试文件提供 —— 仅宿主可见, 固件构建用不到本头文件 */
size_t strlcpy(char *dst, const char *src, size_t cap);

typedef int cube_evt_id_t;
#define CUBE_EVT_SD_READY 1
typedef void (*cube_evt_cb_t)(cube_evt_id_t id, const void *data, size_t len,
                              void *user_ctx);
esp_err_t cube_event_subscribe(cube_evt_cb_t cb, void *user_ctx);
