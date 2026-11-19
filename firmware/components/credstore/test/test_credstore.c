/*
 * test_credstore.c —— 凭据安全存储宿主验收(docs/18 T17 设备侧; 批次 22)
 *
 * 编译运行(tools/host_tests.sh 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/credstore/include \
 *       -I firmware/components/app_storage/include \
 *       -I firmware/components/app_soul/test/host_stubs \
 *       -o /tmp/test_credstore \
 *       firmware/components/credstore/test/test_credstore.c \
 *       firmware/components/credstore/src/credstore.c \
 *       firmware/components/app_storage/src/store_crypt.c \
 *   && /tmp/test_credstore
 *
 * 验收(T17: 存储检查不见密钥明文):
 * - set/get 往返 + 版本单调
 * - **at-rest 非明文**: NVS 表里扫不到值原文(EPSCV2 容器)
 * - 版本冲突全集: 新建 expected≠0 拒 / 轮换 expected 错 → CONFLICT+当前
 *   版本回传 / 正确 expected → ver 单调; delete 同语义
 * - delete 后 get NOTFND; 重建 ver 继续(seq 不复用)
 * - 名字注入防御(非法字符拒) / TOOBIG / FULL
 * - dev_key 变更(模拟恢复出厂) → 旧值 AUTH 拒 → 目录摘除 → NOTFND
 * 测试桩: 可逆变换+校验和 tag port(同 test_trust_crypt)+NVS 内存表。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "credstore.h"

#include "esp_err.h"
#include "nvs.h"

/* 宿主桩: heap_caps → 直通 malloc(store_crypt 链入, host_stubs 头) */
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- NVS stub(同 test_trust_crypt 模式; 带内容扫描接口) ---- */
#define NVS_MAX_ENT 16
typedef struct { char key[48]; uint8_t val[2048]; size_t len; } nvs_ent;
static nvs_ent s_nvs[NVS_MAX_ENT];
static int s_nvs_n;

static void nvs_reset(void) { memset(s_nvs, 0, sizeof(s_nvs)); s_nvs_n = 0; }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v,
                       size_t n)
{
    (void)h;
    if (n > 2048) return 1;
    nvs_ent *e = NULL;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) { e = &s_nvs[i]; break; }
    }
    if (e == NULL) {
        if (s_nvs_n >= NVS_MAX_ENT) return 1;
        e = &s_nvs[s_nvs_n++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->len = n;
    memcpy(e->val, v, n);
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *p, size_t *n)
{
    (void)h;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            size_t k = *n < s_nvs[i].len ? *n : s_nvs[i].len;
            if (p != NULL && k > 0) memcpy(p, s_nvs[i].val, k);
            *n = s_nvs[i].len;
            return ESP_OK;
        }
    }
    return 1;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; s_nvs_n = 0; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ (void)h; (void)key; return ESP_OK; }

static bool nvs_contains(const char *needle, size_t n)
{
    for (int i = 0; i < s_nvs_n; i++) {
        for (size_t off = 0; off + n <= s_nvs[i].len; off++) {
            if (memcmp(s_nvs[i].val + off, needle, n) == 0) return true;
        }
    }
    return false;
}

