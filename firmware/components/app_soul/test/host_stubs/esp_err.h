/* 宿主回归桩: esp_err(IDF 原件不可用) */
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
const char *esp_err_to_name(esp_err_t e);
