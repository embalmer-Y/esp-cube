/*
 * esp_heap_caps.h —— 模拟器 shim(复用固件源码 cube_fft.c 时的替身头)
 * PC 端统一走 libc 堆。
 */
#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM   (1 << 0)
#define MALLOC_CAP_8BIT     (1 << 1)
#define MALLOC_CAP_INTERNAL (1 << 2)

static inline void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr) { free(ptr); }
