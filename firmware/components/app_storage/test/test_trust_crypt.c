/*
 * test_trust_crypt.c —— 信任区+EPSCV2 容器宿主验收(docs/18 T14; docs/20 §5/§9)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/app_storage/include \
 *       -I firmware/components/app_soul/test/host_stubs \
 *       -o /tmp/test_trust_crypt \
 *       firmware/components/app_storage/test/test_trust_crypt.c \
 *       firmware/components/app_storage/src/trust.c \
 *       firmware/components/app_storage/src/store_crypt.c \
 *   && /tmp/test_trust_crypt
 *
 * 验收(docs/20 §5/§9; T14 H):
 * - trust: 密钥生成/持久化(NVS stub)/重读一致/wipe 清除
 * - EPSCV2: 加密往返/密文非明文/篡改 AUTH 拒/明文兼容读(迁移期)/
 *   nonce 唯一(同内容两次写 blob 不同)/路径绑定(换名读失败)
 * 测试桩 port: 非密码学(仅可逆+校验和 tag)——验证流程与失败语义,
 * 真 GCM/HKDF 正确性由 mbedtls/PSA 自身保证 + 目标构建集成。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trust.h"
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "store_crypt.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- NVS stub(内存表; T20 模式) ---- */
#define NVS_MAX_ENT 8
typedef struct { char key[16]; uint8_t val[64]; size_t len; } nvs_ent;
static nvs_ent s_nvs[NVS_MAX_ENT];
static int s_nvs_n;
static bool s_nvs_fail;

static void nvs_reset(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    s_nvs_n = 0;
    s_nvs_fail = false;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns; (void)mode;
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v,
                       size_t n)
{
    (void)h;
    if (s_nvs_fail) return 1;
    nvs_ent *e = NULL;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) { e = &s_nvs[i]; break; }
    }
    if (e == NULL) {
        if (s_nvs_n >= NVS_MAX_ENT) return 1;
        e = &s_nvs[s_nvs_n++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->len = n < sizeof(e->val) ? n : sizeof(e->val);
    memcpy(e->val, v, e->len);
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *p, size_t *n)
{
    (void)h;
    nvs_ent *e = NULL;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) { e = &s_nvs[i]; break; }
    }
    if (e == NULL) return 1;
    size_t k = *n < e->len ? *n : e->len;
    memcpy(p, e->val, k);
    *n = e->len;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; s_nvs_n = 0; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ (void)h; (void)key; return ESP_OK; }

/* 确定性 RNG(trust 用; 可断言密钥稳定) */
static uint64_t rng_seq;
static int trust_rng(void *ud, uint8_t *out, size_t len)
{
    (void)ud;
    rng_seq++;
    uint64_t v = rng_seq * 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(v >> ((i % 8) * 8));
    return 0;
}

/* ---- T21/T22 用户裁决复用: 测试即验证机制, 非真实资料 ---- */

static void scenario_trust(void)
{
    nvs_reset();
    trust_rng_t tr = { trust_rng, NULL };
    trust_ctx_t tc;
    CHECK(trust_init(&tc, &tr) == 0, "init");
    uint8_t k1[32], k2[32], k1b[32];
    CHECK(trust_dev_key(&tc, k1) == 0, "dev key generated");
    CHECK(trust_sd_key(&tc, k2) == 0, "sd key generated");
    CHECK(memcmp(k1, k2, 32) != 0, "dev != sd");
    CHECK(trust_dev_key(&tc, k1b) == 0 && memcmp(k1, k1b, 32) == 0,
          "dev key stable (nvs read-back)");

    /* "重启": 新 ctx 同 NVS → 密钥一致 */
    trust_ctx_t tc2;
    trust_rng_t tr2 = { trust_rng, NULL };
    trust_init(&tc2, &tr2);
    uint8_t k1c[32];
    CHECK(trust_dev_key(&tc2, k1c) == 0 && memcmp(k1, k1c, 32) == 0,
          "key survives reboot");
    /* RNG 序列推进不改变已持久密钥 */
    CHECK(trust_sd_key(&tc2, k2) == 0 && memcmp(k2, k2, 32) == 0,
          "sd key unchanged");
    CHECK(memcmp(k2, k1, 32) != 0, "sd != dev after reboot");

    CHECK(trust_wipe(&tc2) == 0, "wipe");
    trust_ctx_t tc3;
    trust_rng_t tr3 = { trust_rng, NULL };
    trust_init(&tc3, &tr3);
    uint8_t k3[32];
    CHECK(trust_dev_key(&tc3, k3) == 0, "regen after wipe");
    CHECK(memcmp(k3, k1, 32) != 0, "fresh key differs");
}

