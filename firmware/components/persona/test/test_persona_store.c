/*
 * test_persona_store.c —— 人格持久化回归(宿主; docs/18 T20)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/persona/include \
 *       -I firmware/components/app_storage/include \
 *       -o /tmp/test_persona_store \
 *       firmware/components/persona/test/test_persona_store.c \
 *       firmware/components/persona/src/persona_core.c \
 *       firmware/components/app_storage/src/store_slot.c \
 *   && /tmp/test_persona_store
 *
 * 验收(docs/18 T20 宿主部分): 角色底座+授权经双槽往返恢复;
 * 重启后 epoch=持久值+1(旧代次失效=boot_id 联合判定等价);
 * 槽损坏回退旧代; RECOVERY_REQUIRED 停写; 激活状态恢复。
 * 时间/随机全部注入, SD 用内存 io(协议与介质无关, 板级 T08/T20 另验)。
 */
#include <stdio.h>
#include <string.h>

#include "persona.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- 内存双槽 io(同 test_store_slot 口径) ---- */
#define TIO_CAP 8224
typedef struct {
    uint8_t data[2][TIO_CAP];
    size_t len[2];
    int present[2];
} tio_t;
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
static int io_write_full(void *ud, int slot, const void *buf, size_t len)
{
    (void)ud;
    if (len > TIO_CAP) return 1;
    memcpy(s_tio.data[slot], buf, len);
    s_tio.len[slot] = len;
    s_tio.present[slot] = 1;
    return 0;
}
static int io_write_at(void *ud, int slot, size_t off, const void *buf,
                       size_t len)
{
    (void)ud;
    if (!s_tio.present[slot] || off + len > s_tio.len[slot]) return 1;
    memcpy(s_tio.data[slot] + off, buf, len);
    return 0;
}
static const store_io_t s_io = { io_read, io_write_full, io_write_at, NULL };

static uint64_t rng_seq;
static uint64_t test_rng(void) { return ++rng_seq * 0x9E3779B97F4A7C15ull; }

static uint8_t s_scratch[8224];

static void strlcpy_(char *d, const char *s, size_t cap)
{
    size_t n = strlen(s);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
}

static void make_two(persona_svc_t *s, uint32_t *a, uint32_t *b_id)
{
    CHECK(persona_init(s, test_rng) == PSN_RES_OK, "init");
    CHECK(persona_store_bind(s, &s_io, s_scratch, sizeof(s_scratch)) ==
          PSN_RES_OK, "bind");
    persona_base_t f;
    memset(&f, 0, sizeof(f));
    strlcpy_(f.display_name, "甲", sizeof(f.display_name));
    strlcpy_(f.default_voice, "longanrou_v3", sizeof(f.default_voice));
    CHECK(persona_create_draft(s, "甲", a) == PSN_RES_OK, "create A");
    CHECK(persona_edit_draft(s, *a, &f) == PSN_RES_OK, "edit A");
    CHECK(persona_confirm(s, *a) == PSN_RES_OK, "confirm A");
    memset(&f, 0, sizeof(f));
    strlcpy_(f.display_name, "乙", sizeof(f.display_name));
    CHECK(persona_create_draft(s, "乙", b_id) == PSN_RES_OK, "create B");
    CHECK(persona_edit_draft(s, *b_id, &f) == PSN_RES_OK, "edit B");
    CHECK(persona_confirm(s, *b_id) == PSN_RES_OK, "confirm B");
    CHECK(persona_activate_begin(s, *a, 0) == PSN_RES_OK, "act A");
    CHECK(persona_activate_commit(s) == PSN_RES_OK, "A on");
    CHECK(persona_consent_set(s, *a, true, false) == PSN_RES_OK, "A memory on");
}
/* 往返: 保存→全新 svc 加载 → 角色/consent/active 恢复; epoch 单调 */
static void scenario_roundtrip(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
    persona_svc_t s;
    uint32_t a = 0, b_id = 0;
    make_two(&s, &a, &b_id);
    CHECK(persona_activate_begin(&s, b_id, 0) == PSN_RES_OK, "begin B");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "B on");
    CHECK(persona_consent_set(&s, b_id, true, true) == PSN_RES_OK,
          "B full consent");
    uint32_t ep_before = persona_epoch(&s);
    CHECK(persona_save(&s) == PSN_RES_OK, "save");

    /* "重启": 全新 svc(内存态清零) + 同一介质 */
    persona_svc_t s2;
    CHECK(persona_init(&s2, test_rng) == PSN_RES_OK, "reinit");
    CHECK(persona_store_bind(&s2, &s_io, s_scratch, sizeof(s_scratch)) ==
          PSN_RES_OK, "rebind");
    psn_res_t lr = persona_load(&s2);
    printf("  [dbg] bind status=%d load=%d n=%d active=%d epoch=%u\n",
           s2.store_ctx.status, lr, s2.n, s2.active, persona_epoch(&s2));
    CHECK(lr == PSN_RES_OK, "load");
    CHECK(s2.n == 2, "2 personas restored");
    CHECK(persona_active_id(&s2) == b_id, "active B restored");
    CHECK(persona_epoch(&s2) == ep_before + 1,
          "epoch = saved+1 (旧代次全部失效)");
    CHECK(persona_epoch_current(&s2, ep_before) == false,
          "pre-reboot epoch dead");
    const persona_base_t *b = persona_get(&s2, a);
    CHECK(b != NULL && b->draft == false && b->version == 1 &&
          strcmp(b->default_voice, "longanrou_v3") == 0, "A fields restored");
    CHECK(persona_consent_memory(&s2, a) &&
          !persona_consent_evolution(&s2, a), "A consent restored");
    CHECK(persona_consent_memory(&s2, b_id) &&
          persona_consent_evolution(&s2, b_id), "B consent restored");

    /* T24 演化往返: 值/暂停/修订历史(查看依据)恢复 */
    CHECK(persona_consent_set(&s, a, true, true) == PSN_RES_OK,
          "A evolution on");          /* 授权默认关, 提案前先开 */
    const char *evs[1] = { "round:ok:1" };
    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.05f, "用户正向反馈",
                              evs, 1, "evo-1") == PSN_RES_OK, "evo propose");
    CHECK(persona_evo_pause(&s, a, true) == PSN_RES_OK, "pause A");
    CHECK(persona_save(&s) == PSN_RES_OK, "save with evo");
    persona_svc_t s3;
    persona_init(&s3, test_rng);
    persona_store_bind(&s3, &s_io, s_scratch, sizeof(s_scratch));
    CHECK(persona_load(&s3) == PSN_RES_OK, "load evo");
    float v = persona_evo_value(&s3, a, PSN_DIM_SPEECH);
    CHECK(v > 0.54f && v < 0.56f, "evo value restored");
    CHECK(persona_evo_paused(&s3, a), "paused restored");
    CHECK(persona_evo_revision_count(&s3, a) == 1, "revision restored");
    const psn_evo_rev_t *rev = persona_evo_revision(&s3, a, 0);
    CHECK(rev != NULL && rev->revision == 1 &&
          strcmp(rev->evidence[0], "round:ok:1") == 0,
          "revision evidence viewable");

    /* 恢复后可继续切换并再存 */
    CHECK(persona_activate_begin(&s2, a, 0) == PSN_RES_OK, "begin A");
    CHECK(persona_activate_commit(&s2) == PSN_RES_OK, "A on again");
    CHECK(persona_save(&s2) == PSN_RES_OK, "save again");
    CHECK(persona_active_id(&s2) == a, "A active");
}

