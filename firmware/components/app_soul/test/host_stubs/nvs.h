/* 宿主回归桩: NVS 内存版接口面(实现见 test_soul_mem.c) */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef long nvs_iterator_t;
typedef int nvs_type_t;
#define NVS_TYPE_BLOB 1
#define NVS_READONLY  0
#define NVS_READWRITE 1
typedef struct { char key[16]; } nvs_entry_info_t;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *p, size_t *n);
esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t t,
                         nvs_iterator_t *it);
void nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info);
esp_err_t nvs_entry_next(nvs_iterator_t *it);
void nvs_release_iterator(nvs_iterator_t it);   /* 与 IDF 一致: 按值 */