/* ---- crypt 测试桩 port: 可逆变换 + 校验和 tag(非密码学, 验流程) ---- */
static unsigned sc_rng_seq;
static int sc_rng(void *ud, uint8_t *out, size_t len)
{
    (void)ud;
    sc_rng_seq++;                                /* 每次调用不同 → nonce 唯一 */
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
    return sc_tag_of(key, out, len) == tag[0] ? 0 : -1;   /* 校验失败=AUTH */
}
/* HKDF 桩: 输出 = key^salt 序列(确定, 隔离不同文件) */
static int sc_hkdf(void *ud, const uint8_t ikm[32], const uint8_t *salt,
                   size_t salt_len, const char *info, size_t info_len,
                   uint8_t out[32])
{
    (void)ud;
    /* 简化扩散(非密码学): 多轮 XOR 移位, 使任意输入差异扩散到全 key */
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

/* 内存文件层 */
#define TIO_CAP 8192
typedef struct { uint8_t data[2][TIO_CAP]; size_t len[2]; int present[2]; } tio_t;
static tio_t s_tio;
static int io_read(void *ud, int slot, void *buf, size_t cap, size_t *len)
{
    (void)ud;
    if (!s_tio.present[slot]) return 2;
    size_t n = s_tio.len[slot] < cap ? s_tio.len[slot] : cap;
    memcpy(buf, s_tio.data[slot], n);
    *len = n;
    return 0;
}
static int io_write(void *ud, int slot, const void *buf, size_t len)
{
    (void)ud;
    if (len > TIO_CAP) return 1;
    memcpy(s_tio.data[slot], buf, len);
    s_tio.len[slot] = len;
    s_tio.present[slot] = 1;
    return 0;
}

/* 宿主桩: heap_caps → 直通 malloc(host_stubs/esp_heap_caps.h) */
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }

/* ---- EPSCV2 场景 ---- */
static store_crypt_t s_c;
static const sc_port_t s_stub_port = {
    sc_gcm_enc, sc_gcm_dec, sc_hkdf, sc_rng, NULL,
};
static int stub_slot(const char *rel) { return strstr(rel, "/a/") ? 0 : 1; }
static char *stub_read(const char *rel, size_t *len)
{
    int slot = stub_slot(rel);
    if (!s_tio.present[slot]) return NULL;
    char *buf = malloc(s_tio.len[slot] + 1);
    if (buf == NULL) return NULL;
    memcpy(buf, s_tio.data[slot], s_tio.len[slot]);
    buf[s_tio.len[slot]] = 0;
    *len = s_tio.len[slot];
    return buf;
}
static int stub_write(const char *rel, const void *data, size_t len)
{
    return io_write(NULL, stub_slot(rel), data, len);
}

static uint8_t s_key[32];
static void crypt_setup(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
    memset(s_key, 0xB7, sizeof(s_key));        /* 固定测试密钥 */
    CHECK(store_crypt_init(&s_c, &s_stub_port, s_key,
                           stub_read, stub_write) == 0, "crypt init");
}

static void scenario_crypt_roundtrip(void)
{
    crypt_setup();
    const char *data = "记忆内容: 用户喜欢茉莉花茶";
    CHECK(store_crypt_write(&s_c, "p/a/episodes.tsv", data,
                            strlen(data)) == 0, "enc write");
    char out[128];
    size_t n = 0;
    CHECK(store_crypt_read(&s_c, "p/a/episodes.tsv", out, sizeof(out), &n) == 0,
          "dec read");
    CHECK(n == strlen(data) && memcmp(out, data, n) == 0, "roundtrip equal");

    /* 密文非明文 */
    uint8_t raw[TIO_CAP];
    size_t rn = 0;
    io_read(NULL, 0, raw, sizeof(raw), &rn);
    bool leaked = false;
    for (size_t i = 0; i + strlen(data) <= rn && !leaked; i++) {
        leaked = (memcmp(raw + i, data, strlen(data)) == 0);
    }
    CHECK(!leaked, "ciphertext does not contain plaintext");
}

