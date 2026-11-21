/* pair_server.c —— 配对 HTTPS 服务装配(见 pair_server.h; T15 续/批次 19)
 *
 * M1 边界如实:
 *  - 证书=主机预置(openssl, gen_pair_certs.sh)串口灌入; 设备端签发归 M2
 *    (mbedtls 4 pk API 私有化, x509write 不可用——批次 19 记录)。
 *  - 端点=配对链(加 /v1/pair 前缀)+只读 /v1/device; 管理 v1 端点(mTLS REQUIRED)
 *    随 T17 同批。client_cert_authmode_optional=true: 配对阶段客户端尚无
 *    证书(docs/20 §3 次序), 拒绝逻辑在端点层而非 TLS 层。
 *  - JSON 解析为手写十六进制字段扫描(协议字段定形, 无需通用库)。
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

#include "pair.h"
#include "pair_server.h"
#include "credstore.h"
#include "trust.h"
#include "mbedtls/ssl.h"

static const char *TAG = "pairsrv";

/* ---- 证书灌入(NVS "pairc"; base64 分块) ---- */

#define CERT_MAX 2048
/* 单一 staging(串口 CLI 单线程: 一张证书 append 完即 commit, 再下一张;
 * 3 份独立 staging 曾多占 4KB 内部 .bss —— 恰好挤掉 CLI 任务 8KB 栈,
 * 间歇启动失败, 批次 19 实证) */
static struct {
    char buf[CERT_MAX];
    size_t len;
    int active;                        /* -1 无, 否则 cert idx */
} s_stage;
static const char *k_cert_keys[3] = { "ca", "srv", "key" };

/* 监听服务状态(commit 后重启以重载证书) */
static struct { int lfd; volatile bool run; } s_tls_srv;
static TaskHandle_t s_srv_task;
int pair_server_stop(void);
int pair_server_start(void);

