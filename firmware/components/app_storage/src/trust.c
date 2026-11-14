/* trust.c —— 设备信任区实现(T14; 见 trust.h) */
#include <string.h>

#include "trust.h"
#include "nvs.h"
#include "esp_err.h"

static trust_ctx_t *g_ctx;

static int key_from_nvs(const char *name, uint8_t out[TRUST_KEY_LEN])
{
    nvs_handle_t h;
    if (nvs_open(TRUST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t len = TRUST_KEY_LEN;
    esp_err_t e = nvs_get_blob(h, name, out, &len);
    nvs_close(h);
    return (e == ESP_OK && len == TRUST_KEY_LEN) ? 0 : -1;
}

static int key_to_nvs(const char *name, const uint8_t key[TRUST_KEY_LEN])
{
    nvs_handle_t h;
    if (nvs_open(TRUST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, name, key, TRUST_KEY_LEN);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static int key_ensure(trust_ctx_t *c, const char *name,
                      bool *flag, uint8_t out[TRUST_KEY_LEN])
{
    if (c == NULL || c->rng.rng == NULL) return -1;
    if (key_from_nvs(name, out) == 0) return 0;
    uint8_t k[TRUST_KEY_LEN];
    if (c->rng.rng(c->rng.ud, k, TRUST_KEY_LEN) != 0) return -1;
    if (key_to_nvs(name, k) != 0) return -1;
    memcpy(out, k, TRUST_KEY_LEN);
    *flag = true;
    return 0;
}

int trust_init(trust_ctx_t *c, const trust_rng_t *rng)
{
    if (c == NULL || rng == NULL || rng->rng == NULL) return -1;
    memset(c, 0, sizeof(*c));
    c->rng = *rng;
    g_ctx = c;
    return 0;
}

int trust_dev_key(trust_ctx_t *c, uint8_t out[TRUST_KEY_LEN])
{
    trust_ctx_t *cc = c ? c : g_ctx;
    if (cc == NULL || out == NULL) return -1;
    if (cc->dev_ready) { memcpy(out, cc->dev_cache, TRUST_KEY_LEN); return 0; }
    uint8_t k[TRUST_KEY_LEN];
    if (key_ensure(cc, "dev_key", &cc->dev_ready, k) != 0) return -1;
    memcpy(out, k, TRUST_KEY_LEN);
    memcpy(cc->dev_cache, k, TRUST_KEY_LEN);
    return 0;
}

int trust_sd_key(trust_ctx_t *c, uint8_t out[TRUST_KEY_LEN])
{
    trust_ctx_t *cc = c ? c : g_ctx;
    if (cc == NULL || out == NULL) return -1;
    if (cc->sd_ready) { memcpy(out, cc->sd_cache, TRUST_KEY_LEN); return 0; }
    uint8_t k[TRUST_KEY_LEN];
    if (key_ensure(cc, "sd_key", &cc->sd_ready, k) != 0) return -1;
    memcpy(out, k, TRUST_KEY_LEN);
    memcpy(cc->sd_cache, k, TRUST_KEY_LEN);
    return 0;
}

int trust_wipe(trust_ctx_t *c)
{
    nvs_handle_t h;
    if (nvs_open(TRUST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_erase_all(h);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (c != NULL) c->dev_ready = c->sd_ready = false;
    return e == ESP_OK ? 0 : -1;
}
