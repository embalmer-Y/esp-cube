/* pair_svc.c —— 配对服务实现(见 pair.h; T15 M1)
 *
 * 指纹表: NVS "pair" 区单 blob("tbl") = [u32 seq][u8 n][4×{u32 id,
 * u8 fp[32]}] —— 整表原子重写; 表损 = 已配对关系失效(等价恢复出厂,
 * docs/20 §6 口径), 不引入双槽(M1 骨架边界, 换机重配对代价低)。
 * PIN 生成: 每 digit 取 u16 拒绝采样(≥60000 重试)消除模偏。
 */
#include <string.h>

#include "nvs.h"
#include "esp_log.h"

#include "pair.h"
#include "srp6a.h"

/* 嵌入存储容量契约(编译期校验; pair.h 的 srv_storage 不得缩水) */
_Static_assert(sizeof(srp_srv_t) <=
               sizeof(((pair_svc_t *)0)->srv_storage),
               "pair.h srv_storage too small for srp_srv_t");
#define SRV(p) ((srp_srv_t *)(p)->srv_storage)

static const char *TAG = "pair";

#define NVS_NS        "pair"
#define NVS_KEY_TBL   "tbl"

typedef struct {
    uint32_t id;
    uint8_t fp[PAIR_FP_LEN];
} pair_ent_t;

typedef struct {
    uint32_t seq;
    uint8_t n;
    pair_ent_t ent[PAIR_CLIENTS_MAX];
} pair_tbl_t;

#define PAIR_SVC_BYTES (int)sizeof(pair_svc_t)

int pair_size(void) { return PAIR_SVC_BYTES; }

/* ---- 指纹表 NVS ---- */

static int tbl_load(pair_tbl_t *t)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    size_t len = sizeof(*t);
    esp_err_t e = nvs_get_blob(h, NVS_KEY_TBL, t, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*t)) {
        memset(t, 0, sizeof(*t));      /* 空表/表损 → 出厂态 */
        return 0;
    }
    if (t->n > PAIR_CLIENTS_MAX) t->n = 0;    /* 损坏防御 */
    return 0;
}

static int tbl_save(const pair_tbl_t *t)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, NVS_KEY_TBL, t, sizeof(*t));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

/* ---- PIN ---- */

static int pin_gen(const pair_port_t *port, char pin[PAIR_PIN_LEN + 1])
{
    for (int i = 0; i < PAIR_PIN_LEN; i++) {
        for (;;) {
            uint8_t b[2];
            if (port->srp.rng(port->srp.ud, b, 2) != 0) return -1;
            unsigned v = (unsigned)b[0] << 8 | b[1];
            if (v >= 60000) continue;          /* 拒绝采样除 10 模偏 */
            pin[i] = (char)('0' + v % 10);
            break;
        }
    }
    pin[PAIR_PIN_LEN] = '\0';
    return 0;
}

/* ---- API ---- */

int pair_init(pair_svc_t *p, const pair_port_t *port)
{
    if (p == NULL || port == NULL || port->srp.hash == NULL ||
        port->srp.rng == NULL) {
        return PAIR_RES_ERR;
    }
    memset(p, 0, sizeof(*p));
    p->port = *port;
    if (port->now_ms == NULL) {
        /* 时钟缺省: 窗口永续(仅宿主测试便利用法; 固件装配必须给时钟) */
        ;
    }
    p->inited = 1;
    return PAIR_RES_OK;
}

static uint64_t now_of(const pair_svc_t *p)
{
    return p->port.now_ms != NULL ? p->port.now_ms(p->port.srp.ud) : 0;
}

int pair_window_open(pair_svc_t *p, char pin_out[PAIR_PIN_LEN + 1])
{
    if (p == NULL || !p->inited || pin_out == NULL) return PAIR_RES_ERR;
    if (pin_gen(&p->port, (char *)p->pin) != 0) return PAIR_RES_ERR;
    p->window_open = 1;
    p->fails = 0;
    p->exchange_ok = 0;
    {   /* 保留工作区指针(重开窗复用; ws 在 srv_begin 内再清零) */
        srp_ws_t *ws_old = SRV(p)->ws;
        memset(SRV(p), 0, sizeof(srp_srv_t));
        SRV(p)->ws = ws_old;
    }
    p->window_deadline = now_of(p) + PAIR_WINDOW_MS;
    memcpy(pin_out, p->pin, PAIR_PIN_LEN + 1);
    ESP_LOGI(TAG, "pair window open (pin %s, %us)",
             p->pin, (unsigned)(PAIR_WINDOW_MS / 1000));
    return PAIR_RES_OK;
}

int pair_window_active(pair_svc_t *p)
{
    if (p == NULL || !p->inited || !p->window_open) return 0;
    if (p->port.now_ms != NULL && now_of(p) >= p->window_deadline) {
        p->window_open = 0;
        ESP_LOGW(TAG, "pair window expired");
        return 0;
    }
    return 1;
}