static int cert_idx(const char *which)
{
    for (int i = 0; i < 3; i++) {
        if (strcmp(which, k_cert_keys[i]) == 0) return i;
    }
    return -1;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, size_t n, uint8_t *out,
                         size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_val(in[i]);
        if (v < 0) continue;               /* 忽略换行等 */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return 0;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

int pair_cert_append(const char *which, const char *b64)
{
    int ix = cert_idx(which);
    if (ix < 0 || b64 == NULL) return -1;
    if (s_stage.active != -1 && s_stage.active != ix) return -1;
    size_t add = strlen(b64);
    if (s_stage.len + add >= CERT_MAX) return -1;
    memcpy(s_stage.buf + s_stage.len, b64, add);
    s_stage.len += add;
    s_stage.active = ix;
    return 0;
}

int pair_cert_commit(const char *which)
{
    int ix = cert_idx(which);
    if (ix < 0) return -1;
    if (s_stage.active != ix) return -1;
    uint8_t der[CERT_MAX];
    size_t n = b64_decode(s_stage.buf, s_stage.len, der, sizeof(der));
    memset(s_stage.buf, 0, CERT_MAX);
    s_stage.len = 0;
    s_stage.active = -1;
    if (n == 0) return -1;
    nvs_handle_t h;
    if (nvs_open("pairc", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, k_cert_keys[ix], der, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "cert %s committed (%u bytes)", which, (unsigned)n);
    /* 运行中的服务持有旧证书堆副本 —— 提交后重启以重载(否则新灌证书
     * 不生效, 服务端出示旧链, 客户端 "unable to get local issuer") */
    if (e == ESP_OK && s_tls_srv.run) {
        pair_server_stop();
        pair_server_start();
    }
    return e == ESP_OK ? 0 : -1;
}

void pair_cert_status(void)
{
    nvs_handle_t h;
    if (nvs_open("pairc", NVS_READONLY, &h) != ESP_OK) {
        printf("  pairc: NVS 不可用\n");
        return;
    }
    for (int i = 0; i < 3; i++) {
        size_t len = 0;
        esp_err_t e = nvs_get_blob(h, k_cert_keys[i], NULL, &len);
        printf("  cert %s: %s (%u bytes)\n", k_cert_keys[i],
               e == ESP_OK ? "已灌入" : "缺", (unsigned)len);
    }
    nvs_close(h);
}

/* ---- 配对服务实例 ---- */

/* 会话(~2.2KB)走堆: 静态内部 .bss 会挤占任务栈余量(见 staging 注) */
static pair_svc_t *s_pair;
static char s_pin[PAIR_PIN_LEN + 1];

/* ---- 凭据(T17): credstore + LLM 密钥 provider ---- */
static credstore_t s_creds;
static bool s_creds_ready;
static void pair_port_init(void);   /* 定义在配对服务段(creds_ensure 前向) */
static char *s_llm_key_cache;          /* PSRAM; 凭据轮换代次失效 */
static uint32_t s_llm_key_ver;

static bool creds_ensure(void)
{
    if (s_creds_ready) return true;
    if (s_pin[0] == 0) pair_port_init();
    const srp_port_t *psa = pair_port_psa();
    trust_ctx_t tc;
    trust_rng_t tr = { psa->rng, NULL };
    if (trust_init(&tc, &tr) != 0) return false;
    uint8_t dk[32];
    if (trust_dev_key(&tc, dk) != 0) return false;
    s_creds_ready = credstore_init(&s_creds, dk) == CRED_RES_OK;
    if (!s_creds_ready) ESP_LOGE(TAG, "credstore init failed");
    return s_creds_ready;
}

static void llm_key_invalidate(void)
{
    free(s_llm_key_cache);
    s_llm_key_cache = NULL;
}

static char *s_settings_cache;         /* T30: settings JSON 缓存(PSRAM) */
static uint32_t s_settings_ver;

/* T30: settings blob JSON 文本(llmprofile 的 profile 来源; 版本缓存) */
const char *pair_server_profile_settings(void)
{
    if (!creds_ensure()) return NULL;
    int ver = credstore_version(&s_creds, "settings");
    if (ver < 0) return NULL;
    if (s_settings_cache != NULL && s_settings_ver == (uint32_t)ver) {
        return s_settings_cache;
    }
    char val[CRED_VALUE_MAX];
    size_t n = 0;
    if (credstore_get(&s_creds, "settings", val, sizeof(val), &n)
        != CRED_RES_OK) {
        return NULL;
    }
    free(s_settings_cache);
    s_settings_cache = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM |
                                        MALLOC_CAP_8BIT);
    if (s_settings_cache == NULL) return NULL;
    memcpy(s_settings_cache, val, n + 1);
    s_settings_ver = (uint32_t)ver;
    return s_settings_cache;
}

/* ai_session 注入的解析器: credstore "llm.api_key" 优先; 返回串由
 * 本缓存持有(会话存续期有效; 轮换经 invalidate 刷新) */
const char *pair_server_llm_key(void)
{
    if (!creds_ensure()) return NULL;
    int ver = credstore_version(&s_creds, "llm.api_key");
    if (ver < 0) {
        llm_key_invalidate();
        return NULL;                       /* 未配置 → Kconfig 兜底 */
    }
    if (s_llm_key_cache != NULL && s_llm_key_ver == (uint32_t)ver) {
        return s_llm_key_cache;
    }
    char *buf = heap_caps_malloc(CRED_VALUE_MAX, MALLOC_CAP_SPIRAM |
                                 MALLOC_CAP_8BIT);
    if (buf == NULL) return NULL;
    size_t n = 0;
    if (credstore_get(&s_creds, "llm.api_key", buf, CRED_VALUE_MAX, &n)
        != CRED_RES_OK) {
        free(buf);
        return NULL;
    }
    free(s_llm_key_cache);
    s_llm_key_cache = buf;
    s_llm_key_ver = (uint32_t)ver;
    return s_llm_key_cache;
}

static uint64_t now_ms(void *ud)
{
    (void)ud;
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void pair_port_init(void)
{
    if (s_pair == NULL) {
        s_pair = malloc(sizeof(pair_svc_t));
        if (s_pair == NULL) {
            ESP_LOGE(TAG, "pair svc alloc failed");
            return;
        }
    }
    pair_port_t pp = {
        .srp = *pair_port_psa(),
        .now_ms = now_ms,
    };
    (void)pair_init(s_pair, &pp);
}

int pair_window_open_cmd(void)
{
    if (s_pin[0] == 0) pair_port_init();
    if (s_pair == NULL) pair_port_init();
    if (s_pair == NULL) return PAIR_RES_ERR;
    int rc = pair_window_open(s_pair, s_pin);
    return rc;
}

void pair_window_pin(char out[7])
{
    memcpy(out, s_pin, PAIR_PIN_LEN + 1);
}

int pair_client_count_pub(void) { return pair_client_count(s_pair); }

/* FR-02 取消配对: 表在 NVS 单一事实源, bind/revoke 同走 tbl_load 无竞态 */
int pair_revoke_cmd(uint32_t client_id)
{
    if (s_pair == NULL) pair_port_init();
    if (s_pair == NULL) return PAIR_RES_ERR;
    return pair_client_revoke(s_pair, client_id);
}

/* ---- hex 工具 ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 从 body 中取 "key":"<hex>" 字段到 out(容量 cap); 返回字节数/-1 */
static int json_hex(const char *body, size_t blen, const char *key,
                    uint8_t *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *hit = NULL;
    for (size_t i = 0; i + strlen(pat) < blen; i++) {
        if (memcmp(body + i, pat, strlen(pat)) == 0) { hit = body + i; break; }
    }
    if (hit == NULL) return -1;
    const char *p = hit + strlen(pat);
    while (p < body + blen && (*p == ':' || *p == ' ' || *p == '"')) p++;
    size_t o = 0;
    while (p + 1 < body + blen && o < cap) {
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0) break;
        out[o++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return (int)o;
}

/* ---- https 服务 ---- */

/* ---- TLS 监听任务(直接 esp-tls; esp_https_server 的 open/accept
 * 集成层在本机反复僵死且无日志——批次 19 实证; 自管监听同语义且全路径
 * 可观测。响应语义与 esp_https_server 版本一致) ---- */
#define httpd_handle_t_unused_impl_(x) ((void)(x))
static uint8_t *s_pem[3];                  /* ca/srv/key 堆副本 */
static size_t s_pem_len[3];

static int load_certs(void)
{
    nvs_handle_t h;
    if (nvs_open("pairc", NVS_READONLY, &h) != ESP_OK) return -1;
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        size_t len = 0;
        if (nvs_get_blob(h, k_cert_keys[i], NULL, &len) != ESP_OK ||
            len == 0 || len > CERT_MAX) {
            continue;
        }
        free(s_pem[i]);
        s_pem[i] = malloc(len + 1);
        if (s_pem[i] == NULL) continue;
        if (nvs_get_blob(h, k_cert_keys[i], s_pem[i], &len) != ESP_OK) {
            free(s_pem[i]);
            s_pem[i] = NULL;
            continue;
        }
        s_pem[i][len] = 0;                 /* esp-tls PEM 约定: NUL 结尾 */
        s_pem_len[i] = len;
        ok++;
    }
    nvs_close(h);
    return ok == 3 ? 0 : -1;
}

static void unload_certs(void)
{
    for (int i = 0; i < 3; i++) {
        free(s_pem[i]);
        s_pem[i] = NULL;
        s_pem_len[i] = 0;
    }
}

/* ---- 请求/响应(微型 HTTP; 端点定形, 无通用解析) ---- */

typedef struct {
    char path[48];
    char method[8];                    /* 请求行动词(GET/PUT/DELETE/POST) */
    char body[2048];                   /* B-6a: hex 编码值翻倍, 900 装
                                         * 不下 690B JWT(→1380 hex) */
    int body_len;
} pair_req_t;

/* 读请求头(至 \r\n\r\n)+body(Content-Length); 超时/超长 → -1 */
static int tls_read_request(esp_tls_t *tls, pair_req_t *rq)
{
    char hdr[512];
    int hn = 0, blen = 0;
    memset(hdr, 0, sizeof(hdr));
    uint32_t deadline = (uint32_t)esp_timer_get_time() / 1000 + 5000;
    while (hn < (int)sizeof(hdr) - 1) {
        char ch;
        int n = esp_tls_conn_read(tls, &ch, 1);
        if (n == 1) {
            hdr[hn++] = ch;
            if (hn >= 4 && memcmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) break;
        } else {
            if ((uint32_t)(esp_timer_get_time() / 1000) > deadline) return -1;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    /* Content-Length(GET 无体 → 无该头, 默认 0; 负值仅来自 atoi 异常) */
    const char *cl = strstr(hdr, "Content-Length:");
    if (cl == NULL) cl = strstr(hdr, "content-length:");
    if (cl != NULL) blen = atoi(cl + 15);
    if (blen < 0 || blen > (int)sizeof(rq->body) - 1) return -1;
    int bn = 0;
    deadline = (uint32_t)esp_timer_get_time() / 1000 + 5000;
    while (bn < blen) {
        int n = esp_tls_conn_read(tls, rq->body + bn, blen - bn);
        if (n > 0) {
            bn += n;
        } else {
            if ((uint32_t)(esp_timer_get_time() / 1000) > deadline) return -1;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    rq->body[bn] = 0;
    rq->body_len = bn;
    /* 请求行: <METHOD> <path> HTTP/1.x */
    const char *sp = strchr(hdr, ' ');
    if (sp == NULL) return -1;
    const char *sp2 = strchr(sp + 1, ' ');
    if (sp2 == NULL || (size_t)(sp2 - sp - 1) >= sizeof(rq->path)) return -1;
    memcpy(rq->path, sp + 1, sp2 - sp - 1);
    rq->path[sp2 - sp - 1] = 0;
    size_t ml = (size_t)(sp - hdr);
    if (ml >= sizeof(rq->method)) return -1;
    memcpy(rq->method, hdr, ml);
    rq->method[ml] = 0;
    return 0;
}

static const char *resp_status(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 412: return "412 Precondition Failed";
    default:  return "500 Server Error";
    }
}

static int tls_write_response(esp_tls_t *tls, int code, const char *json)
{
    char head[128];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                      resp_status(code), (int)strlen(json));
    if (esp_tls_conn_write(tls, head, hn) < 0) return -1;
    return esp_tls_conn_write(tls, json, strlen(json)) < 0 ? -1 : 0;
}

/* ---- 端点工具 ---- */

/* 从 body 取 "key":<十进制数>; 无该字段返回 dflt */
static long json_uint(const char *body, size_t blen, const char *key,
                      long dflt)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *hit = NULL;
    for (size_t i = 0; i + strlen(pat) < blen; i++) {
        if (memcmp(body + i, pat, strlen(pat)) == 0) { hit = body + i; break; }
    }
    if (hit == NULL) return dflt;
    const char *p = hit + strlen(pat);
    while (p < body + blen && (*p == ':' || *p == ' ')) p++;
    char num[16];
    size_t k = 0;
    while (p < body + blen && k < sizeof(num) - 1
           && *p >= '0' && *p <= '9') {
        num[k++] = *p++;
    }
    num[k] = 0;
    return k > 0 ? atol(num) : dflt;
}

/* hex 串 → 字节(容量 cap; 返回字节数/-1) */
static int hex_decode(const char *hex, size_t hlen, uint8_t *out, size_t cap)
{
    if (hlen == 0 || hlen % 2 != 0 || hlen / 2 > cap) return -1;
    for (size_t i = 0; i < hlen; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(hlen / 2);
}

/* ---- 管理端点(T17; v1/mgmt 前缀, 客户端证书 REQUIRED) ---- */

static const char *mgmt_res_json(int rc, uint32_t ver, char *out, size_t cap)
{
    switch (rc) {
    case CRED_RES_OK:
        snprintf(out, cap, "{\"ver\":%lu}", (unsigned long)ver);
        return NULL;
    case CRED_RES_CONFLICT:
        snprintf(out, cap, "{\"ver\":%lu,\"error\":\"conflict\"}",
                 (unsigned long)ver);
        return "conflict";
    case CRED_RES_NOTFND:
        snprintf(out, cap, "{\"error\":\"not found\"}");
        return "notfound";
    case CRED_RES_FULL:
        snprintf(out, cap, "{\"error\":\"full\"}");
        return "full";
    case CRED_RES_TOOBIG:
        snprintf(out, cap, "{\"error\":\"toobig\"}");
        return "toobig";
    default:
        snprintf(out, cap, "{\"error\":\"err\"}");
        return "err";
    }
}

static int ep_creds_list(char *out, size_t cap)
{
    char names[8][CRED_NAME_MAX];
    uint32_t vers[8];
    int n = credstore_list(&s_creds, names, vers, 8);
    if (n < 0) {
        snprintf(out, cap, "{\"error\":\"err\"}");
        return 500;
    }
    size_t off = 0;
    int w = snprintf(out, cap, "{\"creds\":[");
    off += w > 0 ? (size_t)w : 0;
    for (int i = 0; i < n && off < cap - 64; i++) {
        w = snprintf(out + off, cap - off, "%s{\"name\":\"%s\",\"ver\":%lu}",
                     i > 0 ? "," : "", names[i], (unsigned long)vers[i]);
        off += w > 0 ? (size_t)w : 0;
    }
    snprintf(out + off, cap - off, "]}");
    return 200;
}

static int ep_cred_put(const char *name, const pair_req_t *rq,
                       char *out, size_t cap)
{
    if (!creds_ensure()) {
        snprintf(out, cap, "{\"error\":\"credstore\"}");
        return 500;
    }
    uint8_t val[CRED_VALUE_MAX];
    size_t hlen = strnlen(rq->body, rq->body_len);
    /* "value":"<hex>" 顺序扫描定位(同 json_hex 模式, 但不做定长假设) */
    const char *pat = "\"value\"";
    const char *hit = strstr(rq->body, pat);
    if (hit == NULL) {
        snprintf(out, cap, "{\"error\":\"value format\"}");
        return 400;
    }
    const char *p = hit + strlen(pat);
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    const char *q = strchr(p, '"');
    size_t hlen2 = (q != NULL && q >= p) ? (size_t)(q - p) : 0;
    (void)hlen;
    int vn = hex_decode(p, hlen2, val, sizeof(val));
    if (vn <= 0 || vn >= CRED_VALUE_MAX) {
        snprintf(out, cap, "{\"error\":\"value format\"}");
        return 400;
    }
    val[vn] = 0;
    long expected = json_uint(rq->body, rq->body_len, "expected", 0);
    uint32_t ver = 0;
    int rc = credstore_set(&s_creds, name, (const char *)val,
                           expected != 0 ? (uint32_t)expected : 0, &ver);
    const char *err = mgmt_res_json(rc, ver, out, cap);
    return err == NULL ? 200 : (strcmp(err, "conflict") == 0 ? 412 : 400);
}

static int ep_cred_del(const char *name, const pair_req_t *rq,
                       char *out, size_t cap)
{
    if (!creds_ensure()) {
        snprintf(out, cap, "{\"error\":\"credstore\"}");
        return 500;
    }
    long expected = json_uint(rq->body, rq->body_len, "expected", 0);
    uint32_t ver = 0;
    int rc = credstore_delete(&s_creds, name,
                              expected != 0 ? (uint32_t)expected : 0, &ver);
    const char *err = mgmt_res_json(rc, ver, out, cap);
    return err == NULL ? 200 : (strcmp(err, "conflict") == 0 ? 412 : 400);
}

static int ep_settings_get(char *out, size_t cap)
{
    if (!creds_ensure()) {
        snprintf(out, cap, "{\"error\":\"credstore\"}");
        return 500;
    }
    int ver = credstore_version(&s_creds, "settings");
    if (ver < 0) {
        snprintf(out, cap, "{\"ver\":0}");
        return 200;
    }
    char val[CRED_VALUE_MAX];
    size_t n = 0;
    if (credstore_get(&s_creds, "settings", val, sizeof(val), &n)
        != CRED_RES_OK) {
        snprintf(out, cap, "{\"ver\":0}");
        return 200;
    }
    /* 值以 hex 回传(避免 JSON 转义歧义) */
    size_t off = 0;
    int w = snprintf(out, cap, "{\"ver\":%d,\"value\":\"",
                     ver);
    off += w > 0 ? (size_t)w : 0;
    for (size_t i = 0; i < n && off + 2 < cap - 8; i++) {
        w = snprintf(out + off, cap - off, "%02x",
                     (unsigned char)val[i]);
        off += w > 0 ? (size_t)w : 0;
    }
    snprintf(out + off, cap - off, "\"}");
    return 200;
}

static int ep_settings_put(const pair_req_t *rq, char *out, size_t cap)
{
    if (!creds_ensure()) {
        snprintf(out, cap, "{\"error\":\"credstore\"}");
        return 500;
    }
    const char *pat = "\"value\"";
    const char *hit = strstr(rq->body, pat);
    if (hit == NULL) {
        snprintf(out, cap, "{\"error\":\"value format\"}");
        return 400;
    }
    const char *p = hit + strlen(pat);
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    const char *q = strchr(p, '"');
    size_t hlen = (q != NULL && q >= p) ? (size_t)(q - p) : 0;
    uint8_t val[CRED_VALUE_MAX];
    int vn = hex_decode(p, hlen, val, sizeof(val));
    if (vn < 0 || vn >= CRED_VALUE_MAX) {
        snprintf(out, cap, "{\"error\":\"value format\"}");
        return 400;
    }
    val[vn] = 0;
    long expected = json_uint(rq->body, rq->body_len, "expected", 0);
    uint32_t ver = 0;
    int rc = credstore_set(&s_creds, "settings", (const char *)val,
                           expected != 0 ? (uint32_t)expected : 0, &ver);
    const char *err = mgmt_res_json(rc, ver, out, cap);
    return err == NULL ? 200 : (strcmp(err, "conflict") == 0 ? 412 : 400);
}

/* 客户端证书 REQUIRED(docs/20 §4): 配对端点免证, 管理 v1 强制;
 * optional 模式下未出示=peer_cert NULL, 出示但无效=verify_result≠0 */
static bool mgmt_client_authorized(esp_tls_t *tls)
{
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(tls);
    if (ssl == NULL) return false;
    if (mbedtls_ssl_get_verify_result(ssl) != 0) return false;
    return mbedtls_ssl_get_peer_cert(ssl) != NULL;
}

/* ---- 端点处理(与 esp_https_server 版语义一致) ---- */

static void ep_device(char *out, size_t cap)
{
    snprintf(out, cap,
             "{\"name\":\"espcube\",\"proto\":1,\"paired\":%d}",
             pair_client_count(s_pair));
}

static int ep_step1(char *out, size_t cap)
{
    uint8_t salt[16], B[384];
    if (pair_step1(s_pair, salt, B) != PAIR_RES_OK) {
        snprintf(out, cap, "{\"error\":\"window\"}");
        return 403;
    }
    char Bhex[769], shex[33];
    for (int i = 0; i < 384; i++) sprintf(Bhex + i * 2, "%02x", B[i]);
    for (int i = 0; i < 16; i++) sprintf(shex + i * 2, "%02x", salt[i]);
    snprintf(out, cap, "{\"salt\":\"%s\",\"B\":\"%s\"}", shex, Bhex);
    return 200;
}

static int ep_step2(const pair_req_t *rq, char *out, size_t cap)
{
    uint8_t A[384];
    if (json_hex(rq->body, rq->body_len, "A", A, sizeof(A)) != 384) {
        snprintf(out, cap, "{\"error\":\"A format\"}");
        return 400;
    }
    int rc = pair_step2(s_pair, A);
    snprintf(out, cap, rc == PAIR_RES_OK ? "{\"ok\":true}"
                                         : "{\"error\":\"step2\"}");
    return rc == PAIR_RES_OK ? 200 : 403;
}

static int ep_step3(const pair_req_t *rq, char *out, size_t cap)
{
    uint8_t m1[32];
    if (json_hex(rq->body, rq->body_len, "m1", m1, sizeof(m1)) != 32) {
        snprintf(out, cap, "{\"error\":\"m1 format\"}");
        return 400;
    }
    uint8_t m2[32];
    int rc = pair_step3(s_pair, m1, m2);
    if (rc == PAIR_RES_LOCKED) {
        snprintf(out, cap, "{\"error\":\"locked\"}");
        return 403;
    }
    if (rc != PAIR_RES_OK) {
        /* 窗口/PIN 错同响应(不泄露差异) */
        snprintf(out, cap, "{\"error\":\"auth\"}");
        return 403;
    }
    char mhex[65];
    for (int i = 0; i < 32; i++) sprintf(mhex + i * 2, "%02x", m2[i]);
    snprintf(out, cap, "{\"m2\":\"%s\"}", mhex);
    return 200;
}

static int ep_bind(const pair_req_t *rq, char *out, size_t cap)
{
    uint8_t pk[PAIR_PUBKEY_LEN];
    if (json_hex(rq->body, rq->body_len, "pubkey", pk, sizeof(pk))
        != PAIR_PUBKEY_LEN) {
        snprintf(out, cap, "{\"error\":\"pubkey format\"}");
        return 400;
    }
    uint32_t id = 0;
    int rc = pair_bind(s_pair, pk, &id);
    if (rc == PAIR_RES_OK) {
        snprintf(out, cap, "{\"client_id\":%lu}", (unsigned long)id);
        return 200;
    }
    snprintf(out, cap,
             rc == PAIR_RES_FULL ? "{\"error\":\"full\"}"
                                 : "{\"error\":\"state\"}");
    return rc == PAIR_RES_FULL ? 403 : 400;
}

/* ---- 监听任务 ---- */

static void srv_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pair server task on :443 (paired=%d)",
             pair_client_count(s_pair));
    while (s_tls_srv.run) {
        int cfd = lwip_accept(s_tls_srv.lfd, NULL, NULL);
        if (cfd < 0) {
            if (s_tls_srv.run) vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        esp_tls_t *tls = esp_tls_init();
        if (tls == NULL) { lwip_close(cfd); continue; }
        esp_tls_cfg_server_t cfg = {
            .cacert_buf = s_pem[0],
            .cacert_bytes = (unsigned int)(s_pem_len[0] + 1),
            .servercert_buf = s_pem[1],
            .servercert_bytes = (unsigned int)(s_pem_len[1] + 1),
            .serverkey_buf = s_pem[2],
            .serverkey_bytes = (unsigned int)(s_pem_len[2] + 1),
            .client_cert_authmode_optional = true,   /* 配对阶段无证书 */
        };
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        int rc = esp_tls_server_session_create(&cfg, cfd, tls);
        ESP_LOGI(TAG, "tls handshake rc=%d (%lu ms)", rc,
                 (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - t0));
        if (rc != 0) {
            esp_tls_server_session_delete(tls);
            lwip_close(cfd);
            continue;
        }
        pair_req_t *rq = malloc(sizeof(pair_req_t));
        if (rq == NULL || tls_read_request(tls, rq) != 0) {
            free(rq);
            esp_tls_server_session_delete(tls);
            lwip_close(cfd);
            continue;
        }
        char out[968];
        int code = 400;
        /* 管理 v1(T17): 客户端证书 REQUIRED; 401 不泄露差异(docs/20 §4) */
        if (strncmp(rq->path, "/v1/mgmt/", 9) == 0
            && !mgmt_client_authorized(tls)) {
            code = 401;
            snprintf(out, sizeof(out), "{\"error\":\"unauthorized\"}");
        } else if (strcmp(rq->path, "/v1/mgmt/creds") == 0
                   && strcmp(rq->method, "GET") == 0) {
            code = ep_creds_list(out, sizeof(out));
        } else if (strncmp(rq->path, "/v1/mgmt/creds/", 15) == 0) {
            const char *nm = rq->path + 15;
            if (strcmp(rq->method, "PUT") == 0) {
                code = ep_cred_put(nm, rq, out, sizeof(out));
            } else if (strcmp(rq->method, "DELETE") == 0) {
                code = ep_cred_del(nm, rq, out, sizeof(out));
            } else {
                snprintf(out, sizeof(out), "{\"error\":\"method\"}");
            }
        } else if (strcmp(rq->path, "/v1/mgmt/settings") == 0) {
            if (strcmp(rq->method, "GET") == 0) {
                code = ep_settings_get(out, sizeof(out));
            } else if (strcmp(rq->method, "PUT") == 0) {
                code = ep_settings_put(rq, out, sizeof(out));
            } else {
                snprintf(out, sizeof(out), "{\"error\":\"method\"}");
            }
        } else if (strcmp(rq->path, "/v1/device") == 0) {
            ep_device(out, sizeof(out));
            code = 200;
        } else if (strcmp(rq->path, "/v1/pair/step1") == 0) {
            code = ep_step1(out, sizeof(out));
        } else if (strcmp(rq->path, "/v1/pair/step2") == 0) {
            code = ep_step2(rq, out, sizeof(out));
        } else if (strcmp(rq->path, "/v1/pair/step3") == 0) {
            code = ep_step3(rq, out, sizeof(out));
        } else if (strcmp(rq->path, "/v1/pair/bind") == 0) {
            code = ep_bind(rq, out, sizeof(out));
        } else {
            snprintf(out, sizeof(out), "{\"error\":\"not found\"}");
        }
        tls_write_response(tls, code, out);
        free(rq);
        esp_tls_server_session_delete(tls);
        lwip_close(cfd);
    }
    unload_certs();
    s_srv_task = NULL;
    vTaskDelete(NULL);
}

int pair_server_start(void)
{
    if (s_tls_srv.run) return 0;           /* 已在跑 */
    if (s_pin[0] == 0) pair_port_init();
    if (load_certs() != 0) {
        ESP_LOGW(TAG, "certs incomplete (certup/certdone 先灌入)");
        return -1;
    }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(443),
    };
    int one = 1;
    if (lwip_setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(lfd, 2) < 0) {
        ESP_LOGE(TAG, "listen :443 failed");
        lwip_close(lfd);
        unload_certs();
        return -1;
    }
    s_tls_srv.lfd = lfd;
    s_tls_srv.run = true;
    /* 任务栈必须内部 RAM: PSRAM 栈在 flash cache 关断期断言崩溃
     * (esp_task_stack_is_sane_cache_disabled, 批次 20 实证——PSRAM 栈
     * 路线在本配置不可用)。10240 = esp_https_server 官方默认值; 内部
     * 紧张由 lwip→PSRAM(SPIRAM_TRY_ALLOCATE_WIFI_LWIP)解决。 */
    if (xTaskCreate(srv_task, "pairsrv", 10240, NULL,
                    tskIDLE_PRIORITY + 5, &s_srv_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed (largest internal block %u)",
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_tls_srv.run = false;
        lwip_close(lfd);
        unload_certs();
        return -1;
    }
    ESP_LOGI(TAG, "pair server started (paired=%d)",
             pair_client_count(s_pair));
    return 0;
}

int pair_server_stop(void)
{
    if (!s_tls_srv.run) return 0;
    s_tls_srv.run = false;
    lwip_close(s_tls_srv.lfd);             /* 解开 accept */
    /* 任务自卸 unload_certs */
    for (int i = 0; i < 40 && s_srv_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    ESP_LOGI(TAG, "pair server stopped");
    return 0;
}

bool pair_server_running(void) { return s_tls_srv.run; }
