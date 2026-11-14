/* 宿主回归桩: FreeRTOS 最小面(信号量恒可用) */
#pragma once
#include <stdint.h>
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
typedef void *SemaphoreHandle_t;
#include "freertos/task.h"
#include "freertos/semphr.h"
