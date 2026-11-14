/*
 * test_persona.c —— 人格服务骨架回归(宿主; docs/18 T19)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter -I firmware/components/persona/include \
 *       -o /tmp/test_persona \
 *       firmware/components/persona/test/test_persona.c \
 *       firmware/components/persona/src/persona_core.c \
 *   && /tmp/test_persona
 *
 * 验收(docs/18 T19): A→B→A 配置/缓存/行动/晚到结果不串(代次复核);
 * 改名 ID 不变; 草稿未确认不生效; 锁定字段不可覆盖; 屏障三段。
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

/* 确定性随机源(测试可复现) */
static uint64_t rng_seq;
static uint64_t test_rng(void) { return ++rng_seq * 0x9E3779B97F4A7C15ull; }

static void strlcpy_(char *d, const char *s, size_t cap)
{
    size_t n = strlen(s);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
}

static void fields_of(persona_base_t *f, const char *name, const char *desc,
                      const char *voice)
{
    memset(f, 0, sizeof(*f));
    if (name) strlcpy_(f->display_name, name, sizeof(f->display_name));
    if (desc) strlcpy_(f->description, desc, sizeof(f->description));
    if (voice) strlcpy_(f->default_voice, voice, sizeof(f->default_voice));
}

/* 建角色并确认生效的快捷路径 */
static uint32_t make_persona(persona_svc_t *s, const char *name)
{
    uint32_t id = 0;
    persona_base_t f;
    fields_of(&f, name, NULL, NULL);
    CHECK(persona_create_draft(s, name, &id) == PSN_RES_OK, "create draft");
    CHECK(persona_edit_draft(s, id, &f) == PSN_RES_OK, "edit draft");
    CHECK(persona_confirm(s, id) == PSN_RES_OK, "confirm");
    return id;
}

/* 场景 1: 创建→确认→激活; 草稿未确认不生效 */
static void scenario_lifecycle(void)
{
    persona_svc_t s;
    CHECK(persona_init(&s, test_rng) == PSN_RES_OK, "init");
    CHECK(persona_active(&s) == NULL && persona_epoch(&s) == 1, "empty state");

    uint32_t id = 0;
    CHECK(persona_create_draft(&s, "小灵", &id) == PSN_RES_OK, "draft");
    CHECK(id != 0, "id allocated");
    const persona_base_t *b = persona_get(&s, id);
    CHECK(b != NULL && b->draft && b->version == 0, "draft not effective");

    /* 草稿不可激活/不可弃保护在后面场景; 此处先确认 */
    CHECK(persona_activate_begin(&s, id, 0) == PSN_RES_FORBIDDEN,
          "draft cannot activate");
    CHECK(persona_confirm(&s, id) == PSN_RES_OK, "confirm");
    CHECK(b->draft == false && b->version == 1, "version 1 effective");

    CHECK(persona_activate_begin(&s, id, 0) == PSN_RES_OK, "begin");
    CHECK(persona_switching(&s), "barrier up");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "commit");
    CHECK(!persona_switching(&s), "barrier down");
    CHECK(persona_active_id(&s) == id && persona_epoch(&s) == 2, "active");

    /* 再确认已生效版: INVALID(编辑必须走草稿) */
    CHECK(persona_confirm(&s, id) == PSN_RES_INVALID, "no reconfirm");
}

/* 场景 2: 改名保 ID, 版本递增 */
static void scenario_rename_keeps_id(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t id = make_persona(&s, "旧名字");

    persona_base_t f;
    fields_of(&f, "新名字", "新的描述", NULL);
    CHECK(persona_edit_draft(&s, id, &f) == PSN_RES_OK, "edit as draft");
    const persona_base_t *b = persona_get(&s, id);
    CHECK(b->draft && b->persona_id == id, "same id, back to draft");
    CHECK(persona_confirm(&s, id) == PSN_RES_OK, "confirm rename");
    CHECK(b->version == 2 && strcmp(b->display_name, "新名字") == 0 &&
          b->persona_id == id, "id stable, version 2");
}

