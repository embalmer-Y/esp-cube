/* 宿主回归桩: 互斥量桩(实现见 test_soul_mem.c) */
#pragma once
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks);
void xSemaphoreGive(SemaphoreHandle_t h);
