/*
 * test_cube_act.c —— 统一动作骨架回归(宿主, 自带 main; docs/18 T09/T10)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -Wall -Wextra -Wno-unused-parameter \
 *       -I firmware/components/cube_act/include \
 *       -o /tmp/test_cube_act \
 *       firmware/components/cube_act/test/test_cube_act.c \
 *       firmware/components/cube_act/src/cube_act_core.c \
 *   && /tmp/test_cube_act
 *
 * 覆盖(docs/17 §5): 能力注册无需改核心 / 未知与越权拒绝 / 状态机迁移
 * 合法性 / 到期回收 / 队列有界 / 自主开关与用户停止锁存 / 取消各状态
 * / 幂等重放 / 敏感动作确认。时间全部注入, 无 OS 依赖。
 */
#include <stdio.h>
#include <string.h>

#include "cube_act.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- 测试执行器: 计数 + 可编程行为 ---- */
typedef struct {
    int exec_calls, cancel_calls, started, completed, cancelled, interrupted;
    bool exec_reject;          /* execute 直接返回非 OK */
    bool report_completed_first;/* 违规: 不 STARTED 直接 COMPLETED */
    bool hold_started;         /* 只 report STARTED 即返回(模拟在途) */
} exec_stats_t;
static exec_stats_t s_ex;

static cact_res_t ex_validate_ok(const cact_desc_t *d, const uint8_t *p,
                                 size_t n)
{
    (void)d; (void)p; (void)n;
    return CACT_RES_OK;
}

static cact_res_t ex_execute(const cact_desc_t *d, uint32_t id,
                             const uint8_t *p, size_t n)
{
    extern cact_ctx_t *test_ctx(void);
    (void)p; (void)n;
    s_ex.exec_calls++;
    if (s_ex.exec_reject) return CACT_RES_NOT_READY;
    if (s_ex.hold_started) {
        (void)cact_report(test_ctx(), id, CACT_ST_STARTED, NULL);
        s_ex.started++;
        return CACT_RES_OK;                   /* 在途: 不完成 */
    }
    if (s_ex.report_completed_first) {
        (void)cact_report(test_ctx(), id, CACT_ST_COMPLETED, "cheat");
        return CACT_RES_OK;
    }
    (void)cact_report(test_ctx(), id, CACT_ST_STARTED, NULL);
    s_ex.started++;
    (void)cact_report(test_ctx(), id, CACT_ST_COMPLETED, NULL);
    s_ex.completed++;
    (void)d;
    return CACT_RES_OK;
}

static void ex_cancel(const cact_desc_t *d, uint32_t id)
{
    (void)d; (void)id;
    s_ex.cancel_calls++;
}

static const char *s_cap_names[CACT_QUEUE_MAX + 1] = {
    "cap.00", "cap.01", "cap.02", "cap.03", "cap.04",
    "cap.05", "cap.06", "cap.07", "cap.08",
};

/* 各场景自己接线 cact_report 所需 ctx */
static cact_ctx_t *g_ctx;
cact_ctx_t *test_ctx(void) { return g_ctx; }

static void desc_init(cact_desc_t *d, const char *id, int group,
                      cact_risk_t risk)
{
    memset(d, 0, sizeof(*d));
    d->cap_id = id;
    d->group_id = group;
    d->category = CACT_CAT_AUDIO;
    d->risk = risk;
    d->supports_cancel = true;
    d->validate = ex_validate_ok;
    d->execute = ex_execute;
    d->cancel = ex_cancel;
}

static void req_init(cact_req_t *r, cact_origin_t o, const char *cap)
{
    memset(r, 0, sizeof(*r));
    r->origin = o;
    snprintf(r->capability, sizeof(r->capability), "%s", cap);
}

static void stats_reset(void)
{
    memset(&s_ex, 0, sizeof(s_ex));
    g_ctx = NULL;
}

/* ---- 场景 ---- */

/* T09: 注册新能力不需要改仲裁核心; 重名/缺端口拒绝 */
static void scenario_register(void)
{
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a, b;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    desc_init(&b, "face.show", 0, CACT_RISK_LOW);
    CHECK(cact_register(&c, &a) == CACT_RES_OK, "register a");
    CHECK(cact_register(&c, &b) == CACT_RES_OK, "register b");
    CHECK(cact_register(&c, &a) == CACT_RES_INVALID, "duplicate rejected");
    cact_desc_t bad = a;
    bad.execute = NULL;
    CHECK(cact_register(&c, &bad) == CACT_RES_INVALID, "no execute rejected");
}

