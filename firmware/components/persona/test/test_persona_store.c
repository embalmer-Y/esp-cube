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
#include "store_slot.h"

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

    /* S2.5b trait 慢变量往返(docs/24 §3.1): 写→存→载→读一致; 缺省中性 */
    float tv[PSN_TRAIT_MAX] = { 0.62f, 0.41f, 0.55f, 0.50f, 0.38f };
    CHECK(persona_trait_write(&s2, a, tv) == PSN_RES_OK, "trait write");
    CHECK(persona_save(&s2) == PSN_RES_OK, "save trait");
    persona_svc_t s4;
    persona_init(&s4, test_rng);
    persona_store_bind(&s4, &s_io, s_scratch, sizeof(s_scratch));
    CHECK(persona_load(&s4) == PSN_RES_OK, "load trait");
    float tr[PSN_TRAIT_MAX];
    CHECK(persona_trait_read(&s4, a, tr), "trait valid after reload");
    for (int k = 0; k < PSN_TRAIT_MAX; k++)
        CHECK(tr[k] > tv[k] - 0.0001f && tr[k] < tv[k] + 0.0001f,
              "trait value restored");
    float tn[PSN_TRAIT_MAX];
    CHECK(!persona_trait_read(&s4, b_id, tn), "无 trait 角色=中性缺省");

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
/* v2 旧注册表兼容读(S2.5b): 冻结的 v2 布局重放 + trait 缺省中性 */
static void scenario_v2_compat(void)
{
    /* v2 记录布局(冻结于 PSN_STORE_VER=2 时代, 与 persona_core 的
     * persona_rec_t 逐字段一致; 变更即破坏兼容=违背本测试目的) */
    #pragma pack(push, 1)
    typedef struct {
        uint32_t revision;
        uint32_t persona_id;
        uint8_t  dim;
        float    old_value, new_value, delta_applied;
        char     reason[32];
        char     evidence[2][16];
        uint8_t  evidence_n;
        char     algo_ver[8];
    } v2_rev_t;
    typedef struct {
        uint32_t id;
        uint16_t version;
        uint8_t  draft;
        char     name[24];
        char     desc[128];
        char     voice[24];
        uint8_t  locked_n;
        char     locked[8][24];
        uint8_t  mem_en, evo_en;
        uint32_t consent_epoch;
        float    evo_value[4];
        uint8_t  evo_paused;
        uint8_t  rev_n;
        v2_rev_t revs[4];
    } v2_rec_t;
    #pragma pack(pop)

    memset(&s_tio, 0, sizeof(s_tio));
    v2_rec_t r;
    memset(&r, 0, sizeof(r));
    r.id = 0xAABBCCDD;
    r.version = 1;
    r.draft = 0;
    strcpy(r.name, "旧角色");
    strcpy(r.voice, "longanrou_v3");
    r.mem_en = 1;
    struct { uint32_t magic; uint16_t ver; uint8_t n; int8_t active;
             uint32_t last_epoch; } h = {
        0x50535231u, 2, 1, 0, 4,
    };
    uint8_t v2buf[1024];   /* v2 rec ~776B + 注册表头 */
    size_t off = 0;
    memcpy(v2buf + off, &h, sizeof(h)); off += sizeof(h);
    memcpy(v2buf + off, &r, sizeof(r)); off += sizeof(r);
    CHECK(off <= sizeof(v2buf), "v2 payload fits");
    /* 经 store_slot 正规提交(槽头+CRC+提交位), 模拟 v2 时代真实落盘 */
    {
        static store_slot_t ctx;
        static uint8_t ctx_scratch[8224 + 24];
        CHECK(store_slot_init(&ctx, &s_io, 8000, ctx_scratch,
                              sizeof(ctx_scratch)) == STORE_STATUS_EMPTY,
              "fresh store");
        uint32_t gen = 0;
        CHECK(store_slot_commit(&ctx, v2buf, off, &gen) ==
              STORE_STATUS_OK, "commit v2 payload");
    }

    persona_svc_t s;
    CHECK(persona_init(&s, test_rng) == PSN_RES_OK, "init");
    CHECK(persona_store_bind(&s, &s_io, s_scratch, sizeof(s_scratch)) ==
          PSN_RES_OK, "bind");
    CHECK(persona_load(&s) == PSN_RES_OK, "v2 load ok");
    CHECK(s.n == 1, "v2 角色不丢");
    const persona_base_t *b = persona_get(&s, 0xAABBCCDD);
    CHECK(b != NULL && !b->draft && b->version == 1 &&
          strcmp(b->display_name, "旧角色") == 0 &&
          strcmp(b->default_voice, "longanrou_v3") == 0,
          "v2 字段恢复");
    CHECK(persona_active_id(&s) == 0xAABBCCDD, "v2 active 恢复");
    float tr[PSN_TRAIT_MAX];
    CHECK(!persona_trait_read(&s, 0xAABBCCDD, tr),
          "v2 无 trait → 中性缺省(soul 层保持 0.5)");
    /* 再存=升格 v3(trait 以后才有值) */
    CHECK(persona_save(&s) == PSN_RES_OK, "re-save upgrades to v3");
}

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
        { "v2compat",  scenario_v2_compat },
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
