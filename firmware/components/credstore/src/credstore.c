/* credstore.c —— 服务凭据安全存储实现(见 credstore.h; T17 设备侧)
 *
 * 存储: NVS "creds" 区两个 blob:
 *   idx = [u32 seq][u8 n][8×{name[32], ver u32}]     —— 目录(明文:
 *         名字/版本非机密, 供 list 不解密)
 *   <name> = EPSCV2 容器(store_crypt, 值密文; 密钥 HKDF(dev_key,
 *         info="epsc/cred/<name>"), AAD=name)
 * 损坏防御: idx 超界/长度不符 → 出厂态; 值 blob 解不开(AUTH) → 视作
 * 不存在并从目录摘除(dev_key 轮换/清除后旧凭据不可读=设计口径)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "app_storage.h"

#include "credstore.h"
#include "store_crypt.h"

#define NVS_NS   "creds"
#define NVS_IDX  "idx"

typedef struct {
    char name[CRED_NAME_MAX];
    uint32_t ver;
} cred_ent_t;

typedef struct {
    uint32_t seq;
    uint8_t n;
    cred_ent_t ent[CRED_COUNT_MAX];
} cred_idx_t;

/* NVS 文件层适配(store_crypt 的 read_fn/write_fn 语义)。
 * 存储键 = FNV-1a 16 位哈希("c%04x"): NVS 键限 15 字符且禁 '/'/'.';
 * 密码学绑定(AAD/HKDF info)仍用完整 rel(在 store_crypt 内部), 映射
 * 只影响落盘键名。哈希碰撞面: 条目 ≤8, 65536 空间, M1 接受。 */
static void nvs_key_of(const char *rel, char out[8])
{
    uint32_t h = 0x811C9DC5u;
    for (; *rel; rel++) {
        h ^= (uint8_t)*rel;
        h *= 0x01000193u;
    }
    snprintf(out, 8, "c%04x", (unsigned)(h & 0xFFFFu));
}

static char *nvs_read_fn(const char *rel, size_t *len)
{
    char key[8];
    nvs_key_of(rel, key);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t need = 0;
    esp_err_t e = nvs_get_blob(h, key, NULL, &need);
    if (e != ESP_OK || need == 0) {
        nvs_close(h);
        return NULL;
    }
    char *buf = malloc(need);
    if (buf == NULL) {
        nvs_close(h);
        return NULL;
    }
    e = nvs_get_blob(h, key, buf, &need);
    nvs_close(h);
    if (e != ESP_OK) {
        free(buf);
        return NULL;
    }
    *len = need;
    return buf;
}

static int nvs_write_fn(const char *rel, const void *data, size_t len)
{
    char key[8];
    nvs_key_of(rel, key);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static int idx_load(cred_idx_t *ix)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    size_t len = sizeof(*ix);
    esp_err_t e = nvs_get_blob(h, NVS_IDX, ix, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*ix)) {
        memset(ix, 0, sizeof(*ix));
        return 0;
    }
    if (ix->n > CRED_COUNT_MAX) ix->n = 0;
    return 0;
}

static int idx_save(const cred_idx_t *ix)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, NVS_IDX, ix, sizeof(*ix));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static cred_ent_t *idx_find(cred_idx_t *ix, const char *name)
{
    for (int i = 0; i < ix->n; i++) {
        if (strcmp(ix->ent[i].name, name) == 0) return &ix->ent[i];
    }
    return NULL;
}

int credstore_init(credstore_t *c, const uint8_t dev_key[32])
{
    if (c == NULL || dev_key == NULL) return CRED_RES_ERR;
    memset(c, 0, sizeof(*c));
    memcpy(c->dev_key, dev_key, 32);
    int rc = store_crypt_init(&c->crypt, app_storage_crypt_port(),
                              c->dev_key, nvs_read_fn, nvs_write_fn);
    return rc == 0 ? CRED_RES_OK : CRED_RES_ERR;
}