/* 场景 3: A→B→A 代次复核 —— 晚到结果失效(docs/18 T19 核心) */
static void scenario_abab(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t a = make_persona(&s, "甲");
    uint32_t b_id = make_persona(&s, "乙");

    CHECK(persona_activate_begin(&s, a, 0) == PSN_RES_OK, "begin A");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "commit A");
    uint32_t e1 = persona_epoch(&s);                    /* 假设 A@e1 发出请求 */

    CHECK(persona_activate_begin(&s, b_id, e1) == PSN_RES_OK, "begin B");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "commit B");
    uint32_t e2 = persona_epoch(&s);

    /* 晚到结果: A@e1 在 B 代次复核失败 */
    CHECK(!persona_epoch_current(&s, e1), "stale A result rejected");
    CHECK(persona_epoch_current(&s, e2), "current B ok");

    /* 切回 A: 用旧 expected → STALE(防并发误切) */
    CHECK(persona_activate_begin(&s, a, e1) == PSN_RES_STALE, "stale begin");
    CHECK(persona_activate_begin(&s, a, e2) == PSN_RES_OK, "begin A again");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "commit A again");
    CHECK(persona_active_id(&s) == a, "back to A");
    /* A 的"新请求"携带新代次; B@e2 晚到失效 */
    CHECK(persona_epoch_current(&s, e2) == false, "B late result dead");
    CHECK(persona_epoch_current(&s, persona_epoch(&s)), "A current ok");
}

/* 场景 4: 屏障 abort —— 状态不变 */
static void scenario_barrier_abort(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t a = make_persona(&s, "甲");
    uint32_t b_id = make_persona(&s, "乙");
    CHECK(persona_activate_begin(&s, a, 0) == PSN_RES_OK, "act A");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "A on");
    uint32_t e = persona_epoch(&s);

    CHECK(persona_activate_begin(&s, b_id, 0) == PSN_RES_OK, "begin B");
    CHECK(persona_switching(&s), "in barrier");
    CHECK(persona_activate_begin(&s, a, 0) == PSN_RES_BUSY,
          "no nested switch");
    CHECK(persona_activate_abort(&s) == PSN_RES_OK, "abort");
    CHECK(!persona_switching(&s), "barrier down");
    CHECK(persona_active_id(&s) == a && persona_epoch(&s) == e,
          "nothing changed");
}

/* 场景 5: 锁定字段 —— 用户设置后编辑/确认均不可覆盖 */
static void scenario_locked_fields(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t id = make_persona(&s, "小灵");
    CHECK(persona_set_locked(&s, id, "default_voice", true) == PSN_RES_OK,
          "lock voice");

    persona_base_t f;
    fields_of(&f, NULL, NULL, "longanrou_v3");      /* 改锁定字段 */
    CHECK(persona_edit_draft(&s, id, &f) == PSN_RES_FORBIDDEN,
          "locked field edit rejected");
    fields_of(&f, NULL, "改描述没问题", NULL);        /* 非锁定字段 */
    CHECK(persona_edit_draft(&s, id, &f) == PSN_RES_OK,
          "non-locked edit ok");
    CHECK(persona_field_locked(persona_get(&s, id), "default_voice"),
          "lock persists");
    CHECK(persona_set_locked(&s, id, "default_voice", false) == PSN_RES_OK,
          "unlock by user");
    fields_of(&f, NULL, NULL, "longanrou_v3");
    CHECK(persona_edit_draft(&s, id, &f) == PSN_RES_OK, "editable after unlock");
}

/* 场景 6: 授权默认双关 + consent_epoch 递增 */
static void scenario_consent(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t id = make_persona(&s, "小灵");
    CHECK(!persona_consent_memory(&s, id), "memory default off");
    CHECK(!persona_consent_evolution(&s, id), "evolution default off");
    CHECK(persona_consent_epoch(&s, id) == 0, "consent epoch 0");

    CHECK(persona_consent_set(&s, id, true, false) == PSN_RES_OK, "memory on");
    CHECK(persona_consent_memory(&s, id) &&
          !persona_consent_evolution(&s, id), "memory only");
    uint32_t e1 = persona_consent_epoch(&s, id);
    CHECK(persona_consent_set(&s, id, true, true) == PSN_RES_OK, "evo on");
    CHECK(persona_consent_epoch(&s, id) == e1 + 1, "epoch bumps");
}

