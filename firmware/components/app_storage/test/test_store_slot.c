/*
 * test_store_slot.c —— 双槽可恢复提交协议回归(宿主, 自带 main; docs/18 T07)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -Wall -Wextra -Wno-unused-parameter \
 *       -I firmware/components/app_storage/include \
 *       -o /tmp/test_store_slot \
 *       firmware/components/app_storage/test/test_store_slot.c \
 *       firmware/components/app_storage/src/store_slot.c \
 *   && /tmp/test_store_slot
 *
 * IO 用内存双槽模拟(协议测试与介质无关); 每个提交步骤可注入失败:
 * staging 写失败 / 回读失败 / 提交位翻转失败 / 短写 / 位损坏。
 * "重启" = 重新 store_slot_init(协议无隐藏全局态)。
 * 验收(docs/18 T07): 每个提交点故障后恢复到"可验证提交代或显式报错",
 * 不得静默空库或覆盖旧数据。
 */
#include <stdio.h>
#include <string.h>

#include "store_slot.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- 内存双槽 IO(含故障注入) ---- */
#define TIO_CAP 2048
typedef struct {
    uint8_t data[2][TIO_CAP];
    size_t len[2];
    int present[2];
    int fail_read_nth;        /* 第 n 次 read 返回 IO(1 起; 0=不注) */
    int fail_wfull_nth;
    int fail_wat_nth;
    size_t short_n;           /* 下一次 write_full 截断字节数(短写) */
    int nr, nw, nwa;
} tio_t;
static tio_t s_tio;

static void tio_reset(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
}
static int io_read(void *ud, int slot, void *buf, size_t cap, size_t *len)
{
    (void)ud;
    s_tio.nr++;
    if (s_tio.fail_read_nth == s_tio.nr) return 1;
    if (!s_tio.present[slot]) return 2;
    size_t n = s_tio.len[slot] < cap ? s_tio.len[slot] : cap;
    memcpy(buf, s_tio.data[slot], n);
    *len = n;
    return 0;
}
static int io_write_full(void *ud, int slot, const void *buf, size_t len)
{
    (void)ud;
    s_tio.nw++;
    if (s_tio.fail_wfull_nth == s_tio.nw) return 1;
    if (len > TIO_CAP) return 1;
    size_t eff = (s_tio.short_n && s_tio.short_n < len) ? len - s_tio.short_n
                 : len;
    s_tio.short_n = 0;
    memcpy(s_tio.data[slot], buf, eff);
    s_tio.len[slot] = eff;
    s_tio.present[slot] = 1;
    return 0;
}
static int io_write_at(void *ud, int slot, size_t off, const void *buf,
                       size_t len)
{
    (void)ud;
    s_tio.nwa++;
    if (s_tio.fail_wat_nth == s_tio.nwa) return 1;
    if (!s_tio.present[slot] || off + len > s_tio.len[slot]) return 1;
    memcpy(s_tio.data[slot] + off, buf, len);
    return 0;
}
static const store_io_t s_io = {
    io_read, io_write_full, io_write_at, NULL,
};

/* ---- 镜像头部(布局须与实现一致) ---- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t hdr_ver;
    uint16_t committed;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t hdr_crc;
} mir_hdr_t;
#pragma pack(pop)
_Static_assert(sizeof(mir_hdr_t) == 24, "镜像头部应 24B");
#define MIR_MAGIC 0x5354534Cu

static void craft_slot(int slot, uint32_t gen, int committed,
                       const char *payload, size_t plen)
{
    mir_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = MIR_MAGIC;
    h.hdr_ver = 1;
    h.committed = (uint16_t)committed;
    h.generation = gen;
    h.payload_len = (uint32_t)plen;
    h.payload_crc = store_crc32(payload, plen);
    h.hdr_crc = store_crc32(&h, 20);
    memcpy(s_tio.data[slot], &h, sizeof(h));
    memcpy(s_tio.data[slot] + sizeof(h), payload, plen);
    s_tio.len[slot] = sizeof(h) + plen;
    s_tio.present[slot] = 1;
}
static void corrupt_slot_byte(int slot, size_t off)
{
    s_tio.data[slot][off] ^= 0xFF;
}

/* ---- 场景 ---- */

