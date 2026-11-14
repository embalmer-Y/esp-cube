/* 宿主回归桩: 日志静默(也保证用户文本不进测试输出) */
#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