static void scenario_unknown_and_forbidden(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    CHECK(cact_register(&c, &a) == CACT_RES_OK, "register");

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "no.such.cap");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_UNSUPPORTED,
          "unknown capability");

    cact_desc_t f;
    desc_init(&f, "sys.danger", 0, CACT_RISK_FORBIDDEN);
    CHECK(cact_register(&c, &f) == CACT_RES_OK, "register forbidden cap");
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "sys.danger");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN,
          "forbidden risk rejected");
    CHECK(s_ex.exec_calls == 0, "nothing executed");
}

/* T10: 生命周期与回执; 入队 != 开始 != 完成 */
static void scenario_lifecycle(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    c.now_ms = 100;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit");
    CHECK(id != 0, "id allocated");

    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK && rc.status == CACT_ST_QUEUED,
          "receipt queued");

    cact_pump(&c);
    CHECK(s_ex.started == 1 && s_ex.completed == 1, "executed to completed");
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_COMPLETED, "receipt completed");
    CHECK(rc.seq > 1, "receipt seq grows");
    CHECK(c.q_len == 0, "terminal job leaves queue");

    /* 终态后再 report: 动作已出队 → INVALID, 不产生新回执 */
    uint16_t seq0 = c.rc_seq;
    CHECK(cact_report(&c, id, CACT_ST_COMPLETED, "late") == CACT_RES_INVALID,
          "report after terminal rejected");
    CHECK(c.rc_seq == seq0, "no receipt for illegal report");
}

static void scenario_illegal_transition(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "face.show", 0, CACT_RISK_LOW);
    cact_register(&c, &a);

    s_ex.report_completed_first = true;       /* 违规: 跳过 STARTED */
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "face.show");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit");
    cact_pump(&c);
    /* execute 内的 COMPLETED 被拒; execute 返回 OK 但未到终态 → 停在 ACCEPTED */
    CHECK(c.q_len == 1 && c.q[0].st == CACT_ST_ACCEPTED,
          "skip-started rejected, stays accepted");
    s_ex.report_completed_first = false;
    cact_report(&c, id, CACT_ST_STARTED, NULL);
    CHECK(cact_report(&c, id, CACT_ST_COMPLETED, NULL) == CACT_RES_OK,
          "legal path completes");
    CHECK(c.q_len == 0, "removed after terminal");
}

static void scenario_queue_full(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    static cact_desc_t descs[CACT_QUEUE_MAX + 1];   /* 注册表存指针: 必须稳定 */
    for (int i = 0; i < CACT_QUEUE_MAX + 1; i++) {   /* 9 个不同能力 */
        desc_init(&descs[i], s_cap_names[i], 1, CACT_RISK_LOW);
        cact_register(&c, &descs[i]);
    }
    /* 同能力重复提交会被替换(新语义), 单独验证; 此处只测队列上限 */
    cact_req_t r0;
    req_init(&r0, CACT_ORIGIN_PHYSICAL_USER, "cap.00");
    uint32_t id1 = 0, id2 = 0;
    CHECK(cact_submit(&c, &r0, &id1) == CACT_RES_OK, "cap00 first");
    CHECK(cact_submit(&c, &r0, &id2) == CACT_RES_OK && id2 != id1,
          "same cap resubmits (supersede)");
    CHECK(c.q_len == 1, "old queued job replaced");

    for (int i = 1; i < CACT_QUEUE_MAX; i++) {       /* 填满剩余槽 */
        cact_req_t r;
        req_init(&r, CACT_ORIGIN_PHYSICAL_USER, s_cap_names[i]);
        CHECK(cact_submit(&c, &r, &id1) == CACT_RES_OK, "fill queue");
    }
    cact_req_t r9;
    req_init(&r9, CACT_ORIGIN_PHYSICAL_USER, "cap.08");
    CHECK(cact_submit(&c, &r9, &id1) == CACT_RES_BUSY, "queue full busy");
}