/* 槽损坏 → 回退旧代(最新有效提交) */
static void scenario_corrupt_fallback(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
    persona_svc_t s;
    uint32_t a = 0, b_id = 0;
    make_two(&s, &a, &b_id);
    CHECK(persona_save(&s) == PSN_RES_OK, "save gen1");
    CHECK(persona_activate_begin(&s, b_id, 0) == PSN_RES_OK, "begin B");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "B on");
    CHECK(persona_save(&s) == PSN_RES_OK, "save gen2(B)");

    s_tio.data[STORE_SLOT_B][40] ^= 0xFF;     /* 破坏新代(payload 区) */
    persona_svc_t s2;
    persona_init(&s2, test_rng);
    persona_store_bind(&s2, &s_io, s_scratch, sizeof(s_scratch));
    CHECK(persona_load(&s2) == PSN_RES_OK, "load falls back");
    CHECK(persona_active_id(&s2) == a, "gen1(A) restored");
    CHECK(persona_epoch(&s2) > 2, "epoch still monotonic past gen2");
}

/* 无有效提交代 → RECOVERY_REQUIRED → load 报错且不写 */
static void scenario_recovery_required(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
    persona_svc_t s;
    uint32_t a = 0, b_id = 0;
    make_two(&s, &a, &b_id);
    CHECK(persona_save(&s) == PSN_RES_OK, "save");
    /* 两个槽都破坏成无有效提交代 */
    for (int slot = 0; slot < 2; slot++) {
        if (s_tio.present[slot]) s_tio.data[slot][12] ^= 0xFF;  /* 头部区 */
    }
    persona_svc_t s2;
    persona_init(&s2, test_rng);
    persona_store_bind(&s2, &s_io, s_scratch, sizeof(s_scratch));
    CHECK(persona_load(&s2) == PSN_RES_IO_ERROR, "recovery required");
    CHECK(persona_save(&s2) == PSN_RES_IO_ERROR,
          "write refused after recovery state");
}

/* 空库(全新介质) → load OK 零角色 */
static void scenario_empty(void)
{
    memset(&s_tio, 0, sizeof(s_tio));
    persona_svc_t s;
    persona_init(&s, test_rng);
    persona_store_bind(&s, &s_io, s_scratch, sizeof(s_scratch));
    CHECK(persona_load(&s) == PSN_RES_OK, "empty load ok");
    CHECK(s.n == 0 && persona_active(&s) == NULL, "zero personas");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "roundtrip", scenario_roundtrip },
        { "corrupt",   scenario_corrupt_fallback },
        { "recovery",  scenario_recovery_required },
        { "empty",     scenario_empty },
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
        printf("test_persona_store: ALL PASS\n");
        return 0;
    }
    printf("test_persona_store: %d FAILURE(S)\n", s_fail);
    return 1;
}
