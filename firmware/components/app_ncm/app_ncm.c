/*
 * app_ncm.c —— 网易云音乐游客客户端实现
 *
 * HTTP 口径(2026-11 裸调真机验证, 见 docs):
 *   POST https://interface.music.163.com/weapi/<api>
 *   表单 = weapi(json); 头 = UA(Edge/Chrome) + Referer; 游客无需 cookie。
 * 播放地址剥离 query(vuutv 为网页端校验参数, 非 app 客户端携带会被
 * CDN 403 —— 2026-11-11 登录态实测, 游客剥离亦无害)。
 */
#include "app_ncm.h"
#include "ncm_weapi.h"

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "ncm";

#define NCM_HOST        "interface.music.163.com"
#define NCM_UA          "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " \
                        "AppleWebKit/537.36 (KHTML, like Gecko) " \
                        "Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0"
#define NCM_TIMEOUT_MS  12000
#define NCM_RESP_INIT   (8 * 1024)

/* 二期预留: 登录 cookie(不设=游客) */
static char s_cookie[512];

esp_err_t app_ncm_set_cookie(const char *cookie)
{
    if (cookie == NULL || cookie[0] == 0) {
        s_cookie[0] = 0;
        return ESP_OK;
    }
    strlcpy(s_cookie, cookie, sizeof(s_cookie));
    return ESP_OK;
}

/* POST weapi 接口, 返回 NUL 结尾的响应体(malloc, 调用方 free) */
static esp_err_t ncm_post(const char *api_path, const char *json_body,
                          char **out_resp)
{
    char *form = NULL;
    size_t form_len = 0;
    esp_err_t err = ncm_weapi_build(json_body, &form, &form_len);
    if (err != ESP_OK) {
        return err;
    }

    char url[96];
    snprintf(url, sizeof(url), "https://" NCM_HOST "%s", api_path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NCM_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http == NULL) {
        free(form);
        return ESP_FAIL;
    }
    esp_http_client_set_header(http, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_header(http, "User-Agent", NCM_UA);
    esp_http_client_set_header(http, "Referer", "music.163.com");
    if (s_cookie[0] != 0) {
        esp_http_client_set_header(http, "Cookie", s_cookie);
    }

    err = esp_http_client_open(http, form_len);
    if (err == ESP_OK) {
        int w = esp_http_client_write(http, form, (int)form_len);
        if (w < 0) {
            err = ESP_FAIL;
        }
    }
    free(form);
    form = NULL;

    char *resp = NULL;
    if (err == ESP_OK) {
        int64_t clen = esp_http_client_fetch_headers(http);
        /* -1=chunked: 按块读到 EOF */
        size_t cap = clen > 0 ? (size_t)clen + 1 : NCM_RESP_INIT;
        resp = malloc(cap);
        size_t got = 0;
        if (resp != NULL) {
            while (got < cap - 1) {
                int n = esp_http_client_read(http, resp + got,
                                             (int)(cap - 1 - got));
                if (n <= 0) break;
                got += (size_t)n;
                if (got == cap - 1 && clen < 0) {   /* chunked 扩容 */
                    char *nb = realloc(resp, cap * 2);
                    if (nb == NULL) break;
                    resp = nb;
                    cap *= 2;
                }
            }
            resp[got] = 0;
        }
        if (resp == NULL || got == 0) {
            free(resp);
            resp = NULL;
            err = ESP_FAIL;
        }
    }
    esp_http_client_cleanup(http);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST %s failed: %s", api_path, esp_err_to_name(err));
        return err;
    }
    *out_resp = resp;
    return ESP_OK;
}

static void copy_str(char *dst, size_t cap, const cJSON *node)
{
    if (cJSON_IsString(node) && node->valuestring != NULL) {
        strlcpy(dst, node->valuestring, cap);
    } else {
        dst[0] = 0;
    }
}