static void scenario_expiry(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    c.now_ms = 1000;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    r.expires_ms = 900;                       /* 已过期 */
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit expiring");
    cact_pump(&c);
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_REJECTED &&
          strcmp(rc.reason, "expired") == 0, "expired rejected");
    CHECK(s_ex.exec_calls == 0, "expired never executed");

    /* 未过期: 不回收 */
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    r.expires_ms = 2000;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit alive");
    c.now_ms = 1500;
    cact_pump(&c);
    CHECK(s_ex.exec_calls == 1, "valid job executed");
}

/* docs/17 §5.5: 用户停止 → 自主拒绝、用户显式不受影响、在途取消 */
static void scenario_stop_latch(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    cact_boot_gate_open(&c, true);            /* 意图配置已恢复(场景前提) */
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    /* 在途 STARTED(hold 模式执行器) */
    s_ex.hold_started = true;
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "autonomous submit");
    c.now_ms = 10;
    cact_pump(&c);
    CHECK(c.q_len == 1 && c.q[0].st == CACT_ST_STARTED, "in flight");

    uint32_t ep0 = cact_stop_epoch(&c);
    CHECK(cact_user_stop(&c, 1, "user_stop") == CACT_RES_OK, "user stop");
    CHECK(cact_stop_epoch(&c) == ep0 + 1, "stop epoch bumped");
    CHECK(cact_group_stopped(&c, 1), "latched");
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_CANCEL_REQUESTED, "in-flight cancelled");
    CHECK(s_ex.cancel_calls == 1, "cancel forwarded to executor");
    cact_report(&c, id, CACT_ST_CANCELLED, NULL);
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_CANCELLED, "executor confirms cancelled");

    /* 锁存期: 自主拒绝, 用户显式放行 */
    cact_req_t ra;
    req_init(&ra, CACT_ORIGIN_AUTONOMOUS_LLM, "music.play");
    CHECK(cact_submit(&c, &ra, &id) == CACT_RES_FORBIDDEN,
          "autonomous blocked while latched");
    cact_req_t ru;
    req_init(&ru, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    CHECK(cact_submit(&c, &ru, &id) == CACT_RES_OK,
          "explicit user not blocked");

    /* 恢复: 只解除锁存, 不恢复播放(重播由新候选决定) */
    CHECK(cact_user_resume(&c, 1) == CACT_RES_OK, "resume");
    CHECK(!cact_group_stopped(&c, 1), "unlatched");
    CHECK(cact_submit(&c, &ra, &id) == CACT_RES_OK, "autonomous restored");
}

static void scenario_autonomous_switch(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    cact_boot_gate_open(&c, true);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "queued while on");

    cact_set_autonomous(&c, false);
    cact_req_t r2;
    req_init(&r2, CACT_ORIGIN_AUTONOMOUS_LLM, "music.play");
    CHECK(cact_submit(&c, &r2, &id) == CACT_RES_FORBIDDEN,
          "new autonomous rejected");
    cact_pump(&c);                             /* 队列中旧自主 → 复检拒绝 */
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_REJECTED, "queued autonomous re-checked");
    CHECK(c.q_len == 0, "queue drained");

    /* 用户显式命令不受开关影响 */
    cact_req_t ru;
    req_init(&ru, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    CHECK(cact_submit(&c, &ru, &id) == CACT_RES_OK, "user ok while off");
}

static void scenario_cancel_states(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    /* QUEUED → 取消直接终态, 不调执行器 */
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    uint32_t id1 = 0, id2 = 0;
    CHECK(cact_submit(&c, &r, &id1) == CACT_RES_OK, "submit1");
    CHECK(cact_cancel(&c, id1, "user") == CACT_RES_OK, "cancel queued");
    CHECK(s_ex.exec_calls == 0, "never executed");
    CHECK(cact_cancel(&c, id1, "again") == CACT_RES_STALE,
          "terminal cancel stale");

    /* STARTED → CANCEL_REQUESTED, 执行器回报 INTERRUPTED */
    CHECK(cact_submit(&c, &r, &id2) == CACT_RES_OK, "submit2");
    cact_pump(&c);
    /* ex_execute 同步跑完 → 已终态出队; 重新构造: 用 exec_reject? */
    /* 同步执行器已在 pump 中完成; 此处验证对已终态 cancel → STALE */
    CHECK(cact_cancel(&c, id2, "late") == CACT_RES_STALE,
          "completed cancel stale");
}

