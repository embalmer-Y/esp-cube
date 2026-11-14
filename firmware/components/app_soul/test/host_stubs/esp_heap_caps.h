/* 宿主回归桩: heap_caps → 受测控的 malloc(可注入分配失败) */
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT   2
void *heap_caps_malloc(size_t n, unsigned caps);