int credstore_set(credstore_t *c, const char *name,
                  const char *value, uint32_t expected,
                  uint32_t *ver_out)
{
    if (c == NULL || name == NULL || value == NULL) return CRED_RES_ERR;
    size_t nl = strlen(name);
    if (nl == 0 || nl >= CRED_NAME_MAX) return CRED_RES_ERR;
    size_t vl = strlen(value);
    if (vl == 0 || vl > CRED_VALUE_MAX) return CRED_RES_TOOBIG;
    for (size_t i = 0; i < nl; i++) {
        /* 名字限定: 字母数字._- (NVS key 兼容+路径注入防御) */
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
              ch == '-')) {
            return CRED_RES_ERR;
        }
    }

    cred_idx_t ix;
    if (idx_load(&ix) != 0) return CRED_RES_ERR;
    cred_ent_t *e = idx_find(&ix, name);
    if (e == NULL) {
        if (expected != 0) return CRED_RES_CONFLICT;   /* 新建语义 */
        if (ix.n >= CRED_COUNT_MAX) return CRED_RES_FULL;
        e = &ix.ent[ix.n++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->ver = 0;
    } else if (expected == 0 || expected != e->ver) {
        /* expected=0=新建语义: 已存在即冲突(双手机并发防护) */
        if (ver_out != NULL) *ver_out = e->ver;
        return CRED_RES_CONFLICT;
    }

    /* 值落盘(EPSCV2 容器; rel=cred/<name> → per-cred 密钥+AAD 绑定) */
    char rel[CRED_NAME_MAX + 8];
    snprintf(rel, sizeof(rel), "cred/%s", name);
    if (store_crypt_write(&c->crypt, rel, value, vl + 1) != 0) {
        return CRED_RES_ERR;
    }
    ix.seq += 1;
    e->ver = ix.seq;
    if (idx_save(&ix) != 0) return CRED_RES_ERR;
    if (ver_out != NULL) *ver_out = e->ver;
    return CRED_RES_OK;
}

int credstore_get(credstore_t *c, const char *name,
                  char *value_out, size_t cap, size_t *len_out)
{
    if (c == NULL || name == NULL || value_out == NULL || len_out == NULL) {
        return CRED_RES_ERR;
    }
    cred_idx_t ix;
    if (idx_load(&ix) != 0) return CRED_RES_ERR;
    if (idx_find(&ix, name) == NULL) return CRED_RES_NOTFND;

    char rel[CRED_NAME_MAX + 8];
    snprintf(rel, sizeof(rel), "cred/%s", name);
    size_t n = 0;
    char buf[CRED_VALUE_MAX + 1];               /* 写侧含 NUL 最长 513B, cap 给满 */
    int rc = store_crypt_read(&c->crypt, rel, buf, sizeof(buf), &n);
    if (rc == 1) return CRED_RES_NOTFND;        /* 无 blob */
    if (rc == -1) return CRED_RES_ERR;          /* 批次 44: IO/cap 不足 ≠ AUTH, 不得摘目录(512B 值曾被误删) */
    if (rc == -2) {
        /* AUTH 失败: dev_key 已轮换/清除 → 从目录摘除(不可读=不存在) */
        cred_ent_t *e = idx_find(&ix, name);
        if (e != NULL) {
            *e = ix.ent[ix.n - 1];
            ix.n--;
            (void)idx_save(&ix);
        }
        return CRED_RES_NOTFND;
    }
    buf[n] = '\0';                              /* 写入时含 NUL, 双保险 */
    size_t vl = strlen(buf);
    if (vl >= cap) return CRED_RES_TOOBIG;
    memcpy(value_out, buf, vl + 1);
    *len_out = vl;
    return CRED_RES_OK;
}

int credstore_delete(credstore_t *c, const char *name,
                     uint32_t expected, uint32_t *ver_out)
{
    if (c == NULL || name == NULL) return CRED_RES_ERR;
    cred_idx_t ix;
    if (idx_load(&ix) != 0) return CRED_RES_ERR;
    cred_ent_t *e = idx_find(&ix, name);
    if (e == NULL) return CRED_RES_NOTFND;
    if (expected != 0 && expected != e->ver) {
        if (ver_out != NULL) *ver_out = e->ver;
        return CRED_RES_CONFLICT;
    }
    char rel[CRED_NAME_MAX + 8];
    snprintf(rel, sizeof(rel), "cred/%s", name);
    (void)nvs_write_fn(rel, "", 0);             /* 值 blob 清空 */
    *e = ix.ent[ix.n - 1];
    ix.n--;
    ix.seq += 1;
    if (idx_save(&ix) != 0) return CRED_RES_ERR;
    return CRED_RES_OK;
}

int credstore_list(credstore_t *c,
                   char names[][CRED_NAME_MAX], uint32_t vers[], int max)
{
    if (c == NULL) return CRED_RES_ERR;
    cred_idx_t ix;
    if (idx_load(&ix) != 0) return CRED_RES_ERR;
    int n = ix.n < max ? ix.n : max;
    for (int i = 0; i < n; i++) {
        if (names != NULL) {
            memcpy(names[i], ix.ent[i].name, CRED_NAME_MAX);
        }
        if (vers != NULL) vers[i] = ix.ent[i].ver;
    }
    return n;
}

int credstore_version(credstore_t *c, const char *name)
{
    if (c == NULL || name == NULL) return CRED_RES_ERR;
    cred_idx_t ix;
    if (idx_load(&ix) != 0) return CRED_RES_ERR;
    cred_ent_t *e = idx_find(&ix, name);
    return e != NULL ? (int)e->ver : -1;
}