/* 幂等: 同键重放返回原 id(STALE), 不产生第二次执行 */
static void scenario_idem(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    r.idem_key = 42;
    uint32_t id1 = 0, id2 = 0;
    CHECK(cact_submit(&c, &r, &id1) == CACT_RES_OK, "first");
    cact_pump(&c);
    CHECK(s_ex.exec_calls == 1, "executed once");
    CHECK(cact_submit(&c, &r, &id2) == CACT_RES_STALE, "replay stale");
    CHECK(id2 == id1, "same id returned");
    CHECK(s_ex.exec_calls == 1, "no second execution");

    /* 不同键 → 新动作 */
    r.idem_key = 43;
    CHECK(cact_submit(&c, &r, &id2) == CACT_RES_OK && id2 != id1,
          "different key new action");
    (void)cact_cancel(&c, id2, "cleanup");
}

/* 敏感动作: 未确认挂起, 确认后执行; 确认期策略变化 → 拒绝 */
static void scenario_sensitive(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);            /* 场景前提: 意图已恢复 */
    cact_desc_t s;
    desc_init(&s, "mem.delete", 2, CACT_RISK_SENSITIVE);
    s.category = CACT_CAT_GENERIC;
    cact_register(&c, &s);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PAIRED_APP, "mem.delete");
    r.confirmed = false;
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit unconfirmed");
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_AWAITING_CONFIRM, "awaiting confirm");
    cact_pump(&c);
    CHECK(s_ex.exec_calls == 0, "not executed while awaiting");

    CHECK(cact_confirm(&c, id) == CACT_RES_OK, "confirm");
    cact_pump(&c);
    CHECK(s_ex.exec_calls == 1, "executed after confirm");

    /* 确认期间资源组被停止 → 自主来源确认被复检拒绝 */
    r.origin = CACT_ORIGIN_AUTONOMOUS_LLM;    /* 停止锁存仅约束自主来源 */
    r.confirmed = false;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit again");
    cact_user_stop(&c, 2, "stop");            /* 等待确认期间锁存生效 */
    CHECK(cact_confirm(&c, id) == CACT_RES_FORBIDDEN,
          "confirm re-checks policy");
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_REJECTED, "rejected receipt");
}


/* ---- T12: 勿扰/分类/启动闸(docs/16 FR-14/FR-21, docs/17 §5.5) ---- */

/* 勿扰默认关: 时间不可信也不得制造隐藏禁音(AC-17) */
static void scenario_quiet_default_off(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);
    cact_quiet_clock(&c, 500, false);         /* 时间不可信; 勿扰未配置 */
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK,
          "quiet off: unknown time must not silence");
}

/* 勿扰启用: 时间不可信→保守抑制新自主; 用户显式不受影响 */
static void scenario_quiet_untrusted(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);
    cact_quiet_config(&c, true, 23 * 60, 8 * 60);
    cact_quiet_clock(&c, 500, false);         /* 未知时间 */
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN,
          "quiet+untrusted suppresses autonomous");
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK,
          "explicit user unaffected");
}

/* 勿扰可信时间窗: 跨夜窗口边界 */
static void scenario_quiet_window(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);
    cact_quiet_config(&c, true, 23 * 60, 8 * 60);   /* 23:00-08:00 跨夜 */
    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;

    cact_quiet_clock(&c, 23 * 60 + 30, true);       /* 23:30 窗内 */
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN, "in window");
    cact_quiet_clock(&c, 8 * 60 + 30, true);        /* 08:30 窗外 */
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "outside window");
    cact_quiet_clock(&c, 5 * 60, true);             /* 05:00 窗内(跨夜段) */
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN, "wrap segment");
    cact_quiet_clock(&c, 22 * 60 + 59, true);       /* 22:59 窗外 */
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "just before start");

    /* 白天勿扰(非跨夜窗) */
    cact_quiet_config(&c, true, 8 * 60, 23 * 60);
    cact_quiet_clock(&c, 12 * 60, true);
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN, "daytime window");
    cact_quiet_clock(&c, 7 * 60 + 59, true);
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "before daytime start");
}