int pair_step1(pair_svc_t *p, uint8_t salt[16], uint8_t B[384])
{
    if (p == NULL || !pair_window_active(p) ||
        salt == NULL || B == NULL) {
        return PAIR_RES_WINDOW;
    }
    int rc = srp_srv_begin(SRV(p), &p->port.srp, "espcube",
                           (const char *)p->pin, salt, B);
    return rc == 0 ? PAIR_RES_OK : PAIR_RES_ERR;
}

int pair_step2(pair_svc_t *p, const uint8_t A[384])
{
    if (p == NULL || !pair_window_active(p) || A == NULL) {
        return PAIR_RES_WINDOW;
    }
    return srp_srv_A(SRV(p), A) == 0 ? PAIR_RES_OK : PAIR_RES_ERR;
}

int pair_step3(pair_svc_t *p, const uint8_t m1[32], uint8_t m2_out[32])
{
    if (p == NULL || !pair_window_active(p) || m1 == NULL ||
        m2_out == NULL) {
        return PAIR_RES_WINDOW;
    }
    if (srp_srv_m1_check(SRV(p), m1) != 0) {
        p->fails++;
        ESP_LOGW(TAG, "pair M1 fail (%d/%d)", p->fails, PAIR_MAX_FAILS);
        if (p->fails >= PAIR_MAX_FAILS) {
            p->window_open = 0;        /* 窗口作废: 重开须物理在场 */
            return PAIR_RES_LOCKED;
        }
        return PAIR_RES_ERR;
    }
    srp_srv_m2(SRV(p), m2_out);
    p->exchange_ok = 1;
    ESP_LOGI(TAG, "pair M1/M2 ok");
    return PAIR_RES_OK;
}

const uint8_t *pair_session_key(pair_svc_t *p)
{
    if (p == NULL || !p->exchange_ok) return NULL;
    return srp_srv_key(SRV(p));
}

int pair_bind(pair_svc_t *p, const uint8_t pubkey[PAIR_PUBKEY_LEN],
              uint32_t *client_id_out)
{
    if (p == NULL || !p->exchange_ok || pubkey == NULL ||
        client_id_out == NULL) {
        return PAIR_RES_ERR;
    }
    uint8_t fp[PAIR_FP_LEN];
    p->port.srp.hash(p->port.srp.ud, pubkey, PAIR_PUBKEY_LEN, fp);

    pair_tbl_t t;
    if (tbl_load(&t) != 0) return PAIR_RES_ERR;
    /* 重复绑定(同指纹) → 幂等刷新 */
    for (int i = 0; i < t.n; i++) {
        if (memcmp(t.ent[i].fp, fp, PAIR_FP_LEN) == 0) {
            *client_id_out = t.ent[i].id;
            p->exchange_ok = 0;
            return PAIR_RES_OK;
        }
    }
    if (t.n >= PAIR_CLIENTS_MAX) return PAIR_RES_FULL;
    t.seq += 1;
    t.ent[t.n].id = t.seq;
    memcpy(t.ent[t.n].fp, fp, PAIR_FP_LEN);
    t.n++;
    if (tbl_save(&t) != 0) return PAIR_RES_ERR;
    *client_id_out = t.seq;
    p->exchange_ok = 0;
    ESP_LOGI(TAG, "pair bound client #%lu (n=%d)",
             (unsigned long)t.seq, t.n);
    return PAIR_RES_OK;
}

int pair_client_known(pair_svc_t *p, const uint8_t pubkey[PAIR_PUBKEY_LEN],
                      uint32_t *client_id_out)
{
    if (p == NULL || pubkey == NULL || client_id_out == NULL) {
        return PAIR_RES_ERR;
    }
    uint8_t fp[PAIR_FP_LEN];
    p->port.srp.hash(p->port.srp.ud, pubkey, PAIR_PUBKEY_LEN, fp);
    pair_tbl_t t;
    if (tbl_load(&t) != 0) return PAIR_RES_ERR;
    for (int i = 0; i < t.n; i++) {
        if (memcmp(t.ent[i].fp, fp, PAIR_FP_LEN) == 0) {
            *client_id_out = t.ent[i].id;
            return PAIR_RES_OK;
        }
    }
    return PAIR_RES_NOTFND;
}

int pair_client_revoke(pair_svc_t *p, uint32_t client_id)
{
    if (p == NULL) return PAIR_RES_ERR;
    pair_tbl_t t;
    if (tbl_load(&t) != 0) return PAIR_RES_ERR;
    int found = -1;
    for (int i = 0; i < t.n; i++) {
        if (t.ent[i].id == client_id) { found = i; break; }
    }
    if (found < 0) return PAIR_RES_NOTFND;
    for (int i = found; i < t.n - 1; i++) t.ent[i] = t.ent[i + 1];
    t.n--;
    if (tbl_save(&t) != 0) return PAIR_RES_ERR;
    ESP_LOGI(TAG, "pair revoked client #%lu (n=%d)",
             (unsigned long)client_id, t.n);
    return PAIR_RES_OK;
}

int pair_client_count(pair_svc_t *p)
{
    if (p == NULL) return PAIR_RES_ERR;
    pair_tbl_t t;
    if (tbl_load(&t) != 0) return PAIR_RES_ERR;
    return t.n;
}