/* ---- 测试桩 port(可逆变换+校验和 tag; 同 test_trust_crypt) ---- */
static unsigned sc_rng_seq;
static int sc_rng(void *ud, uint8_t *out, size_t len)
{
    (void)ud;
    sc_rng_seq++;
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(0xA5 ^ sc_rng_seq ^ (i * 7));
    }
    return 0;
}
static uint8_t sc_tag_of(const uint8_t key[32], const uint8_t *data,
                         size_t len)
{
    uint8_t t = 0x5A;
    for (size_t i = 0; i < 32; i++) t ^= key[i];
    for (size_t i = 0; i < len; i++) t ^= data[i] ^ (uint8_t)i;
    return t;
}
static int sc_gcm_enc(void *ud, const uint8_t key[32],
                      const uint8_t gcm_nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *plain, size_t len,
                      uint8_t *out, uint8_t tag[16])
{
    (void)ud; (void)gcm_nonce; (void)aad; (void)aad_len;
    for (size_t i = 0; i < len; i++) out[i] = plain[i] ^ key[i % 32] ^ 0x33;
    memset(tag, 0, 16);
    tag[0] = sc_tag_of(key, plain, len);
    return 0;
}
static int sc_gcm_dec(void *ud, const uint8_t key[32],
                      const uint8_t gcm_nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *cipher, size_t len,
                      const uint8_t tag[16], uint8_t *out)
{
    (void)ud; (void)gcm_nonce; (void)aad; (void)aad_len;
    for (size_t i = 0; i < len; i++) out[i] = cipher[i] ^ key[i % 32] ^ 0x33;
    return sc_tag_of(key, out, len) == tag[0] ? 0 : -1;
}
static int sc_hkdf(void *ud, const uint8_t ikm[32], const uint8_t *salt,
                   size_t salt_len, const char *info, size_t info_len,
                   uint8_t out[32])
{
    (void)ud;
    uint8_t tmp[32];
    for (int i = 0; i < 32; i++) {
        tmp[i] = (uint8_t)(ikm[i] ^ salt[i % salt_len]
                           ^ (info_len > 0 ? info[i % info_len] : 0));
    }
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 32; i++) {
            uint8_t prev = tmp[(i + 31) % 32];
            tmp[i] = (uint8_t)(tmp[i] * 31 + prev + round + 1);
        }
    }
    memcpy(out, tmp, 32);
    return 0;
}

/* 内存"PSA 端口"(替代 app_storage_crypt_port——宿主无 PSA) */
static const sc_port_t s_test_port = {
    sc_gcm_enc, sc_gcm_dec, sc_hkdf, sc_rng, NULL,
};
const sc_port_t *app_storage_crypt_port(void) { return &s_test_port; }

static uint8_t s_key1[32], s_key2[32];
static void keys_init(void)
{
    for (int i = 0; i < 32; i++) { s_key1[i] = (uint8_t)(0x11 + i); }
    for (int i = 0; i < 32; i++) { s_key2[i] = (uint8_t)(0xA0 + i * 2); }
}

static void scenario_roundtrip(void)
{
    nvs_reset();
    keys_init();
    credstore_t c;
    CHECK(credstore_init(&c, s_key1) == CRED_RES_OK, "init");

    uint32_t ver = 0;
    CHECK(credstore_set(&c, "llm.api_key", "sk-test-1234567890",
                        0, &ver) == CRED_RES_OK && ver == 1,
          "create ver=1");
    /* at-rest 非明文(T17 核心): NVS 全表扫不到原文 */
    CHECK(!nvs_contains("sk-test-1234567890", 18),
          "at-rest no plaintext");
    CHECK(!nvs_contains("sk-test", 7), "at-rest no prefix leak");

    char val[512];
    size_t n = 0;
    CHECK(credstore_get(&c, "llm.api_key", val, sizeof(val), &n)
          == CRED_RES_OK && n == 18 && strcmp(val, "sk-test-1234567890") == 0,
          "get roundtrip");
    CHECK(credstore_version(&c, "llm.api_key") == 1, "version 1");

    /* 轮换: 正确 expected → ver 单调 */
    CHECK(credstore_set(&c, "llm.api_key", "sk-rotated-abcdef",
                        1, &ver) == CRED_RES_OK && ver == 2,
          "rotate ver=2");
    CHECK(credstore_get(&c, "llm.api_key", val, sizeof(val), &n)
          == CRED_RES_OK && strcmp(val, "sk-rotated-abcdef") == 0,
          "get after rotate");
    CHECK(!nvs_contains("sk-rotated-abcdef", 16), "rotated at-rest no plain");

    /* list 不含值 */
    char names[8][CRED_NAME_MAX];
    uint32_t vers[8];
    int n_items = credstore_list(&c, names, vers, 8);
    CHECK(n_items == 1 && strcmp(names[0], "llm.api_key") == 0 && vers[0] == 2,
          "list name+ver only");
}