static void scenario_tamper_and_compat(void)
{
    crypt_setup();
    CHECK(store_crypt_write(&s_c, "p/a/episodes.tsv", "秘密", 6) == 0,
          "write");
    /* 篡改密文 1 字节 → AUTH 拒 */
    s_tio.data[0][34] ^= 0x01;   /* cipher 区(32-38) */
    char out[64];
    size_t n = 0;
    CHECK(store_crypt_read(&s_c, "p/a/episodes.tsv", out, sizeof(out), &n) == -2,
          "tamper rejected");

    /* 明文兼容读(迁移期): 无 magic 文件透传 */
    s_tio.present[0] = 1;
    memcpy(s_tio.data[0], "旧明文行", 12);
    s_tio.len[0] = 12;
    int prc = store_crypt_read(&s_c, "p/a/episodes.tsv", out, sizeof(out), &n);
    CHECK(prc == 0 && n == 12 && memcmp(out, "旧明文行", 12) == 0,
          "legacy plaintext passthrough");

    /* 明文兼容 × 长度查询模式(buf=NULL)——§55.16 事件 A 根因回归:
     * 原实现返回 len=0 且不设 cache → app_storage_read 判空 →
     * crypt 激活后卡上全部明文资产(表情素材等)静默 BAD_FILE */
    size_t qn = 999;
    int qrc = store_crypt_read(&s_c, "p/a/episodes.tsv", NULL, 0, &qn);
    CHECK(qrc == 0, "plaintext len-query rc");
    CHECK(qn == 12, "plaintext len-query returns real length");
    const void *pl = store_crypt_payload(&s_c);
    CHECK(pl != NULL && memcmp(pl, "旧明文行", 12) == 0,
          "plaintext len-query payload via cache");

    /* nonce 唯一: 同内容两次写 → blob 不同 */
    uint8_t blob1[TIO_CAP], blob2[TIO_CAP];
    CHECK(store_crypt_write(&s_c, "x/y", "同内容", 9) == 0, "w1");
    memcpy(blob1, s_tio.data[1], s_tio.len[1]);
    CHECK(store_crypt_write(&s_c, "x/y", "同内容", 9) == 0, "w2");
    memcpy(blob2, s_tio.data[1], s_tio.len[1]);
    CHECK(memcmp(blob1, blob2, s_tio.len[1]) != 0, "nonce unique (blobs differ)");

    /* 路径绑定: A 文件的密文用 B 路径读 → AUTH 拒 */
    CHECK(store_crypt_write(&s_c, "p/a/x.tsv", "数据", 6) == 0, "write A");
    s_tio.present[1] = 1;
    memcpy(s_tio.data[1], s_tio.data[0], s_tio.len[0]);
    s_tio.len[1] = s_tio.len[0];
    int brc = store_crypt_read(&s_c, "p/b/x.tsv", out, sizeof(out), &n);
    CHECK(brc == -2, "path binding enforced");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "trust",       scenario_trust },
        { "roundtrip",   scenario_crypt_roundtrip },
        { "tamper",      scenario_tamper_and_compat },
    };
    if (argc > 1) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (strcmp(argv[1], cases[i].name) == 0) {
                cases[i].fn();
                printf("%s: %s\n", cases[i].name, s_fail ? "FAIL" : "PASS");
                return s_fail ? 1 : 0;
            }
        }
        fprintf(stderr, "no such scenario: %s\n", argv[1]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = s_fail;
        cases[i].fn();
        printf("%s scenario %s\n", s_fail > before ? "FAIL" : "PASS",
               cases[i].name);
    }
    if (s_fail == 0) {
        printf("test_trust_crypt: ALL PASS\n");
        return 0;
    }
    printf("test_trust_crypt: %d FAILURE(S)\n", s_fail);
    return 1;
}