/* 勿扰作用域: 組外自主不受影响; 勿扰不停在途/用户音乐 */
static void scenario_quiet_scope_and_user_music(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);
    cact_desc_t music, face;
    desc_init(&music, "music.play", 1, CACT_RISK_LOW);   /* 组1: 音频 */
    desc_init(&face, "face.show", 0, CACT_RISK_LOW);     /* 组0: 无组 */
    cact_register(&c, &music);
    cact_register(&c, &face);
    cact_quiet_config(&c, true, 0, 24 * 60);             /* 全天窗 */
    cact_quiet_clock(&c, 12 * 60, true);

    /* 用户先开启播放; 勿扰开启后: 在途用户动作不受影响 */
    cact_req_t ru;
    req_init(&ru, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    uint32_t idu = 0;
    CHECK(cact_submit(&c, &ru, &idu) == CACT_RES_OK, "user music queued");
    cact_pump(&c);
    CHECK(s_ex.completed == 1, "user music completed during quiet");

    /* 作用域: 组0(face)不在勿扰组(组0 无组豁免); 组1 自主被抑制 */
    cact_quiet_scope(&c, 1, true);
    cact_req_t rm;
    req_init(&rm, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &rm, &id) == CACT_RES_FORBIDDEN,
          "scoped group suppressed");
    cact_quiet_scope(&c, 1, false);                      /* 移出作用域 */
    CHECK(cact_submit(&c, &rm, &id) == CACT_RES_OK,
          "out-of-scope group allowed");
}

/* 分类控制: 关音频类不影响语音类; 用户显式不受分类关影响 */
static void scenario_category(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_boot_gate_open(&c, true);
    cact_desc_t audio, speech;
    desc_init(&audio, "music.play", 1, CACT_RISK_LOW);
    audio.category = CACT_CAT_AUDIO;
    desc_init(&speech, "speech.say", 2, CACT_RISK_LOW);
    speech.category = CACT_CAT_SPEECH;
    cact_register(&c, &audio);
    cact_register(&c, &speech);

    cact_set_category(&c, CACT_CAT_AUDIO, false);
    CHECK(!cact_category_enabled(&c, CACT_CAT_AUDIO), "audio off");
    CHECK(cact_category_enabled(&c, CACT_CAT_SPEECH), "speech still on");

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN,
          "autonomous audio blocked by category");
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "speech.say");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK,
          "other category unaffected");
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK,
          "explicit user unaffected by category off");
    cact_set_category(&c, CACT_CAT_AUDIO, true);
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "re-enabled");
}

/* 启动安全闸: 初始化即关; 闸开前自主拒绝、用户可用; 开启后恢复 */
static void scenario_boot_gate(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    CHECK(!cact_boot_gate_is_open(&c), "gate closed at init");
    CHECK(c.autonomous_enabled == true, "configured default still on");
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_FORBIDDEN,
          "autonomous blocked before gate");
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK,
          "explicit user ok before gate");

    cact_boot_gate_open(&c, true);            /* 意图配置恢复验证后 */
    CHECK(cact_boot_gate_is_open(&c), "gate open");
    req_init(&r, CACT_ORIGIN_AUTONOMOUS_LOCAL, "music.play");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "autonomous after gate");
}

/* 执行器拒绝受理 → 同步 failed */
static void scenario_exec_reject(void)
{
    stats_reset();
    cact_ctx_t c;
    cact_init(&c);
    g_ctx = &c;
    cact_desc_t a;
    desc_init(&a, "music.play", 1, CACT_RISK_LOW);
    cact_register(&c, &a);
    s_ex.exec_reject = true;

    cact_req_t r;
    req_init(&r, CACT_ORIGIN_PHYSICAL_USER, "music.play");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit");
    cact_pump(&c);
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_FAILED &&
          strcmp(rc.reason, "exec_reject") == 0, "exec_reject failed");
    CHECK(c.q_len == 0, "failed leaves queue");
    s_ex.exec_reject = false;
}

