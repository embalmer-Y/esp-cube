/*
 * ncm_weapi.h —— weapi 加密构造(内部头)
 */
#pragma once

#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 由 JSON 请求体构造 urlencoded 表单 "params=..&encSecKey=..";
 * 返回 malloc 体(调用方 free) */
esp_err_t ncm_weapi_build(const char *json_body, char **out_body,
                          size_t *out_len);

#ifdef __cplusplus
}
#endif