/* 场景 7: 容量上限 / 激活角色不可弃 / 生效版不可弃 / id 非零唯一 */
static void scenario_guards(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    /* 纯草稿可弃(正向路径, 在满员前验证) */
    uint32_t d = 0;
    CHECK(persona_create_draft(&s, "纯草稿", &d) == PSN_RES_OK, "plain draft");
    CHECK(persona_discard(&s, d) == PSN_RES_OK, "draft discardable");

    uint32_t ids[PSN_LIST_MAX];
    char name[24];
    for (int i = 0; i < PSN_LIST_MAX; i++) {
        snprintf(name, sizeof(name), "角色%d", i);
        ids[i] = make_persona(&s, name);
        CHECK(ids[i] != 0, "id nonzero");
        for (int j = 0; j < i; j++) {
            CHECK(ids[i] != ids[j], "ids unique");
        }
    }
    uint32_t extra = 0;
    CHECK(persona_create_draft(&s, "超出", &extra) == PSN_RES_NO_SPACE,
          "capacity limit");

    CHECK(persona_activate_begin(&s, ids[0], 0) == PSN_RES_OK, "act");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "on");

    /* 激活角色先编辑回草稿再弃 → 仍拒绝(激活位不可弃) */
    persona_base_t f;
    fields_of(&f, "改名", NULL, NULL);
    CHECK(persona_edit_draft(&s, ids[0], &f) == PSN_RES_OK, "to draft");
    CHECK(persona_discard(&s, ids[0]) == PSN_RES_FORBIDDEN,
          "active slot not discardable");
    /* 生效版直接弃 → FORBIDDEN */
    CHECK(persona_discard(&s, ids[1]) == PSN_RES_FORBIDDEN,
          "effective not discardable");
}


/* S-evo(T24): 有界演化 —— 限幅/证据门/授权与暂停/回滚/跨角色不串 */
static void scenario_evo(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t a = make_persona(&s, "甲");
    uint32_t b_id = make_persona(&s, "乙");
    CHECK(persona_activate_begin(&s, a, 0) == PSN_RES_OK, "act A");
    CHECK(persona_activate_commit(&s) == PSN_RES_OK, "A on");

    const char *evs[1] = { "round:ok:1" };
    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.03f, "正向反馈",
                              evs, 1, "evo-1") == PSN_RES_FORBIDDEN_LOCKED,
          "evolution off by default");
    CHECK(persona_consent_set(&s, a, false, true) == PSN_RES_OK, "evo on");

    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.03f, "r", NULL, 0,
                              "evo-1") == PSN_RES_INVALID, "no evidence");
    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.5f, "r", evs, 1,
                              "evo-1") == PSN_RES_INVALID, "delta over limit");
    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.0f, "r", evs, 1,
                              "evo-1") == PSN_RES_INVALID, "zero delta");

    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.03f, "正向反馈",
                              evs, 1, "evo-1") == PSN_RES_OK, "propose ok");
    float v = persona_evo_value(&s, a, PSN_DIM_SPEECH);
    CHECK(v > 0.529f && v < 0.531f, "bounded apply");

    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, PSN_EVO_MAX_DELTA,
                              "再正向", evs, 1, "evo-1") == PSN_RES_OK,
          "second propose");
    v = persona_evo_value(&s, a, PSN_DIM_SPEECH);
    CHECK(v > 0.579f && v < 0.581f, "second apply 0.58");

    const psn_evo_rev_t *rev = persona_evo_revision(&s, a, 0);
    CHECK(rev != NULL && rev->revision == 2 &&
          rev->evidence_n == 1 && rev->old_value > 0.529f &&
          rev->new_value > 0.579f && rev->persona_id == a,
          "revision auditable");

    CHECK(persona_evo_pause(&s, a, true) == PSN_RES_OK, "pause");
    CHECK(persona_evo_propose(&s, a, PSN_DIM_SPEECH, 0.01f, "r", evs, 1,
                              "evo-1") == PSN_RES_FORBIDDEN_LOCKED,
          "paused blocks");
    CHECK(persona_evo_pause(&s, a, false) == PSN_RES_OK, "resume");

    CHECK(persona_evo_rollback(&s, a, 1) == PSN_RES_OK, "rollback");
    v = persona_evo_value(&s, a, PSN_DIM_SPEECH);
    CHECK(v > 0.499f && v < 0.501f, "value back to pre-rev1");
    CHECK(persona_evo_revision_count(&s, a) == 3, "rollback audited");
    rev = persona_evo_revision(&s, a, 0);
    CHECK(rev != NULL && strcmp(rev->reason, "rollback") == 0,
          "rollback reason recorded");

    CHECK(persona_evo_value(&s, b_id, PSN_DIM_SPEECH) == 0.5f,
          "B untouched (cross-persona isolation)");
}