static void scenario_conflict(void)
{
    nvs_reset();
    keys_init();
    credstore_t c;
    credstore_init(&c, s_key1);
    uint32_t ver = 0;
    CHECK(credstore_set(&c, "asr.key", "v1", 0, &ver) == CRED_RES_OK,
          "conf setup");

    /* 新建语义: 已存在 + expected=0 → CONFLICT */
    CHECK(credstore_set(&c, "asr.key", "x", 0, &ver) == CRED_RES_CONFLICT
          && ver == 1, "create-on-existing conflict");

    /* 轮换: expected 错 → CONFLICT + 当前版本回传 */
    CHECK(credstore_set(&c, "asr.key", "v2", 7, &ver) == CRED_RES_CONFLICT
          && ver == 1, "stale expected conflict");
    /* 正确 expected → 过 */
    CHECK(credstore_set(&c, "asr.key", "v2", 1, &ver) == CRED_RES_OK
          && ver == 2, "rotate ok");
    /* 删除: expected 错 → CONFLICT; 对 → OK; 再删 NOTFND */
    CHECK(credstore_delete(&c, "asr.key", 1, &ver) == CRED_RES_CONFLICT
          && ver == 2, "delete stale conflict");
    CHECK(credstore_delete(&c, "asr.key", 2, &ver) == CRED_RES_OK,
          "delete ok");
    char val[64];
    size_t n = 0;
    CHECK(credstore_get(&c, "asr.key", val, sizeof(val), &n)
          == CRED_RES_NOTFND, "get after delete");
    /* 重建: ver 继续(seq 不复用) */
    CHECK(credstore_set(&c, "asr.key", "v3", 0, &ver) == CRED_RES_OK
          && ver == 4, "recreate ver continues");
    /* delete 不存在 */
    CHECK(credstore_delete(&c, "nope.key", 0, &ver) == CRED_RES_NOTFND,
          "delete missing");

    /* 名字注入防御 / TOOBIG */
    CHECK(credstore_set(&c, "bad/name", "x", 0, &ver) == CRED_RES_ERR,
          "slash in name rejected");
    CHECK(credstore_set(&c, "bad;name", "x", 0, &ver) == CRED_RES_ERR,
          "semicolon rejected");
    char big[CRED_VALUE_MAX + 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK(credstore_set(&c, "big.key", big, 0, &ver) == CRED_RES_TOOBIG,
          "TOOBIG");
}

static void scenario_keyrotation(void)
{
    nvs_reset();
    keys_init();
    credstore_t c;
    credstore_init(&c, s_key1);
    uint32_t ver = 0;
    CHECK(credstore_set(&c, "llm.api_key", "secret-under-key1",
                        0, &ver) == CRED_RES_OK, "kr setup");

    /* 模拟恢复出厂: 新实例 + 不同 dev_key → 旧值 AUTH 拒 → 目录摘除 */
    credstore_t c2;
    CHECK(credstore_init(&c2, s_key2) == CRED_RES_OK, "reinit new key");
    char val[64];
    size_t n = 0;
    CHECK(credstore_get(&c2, "llm.api_key", val, sizeof(val), &n)
          == CRED_RES_NOTFND, "old value unreadable under new key");
    /* 目录已摘除: 原实例也读不到了 */
    CHECK(credstore_get(&c, "llm.api_key", val, sizeof(val), &n)
          == CRED_RES_NOTFND, "pruned from index");
    int n_items = credstore_list(&c, NULL, NULL, 8);
    CHECK(n_items == 0, "list empty after prune");

    /* 新 key 下重写 → 可读 */
    CHECK(credstore_set(&c2, "llm.api_key", "under-key2", 0, &ver)
          == CRED_RES_OK, "rewrite under new key");
    CHECK(credstore_get(&c2, "llm.api_key", val, sizeof(val), &n)
          == CRED_RES_OK && strcmp(val, "under-key2") == 0,
          "read under new key");

    /* FULL 语义: 8 条后第 9 条拒 */
    for (int i = 0; i < CRED_COUNT_MAX - 1; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "k%02d.key", i);
        CHECK(credstore_set(&c2, nm, "v", 0, &ver) == CRED_RES_OK,
              "fill to max");
    }
    CHECK(credstore_set(&c2, "overflow.key", "v", 0, &ver) == CRED_RES_FULL,
          "FULL rejected");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "roundtrip",    scenario_roundtrip },
        { "conflict",     scenario_conflict },
        { "keyrotation",  scenario_keyrotation },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = s_fail;
        cases[i].fn();
        printf("%s %s\n", s_fail > before ? "FAIL" : "PASS", cases[i].name);
    }
    if (s_fail == 0) {
        printf("test_credstore: ALL PASS\n");
        return 0;
    }
    printf("test_credstore: %d FAILURE(S)\n", s_fail);
    return 1;
}