esp_err_t app_ncm_search(const char *keywords, int limit,
                         ncm_search_result_t *out)
{
    if (keywords == NULL || keywords[0] == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (limit <= 0 || limit > NCM_MAX_RESULTS) {
        limit = NCM_MAX_RESULTS;
    }

    /* JSON 转义关键词(引号/反斜杠/控制字符) */
    char esc[128];
    size_t e = 0;
    for (const char *c = keywords; *c && e < sizeof(esc) - 8; c++) {
        if (*c == '"' || *c == '\\') {
            esc[e++] = '\\';
        }
        esc[e++] = *c;
    }
    esc[e] = 0;

    char body[256];
    snprintf(body, sizeof(body),
             "{\"s\":\"%s\",\"type\":1,\"limit\":%d,\"offset\":0,"
             "\"csrf_token\":\"\"}", esc, limit);

    char *resp = NULL;
    esp_err_t err = ncm_post("/weapi/search/get", body, &resp);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "search: json parse fail");
        return ESP_FAIL;
    }
    cJSON *songs = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"),
                                       "songs");
    int rc = ESP_OK;
    if (!cJSON_IsArray(songs)) {
        ESP_LOGE(TAG, "search: no result.songs");
        rc = ESP_FAIL;
    } else {
        cJSON *s;
        cJSON_ArrayForEach(s, songs) {
            if (out->count >= limit) break;
            ncm_song_t *song = &out->songs[out->count];
            const cJSON *id = cJSON_GetObjectItem(s, "id");
            if (cJSON_IsNumber(id)) {
                song->id = (uint64_t)id->valuedouble;
            }
            copy_str(song->name, sizeof(song->name), cJSON_GetObjectItem(s, "name"));
            cJSON *arts = cJSON_GetObjectItem(s, "artists");
            if (cJSON_IsArray(arts) && cJSON_GetArraySize(arts) > 0) {
                copy_str(song->artist, sizeof(song->artist),
                         cJSON_GetObjectItem(cJSON_GetArrayItem(arts, 0), "name"));
            }
            const cJSON *fee = cJSON_GetObjectItem(s, "fee");
            const cJSON *dur = cJSON_GetObjectItem(s, "duration");
            song->fee = cJSON_IsNumber(fee) ? fee->valueint : -1;
            song->duration_ms = cJSON_IsNumber(dur) ? dur->valueint : 0;
            if (song->id != 0) {
                out->count++;
            }
        }
        if (out->count == 0) {
            rc = ESP_ERR_NOT_FOUND;
        }
    }
    cJSON_Delete(root);
    return rc;
}

esp_err_t app_ncm_get_url(uint64_t song_id, char *out_url, size_t out_len,
                          int *out_br)
{
    if (song_id == 0 || out_url == NULL || out_len < 64) {
        return ESP_ERR_INVALID_ARG;
    }
    out_url[0] = 0;

    char body[96];
    snprintf(body, sizeof(body),
             "{\"ids\":\"[%llu]\",\"br\":128000,\"csrf_token\":\"\"}",
             (unsigned long long)song_id);

    char *resp = NULL;
    esp_err_t err = ncm_post("/weapi/song/enhance/player/url", body, &resp);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "url: json parse fail");
        return ESP_FAIL;
    }
    esp_err_t rc = ESP_FAIL;
    cJSON *d0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "data"), 0);
    const cJSON *code = cJSON_GetObjectItem(d0, "code");
    const cJSON *url = cJSON_GetObjectItem(d0, "url");
    const cJSON *br = cJSON_GetObjectItem(d0, "br");
    if (cJSON_IsNumber(code) && code->valueint == 200 &&
        cJSON_IsString(url) && url->valuestring != NULL) {
        /* 剥离 query: vuutv 为网页端校验参数, 非 app 客户端携带会被
         * CDN 403(登录态实测), 游客剥离亦无害 */
        char *q = strchr(url->valuestring, '?');
        size_t n = (q != NULL) ? (size_t)(q - url->valuestring)
                               : strlen(url->valuestring);
        if (n < out_len) {
            memcpy(out_url, url->valuestring, n);
            out_url[n] = 0;
            if (out_br != NULL && cJSON_IsNumber(br)) {
                *out_br = br->valueint;
            }
            rc = ESP_OK;
        } else {
            rc = ESP_ERR_INVALID_SIZE;
        }
    } else {
        ESP_LOGW(TAG, "url: song %llu not available (code=%d, 游客仅免 "
                 "费曲/VIP曲需二期登录)", (unsigned long long)song_id,
                 cJSON_IsNumber(code) ? code->valueint : -1);
    }
    cJSON_Delete(root);
    return rc;
}