/* S-sig(T34): 信号聚合 —— 3 次正向自动提案; 授权关丢弃; 提案后计数清零 */
static void scenario_evo_signal(void)
{
    persona_svc_t s;
    persona_init(&s, test_rng);
    uint32_t a = make_persona(&s, "甲");
    CHECK(persona_consent_set(&s, a, false, true) == PSN_RES_OK, "evo on");
    float v0 = persona_evo_value(&s, a, PSN_DIM_INTEREST);

    const char *refs[3] = { "act:1", "act:2", "act:3" };
    /* 授权关时段(先关) → 信号丢弃 */
    CHECK(persona_consent_set(&s, a, false, false) == PSN_RES_OK, "evo off");
    CHECK(persona_evo_signal(&s, a, PSN_DIM_INTEREST, 1, refs[0]) ==
          PSN_RES_FORBIDDEN_LOCKED, "signal dropped when off");
    CHECK(persona_consent_set(&s, a, false, true) == PSN_RES_OK, "evo on");

    /* 3 次正向 → 第 3 次自动提案(返回 OK=修订已记) */
    CHECK(persona_evo_signal(&s, a, PSN_DIM_INTEREST, 1, "act:1") ==
          PSN_RES_OK, "sig 1");
    CHECK(persona_evo_signal(&s, a, PSN_DIM_INTEREST, 1, "act:2") ==
          PSN_RES_OK, "sig 2");
    psn_res_t r3 = persona_evo_signal(&s, a, PSN_DIM_INTEREST, 1, "act:3");
    CHECK(r3 == PSN_RES_OK, "sig 3 aggregates");
    float v1 = persona_evo_value(&s, a, PSN_DIM_INTEREST);
    CHECK(v1 > v0 && v1 - v0 < 0.041f, "bounded +delta applied");
    const psn_evo_rev_t *rev = persona_evo_revision(&s, a, 0);
    CHECK(rev != NULL && rev->evidence_n >= 1 &&
          strcmp(rev->evidence[0], "act:1") == 0,
          "evidence refs in revision");

    /* 混合负向: 未达阈值不提案 */
    CHECK(persona_evo_signal(&s, a, PSN_DIM_INTEREST, -1, "stop:1") ==
          PSN_RES_OK, "neg 1");
    CHECK(persona_evo_revision_count(&s, a) == 1, "no propose below threshold");
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "lifecycle",   scenario_lifecycle },
        { "rename_id",   scenario_rename_keeps_id },
        { "abab",        scenario_abab },
        { "barrier",     scenario_barrier_abort },
        { "locked",      scenario_locked_fields },
        { "consent",     scenario_consent },
        { "guards",      scenario_guards },
        { "evo",         scenario_evo },
        { "evo_signal",  scenario_evo_signal },
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
        printf("test_persona: ALL PASS\n");
        return 0;
    }
    printf("test_persona: %d FAILURE(S)\n", s_fail);
    return 1;
}
