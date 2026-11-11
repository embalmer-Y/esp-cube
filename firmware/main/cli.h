/*
 * cli.h —— 串口命令行入口(装配层, main/cli.c)
 */
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 CLI 任务(UART0 行输入; 幂等性由调用方保证仅启动一次) */
esp_err_t cli_start(void);

#ifdef __cplusplus
}
#endif
