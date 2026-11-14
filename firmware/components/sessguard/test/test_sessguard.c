/*
 * test_sessguard.c —— 会话守卫宿主验收(docs/18 T31 H; 批次 26)
 *
 * 编译运行(tools/host_tests.sh 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/sessguard/include \
 *       -o /tmp/test_sessguard \
 *       firmware/components/sessguard/test/test_sessguard.c \
 *       firmware/components/sessguard/src/sessguard.c \
 *   && /tmp/test_sessguard
 *
 * 验收(T31 H 语义逐条):
 * - 有界上下文: 容量滴出最旧, 多轮顺序准确
 * - 多轮语境: append 序 = 读出序
 * - 请求生命周期: begin 串行(第二个 begin=0)/finish 作废/超时失效
 * - 切人格/撤权限: emit_ok 代次 mismatch 自动失效
 * - cancel_all: 未收口请求全作废
 * - 工具回执去重: 首见真/重复假/失效后全拒
 */
#include <stdio.h>
#include <string.h>

#include "sessguard.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

static void scenario_context(void)
{
    sessguard_t *sg;
    CHECK(sg_init(&sg, 4, 64, 4) == 0, "init");
    sg_append(sg, "user", "第一轮问题");
    sg_append(sg, "assistant", "第一轮回答");
    sg_append(sg, "user", "第二轮追问");
    CHECK(sg_count(sg) == 3, "count 3");
    CHECK(strcmp(sg_text_at(sg, 0), "第一轮问题") == 0, "order oldest");
    CHECK(strcmp(sg_text_at(sg, 2), "第二轮追问") == 0, "order newest");

    /* 滴出最旧: 容量 4, 追加至 6 → 保留最后 4 轮 */
    sg_append(sg, "assistant", "第二轮回答");
    sg_append(sg, "user", "第三轮");
    sg_append(sg, "assistant", "第三轮回答");
    CHECK(sg_count(sg) == 4, "capped at 4");
    CHECK(strcmp(sg_text_at(sg, 0), "第二轮追问") == 0, "oldest evicted");
    CHECK(strcmp(sg_text_at(sg, 3), "第三轮回答") == 0, "newest kept");
    CHECK(sg_text_at(sg, 4) == NULL, "out of range NULL");
    /* 超长截断 */
    char big[128];
    memset(big, 'x', sizeof(big));
    big[127] = 0;
    sg_append(sg, "user", big);
    CHECK(strlen(sg_text_at(sg, 3)) == 64, "truncated to cap");
    sg_free(sg);
}

static void scenario_lifecycle(void)
{
    sessguard_t *sg;
    sg_init(&sg, 4, 64, 4);
    sg_epochs_t ep = { .persona_ep = 59, .consent_ep = 3 };
    sg_epochs_t cur = ep;

    /* 串行: 第一个 begin 得 id, 第二个 begin=0 */
    uint32_t r1 = sg_begin(sg, &ep, 1000, 30000);
    CHECK(r1 != 0, "begin r1");
    CHECK(sg_begin(sg, &ep, 1000, 30000) == 0, "second begin rejected");
    CHECK(sg_inflight(sg) == 1, "inflight 1");

    /* emit ok(未超时, 代次同) */
    CHECK(sg_emit_ok(sg, r1, &cur, 2000), "emit ok at t=2000");
    /* 超时 */
    CHECK(!sg_emit_ok(sg, r1, &cur, 31000), "emit rejected after timeout");
    /* 代次变化: 切人格(persona_ep++) → 自动失效 */
    cur.persona_ep = 60;
    CHECK(!sg_emit_ok(sg, r1, &cur, 2000), "persona switch invalidates");
    cur = ep;
    cur.consent_ep = 4;
    CHECK(!sg_emit_ok(sg, r1, &cur, 2000), "consent revoke invalidates");
    cur = ep;
    CHECK(sg_emit_ok(sg, r1, &cur, 2000), "same epochs still ok");

    /* finish 作废 */
    sg_finish(sg, r1);
    CHECK(!sg_emit_ok(sg, r1, &cur, 2000), "emit after finish rejected");
    CHECK(sg_inflight(sg) == 0, "inflight 0 after finish");
    /* 错误 id / NULL epochs */
    CHECK(!sg_emit_ok(sg, 0, &cur, 2000), "id 0 rejected");
    CHECK(!sg_emit_ok(sg, r1, NULL, 2000), "NULL epochs rejected");
    /* cancel_all: 未收口全作废 */
    uint32_t r2 = sg_begin(sg, &ep, 5000, 30000);
    CHECK(r2 != 0, "begin r2");
    sg_cancel_all(sg);
    CHECK(!sg_emit_ok(sg, r2, &ep, 6000), "cancel_all invalidates");
    CHECK(sg_inflight(sg) == 0, "inflight 0 after cancel");
    /* finish 错误 id 不误伤 */
    uint32_t r3 = sg_begin(sg, &ep, 5000, 30000);
    sg_finish(sg, 999);
    CHECK(sg_emit_ok(sg, r3, &ep, 6000), "finish wrong-id no-op");
    sg_free(sg);
}

static void scenario_tools(void)
{
    sessguard_t *sg;
    sg_init(&sg, 4, 64, 4);
    sg_epochs_t ep = { 59, 3 }, cur = ep;
    uint32_t r = sg_begin(sg, &ep, 1000, 30000);
    CHECK(r != 0, "tools begin");
    (void)cur;

    /* 回执去重: 首见真, 重复假 */
    CHECK(sg_tool_first(sg, r, &cur, 1000, "call-1"), "tool first seen");
    CHECK(!sg_tool_first(sg, r, &cur, 1000, "call-1"), "tool duplicate rejected");
    CHECK(sg_tool_first(sg, r, &cur, 1000, "call-2"), "tool second distinct");
    CHECK(!sg_tool_first(sg, r, &cur, 1000, ""), "empty id rejected");
    CHECK(!sg_tool_first(sg, 0, &cur, 1000, "call-3"), "invalid req rejected");

    /* cancel 后: 未执行的 id 全拒(不重复工具) */
    sg_cancel_all(sg);
    CHECK(!sg_tool_first(sg, r, &cur, 1000, "call-4"), "tool after cancel rejected");
    CHECK(!sg_tool_first(sg, r, &cur, 1000, "call-1"), "old id after cancel rejected");

    /* 超时后: 工具拒(超时/切人格/撤权限 → 不重复工具) */
    r = sg_begin(sg, &ep, 1000, 100);
    cur = ep;
    CHECK(sg_tool_first(sg, r, &cur, 1000, "t1"), "first within timeout");
    cur.persona_ep = 60;
    CHECK(!sg_tool_first(sg, r, &cur, 1000, "t2"), "tool after persona switch");
    /* 超时后工具拒 */
    sg_cancel_all(sg);
    uint32_t r2 = sg_begin(sg, &ep, 1000, 100);
    sg_epochs_t cur2 = ep;
    CHECK(sg_tool_first(sg, r2, &cur2, 1000, "t3"), "t3 within timeout");
    CHECK(!sg_tool_first(sg, r2, &cur2, 5000, "t4"), "t4 after timeout");
    sg_free(sg);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "context",   scenario_context },
        { "lifecycle", scenario_lifecycle },
        { "tools",     scenario_tools },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = s_fail;
        cases[i].fn();
        printf("%s %s\n", s_fail > before ? "FAIL" : "PASS", cases[i].name);
    }
    if (s_fail == 0) {
        printf("test_sessguard: ALL PASS\n");
        return 0;
    }
    printf("test_sessguard: %d FAILURE(S)\n", s_fail);
    return 1;
}