static void scenario_happy(void)
{
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    CHECK(store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_EMPTY, "empty init");
    CHECK(store_slot_generation(&s) == 0, "gen0");

    uint32_t g = 0;
    CHECK(store_slot_commit(&s, "v1", 2, &g) == STORE_STATUS_OK &&
          g == 1, "commit v1");
    CHECK(s.active_slot == STORE_SLOT_A, "slot A active");
    char buf[64];
    size_t n = 0;
    CHECK(store_slot_read(&s, buf, sizeof(buf), &n) == STORE_STATUS_OK &&
          n == 2 && memcmp(buf, "v1", 2) == 0, "read v1");

    CHECK(store_slot_commit(&s, "v2-longer-payload", 16, &g) ==
          STORE_STATUS_OK && g == 2, "commit v2");
    CHECK(s.active_slot == STORE_SLOT_B, "slot B active");
    CHECK(store_slot_read(&s, buf, sizeof(buf), &n) == STORE_STATUS_OK &&
          n == 16 && memcmp(buf, "v2-longer-payload", 16) == 0, "read v2");

    CHECK(store_slot_commit(&s, "v3", 3, &g) == STORE_STATUS_OK && g == 3,
          "commit v3 reuses A");
    CHECK(s.active_slot == STORE_SLOT_A, "slot A reused");
}

/* staging 写失败: 旧代不动 */
static void scenario_staging_write_fail(void)
{
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    uint32_t g = 0;
    store_slot_commit(&s, "v1", 2, &g);

    s_tio.fail_wfull_nth = s_tio.nw + 1;
    CHECK(store_slot_commit(&s, "v2", 2, &g) == STORE_STATUS_IO_ERROR,
          "staging fail reported");
    s_tio.fail_wfull_nth = 0;

    store_slot_t s2;                          /* 重启恢复 */
    CHECK(store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_OK, "init after fail");
    CHECK(store_slot_generation(&s2) == 1 && s2.active_slot == STORE_SLOT_A,
          "old generation kept");
    char buf[64];
    size_t n = 0;
    store_slot_read(&s2, buf, sizeof(buf), &n);
    CHECK(n == 2 && memcmp(buf, "v1", 2) == 0, "payload v1 intact");
}

/* 提交位翻转失败: staging 被忽略 */
static void scenario_flip_fail(void)
{
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    uint32_t g = 0;
    store_slot_commit(&s, "v1", 2, &g);

    s_tio.fail_wat_nth = s_tio.nwa + 1;       /* 步骤3 翻转失败 */
    CHECK(store_slot_commit(&s, "v2", 2, &g) == STORE_STATUS_IO_ERROR,
          "flip fail reported");
    s_tio.fail_wat_nth = 0;

    store_slot_t s2;
    CHECK(store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_OK, "init after flip fail");
    CHECK(store_slot_generation(&s2) == 1, "gen stays 1");
    /* B 槽留着 staging(committed=0), 下次提交应覆盖它并成功 */
    uint32_t g2 = 0;
    CHECK(store_slot_commit(&s2, "v2-fixed", 8, &g2) == STORE_STATUS_OK &&
          g2 == 2, "next commit succeeds over staging");
    char buf[64];
    size_t n = 0;
    store_slot_read(&s2, buf, sizeof(buf), &n);
    CHECK(n == 8 && memcmp(buf, "v2-fixed", 8) == 0, "v2-fixed readable");
}

/* staging 回读失败(注入) / 短写: 均不动旧代 */
static void scenario_readback_and_short(void)
{
    /* 回读失败 */
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    uint32_t g = 0;
    store_slot_commit(&s, "v1", 2, &g);
    s_tio.fail_read_nth = s_tio.nr + 1;       /* commit 步骤2 的回读 */
    CHECK(store_slot_commit(&s, "v2", 2, &g) == STORE_STATUS_IO_ERROR,
          "readback fail reported");
    s_tio.fail_read_nth = 0;
    store_slot_t s2;
    store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch));
    CHECK(store_slot_generation(&s2) == 1, "gen1 after readback fail");

    /* 短写: staging 长度不足 → 校验拒绝 */
    tio_reset();
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    store_slot_commit(&s, "v1", 2, &g);
    s_tio.short_n = 5;
    CHECK(store_slot_commit(&s, "v2", 2, &g) == STORE_STATUS_IO_ERROR,
          "short write detected");
    s_tio.short_n = 0;
    store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch));
    CHECK(store_slot_generation(&s2) == 1 && s2.active_slot == STORE_SLOT_A,
          "gen1 kept after short write");
}