/* ---- T38: 设置持久化(存储注入; 修改即持久+重启恢复+独立性+损毁默认) ---- */
static uint8_t t38_store[16];
static size_t t38_len;
static bool t38_has;
static int t38_save(const void *buf, size_t len)
{
    if (len > sizeof(t38_store)) return -1;
    memcpy(t38_store, buf, len);
    t38_len = len;
    t38_has = true;
    return 0;
}
static int t38_load(void *buf, size_t cap, size_t *len)
{
    if (!t38_has) return 1;
    if (cap < t38_len) return -1;
    memcpy(buf, t38_store, t38_len);
    *len = t38_len;
    return 0;
}
static const cact_store_t T38_STORE = { t38_save, t38_load };

static void scenario_t38_persist(void)
{
    cact_ctx_t *c = cact_default();
    cact_attach_store(&T38_STORE);
    t38_has = false;

    /* 修改 autonomous+quiet → 持久 */
    cact_set_autonomous(c, false);
    CHECK(t38_has, "autonomous persisted");
    cact_quiet_config(c, true, 1380, 420);
    cact_quiet_scope(c, 1, false);
    cact_quiet_scope(c, 2, true);       /* 显式前置(先前场景可能改过) */
    CHECK(t38_len == 16, "blob 16B");

    /* "重启": 新上下文语义 —— 复制默认 ctx 再 restore */
    cact_ctx_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    cact_init(&fresh);                       /* 默认值 */
    cact_restore(&fresh);
    CHECK(fresh.autonomous_enabled == false, "restore autonomous=off");
    CHECK(fresh.quiet.enabled == true, "restore quiet=on");
    CHECK(fresh.quiet.start_min == 1380 && fresh.quiet.end_min == 420,
          "restore quiet window");
    CHECK(fresh.quiet.scope[1] == false, "restore scope group1 off");

    /* 独立性: autonomous 不影响 quiet 恢复值 */
    CHECK(fresh.quiet.scope[2] == true, "scope others default");

    /* 损毁 blob → 默认(恢复出厂) */
    t38_store[0] ^= 0xFF;
    memset(&fresh, 0, sizeof(fresh));
    cact_init(&fresh);
    cact_restore(&fresh);
    CHECK(fresh.autonomous_enabled == true, "corrupt → default on");
    CHECK(fresh.quiet.enabled == false, "corrupt → default quiet off");

    /* 首次开机(无数据) → 默认 */
    t38_has = false;
    memset(&fresh, 0, sizeof(fresh));
    cact_init(&fresh);
    cact_restore(&fresh);
    CHECK(fresh.autonomous_enabled == true, "first boot default on");
    /* 清理: 摘除 store, 恢复运行时 ctx 状态 */
    cact_attach_store(NULL);
    cact_set_autonomous(c, true);
    cact_quiet_config(c, false, 0, 0);
    cact_quiet_scope(c, 1, true);
}


int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "register",      scenario_register },
        { "unknown_risk",  scenario_unknown_and_forbidden },
        { "lifecycle",     scenario_lifecycle },
        { "illegal_trans", scenario_illegal_transition },
        { "queue_full",    scenario_queue_full },
        { "expiry",        scenario_expiry },
        { "stop_latch",    scenario_stop_latch },
        { "auto_switch",   scenario_autonomous_switch },
        { "cancel_states", scenario_cancel_states },
        { "idem",          scenario_idem },
        { "sensitive",     scenario_sensitive },
        { "exec_reject",   scenario_exec_reject },
        { "quiet_default", scenario_quiet_default_off },
        { "quiet_untrust", scenario_quiet_untrusted },
        { "t38_persist",   scenario_t38_persist },
        { "quiet_window",  scenario_quiet_window },
        { "quiet_scope",   scenario_quiet_scope_and_user_music },
        { "category",      scenario_category },
        { "boot_gate",     scenario_boot_gate },
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
        stats_reset();
        int before = s_fail;
        cases[i].fn();
        if (s_fail > before) {
            printf("FAIL scenario %s\n", cases[i].name);
        } else {
            printf("PASS scenario %s\n", cases[i].name);
        }
        g_ctx = NULL;
    }
    if (s_fail == 0) {
        printf("test_cube_act: ALL PASS\n");
        return 0;
    }
    printf("test_cube_act: %d FAILURE(S)\n", s_fail);
    return 1;
}