/* 选最高有效提交代; 更高代损坏时退回低代 */
static void scenario_pick_best(void)
{
    tio_reset();
    craft_slot(STORE_SLOT_A, 2, 1, "AAA", 3);
    craft_slot(STORE_SLOT_B, 3, 1, "BBB", 3);
    uint8_t scratch[512];
    store_slot_t s;
    CHECK(store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_OK, "init dual");
    CHECK(store_slot_generation(&s) == 3 && s.active_slot == STORE_SLOT_B,
          "highest valid gen");

    corrupt_slot_byte(STORE_SLOT_B, 25);      /* 破坏 B 的 payload(24 头 + 1) */
    store_slot_t s2;
    CHECK(store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_OK, "init after corrupt");
    CHECK(store_slot_generation(&s2) == 2 && s2.active_slot == STORE_SLOT_A,
          "falls back to gen2");
    char buf[64];
    size_t n = 0;
    store_slot_read(&s2, buf, sizeof(buf), &n);
    CHECK(n == 3 && memcmp(buf, "AAA", 3) == 0, "gen2 payload");
}

/* 有文件但无有效提交代 → RECOVERY_REQUIRED 且拒绝写 */
static void scenario_recovery_required(void)
{
    tio_reset();
    craft_slot(STORE_SLOT_A, 1, 1, "AAA", 3);
    corrupt_slot_byte(STORE_SLOT_A, 26);      /* payload CRC 破坏 */
    craft_slot(STORE_SLOT_B, 2, 0, "BBB", 3); /* 仅 staging */
    uint8_t scratch[512];
    store_slot_t s;
    CHECK(store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_RECOVERY_REQUIRED, "recovery required");
    uint32_t g = 0;
    CHECK(store_slot_commit(&s, "v", 1, &g) == STORE_STATUS_RECOVERY_REQUIRED,
          "commit refused");
    /* 现场未被覆盖(旧字节仍在) */
    corrupt_slot_byte(STORE_SLOT_A, 26);      /* 还原后可恢复 */
    store_slot_t s2;
    CHECK(store_slot_init(&s2, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_OK && store_slot_generation(&s2) == 1,
          "recoverable after revert");
}

/* 活动槽运行中劣化: read 报 RECOVERY_REQUIRED, commit 停写 */
static void scenario_live_degrade(void)
{
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    uint32_t g = 0;
    store_slot_commit(&s, "v1", 2, &g);
    corrupt_slot_byte(s.active_slot, 25);   /* payload 区(24头+1) */
    char buf[64];
    size_t n = 0;
    CHECK(store_slot_read(&s, buf, sizeof(buf), &n) ==
          STORE_STATUS_RECOVERY_REQUIRED, "live degrade detected");
    CHECK(store_slot_commit(&s, "v2", 2, &g) == STORE_STATUS_RECOVERY_REQUIRED,
          "write stop after degrade");
}

/* 头部 CRC 篡改 → 槽无效 */
static void scenario_hdr_tamper(void)
{
    tio_reset();
    craft_slot(STORE_SLOT_A, 1, 1, "AAA", 3);
    corrupt_slot_byte(STORE_SLOT_A, 18);      /* 头部区域(前 20B 内) */
    uint8_t scratch[512];
    store_slot_t s;
    CHECK(store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch)) ==
          STORE_STATUS_RECOVERY_REQUIRED, "hdr tamper invalidates");
}

/* 容量守卫 */
static void scenario_guards(void)
{
    tio_reset();
    uint8_t scratch[512];
    store_slot_t s;
    CHECK(store_slot_init(&s, &s_io, 256, scratch, 128) ==
          STORE_STATUS_INVALID, "scratch too small");
    store_slot_init(&s, &s_io, 256, scratch, sizeof(scratch));
    uint32_t g = 0;
    CHECK(store_slot_commit(&s, "x", 300, &g) == STORE_STATUS_INVALID,
          "payload over max");
    CHECK(store_slot_commit(&s, "x", 0, &g) == STORE_STATUS_INVALID,
          "empty payload rejected");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "happy",            scenario_happy },
        { "staging_fail",     scenario_staging_write_fail },
        { "flip_fail",        scenario_flip_fail },
        { "readback_short",   scenario_readback_and_short },
        { "pick_best",        scenario_pick_best },
        { "recovery",         scenario_recovery_required },
        { "live_degrade",     scenario_live_degrade },
        { "hdr_tamper",       scenario_hdr_tamper },
        { "guards",           scenario_guards },
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
        printf("test_store_slot: ALL PASS\n");
        return 0;
    }
    printf("test_store_slot: %d FAILURE(S)\n", s_fail);
    return 1;
}
