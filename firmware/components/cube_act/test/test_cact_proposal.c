/*
 * test_cact_proposal.c —— LLM 提案严格解析回归(宿主; docs/18 T11)
 *
 * 编译运行(仓库根; 或 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter -I firmware/components/cube_act/include \
 *       -o /tmp/test_cact_proposal \
 *       firmware/components/cube_act/test/test_cact_proposal.c \
 *       firmware/components/cube_act/src/cact_proposal.c \
 *       firmware/components/cube_act/src/cube_act_core.c \
 *   && /tmp/test_cact_proposal
 *
 * 验收(docs/18 T11): 恶意提示/重复键/非法数与 NaN/超长/伪引用/过期/
 * 角色变更 均拒绝或失效。全部合成语料。
 */
#include <stdio.h>
#include <string.h>

#include "cact_proposal.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- stubs(前置声明) ---- */
static cact_ctx_t *g_ctx;
static cact_ctx_t *test_ctx(void) { return g_ctx; }
static cact_res_t v_ok(const cact_desc_t *d, const uint8_t *p, size_t n)
{
    (void)d; (void)p; (void)n; return CACT_RES_OK;
}
static cact_res_t ex_stub(const cact_desc_t *d, uint32_t id,
                          const uint8_t *p, size_t n)
{
    (void)p; (void)n; (void)d;
    (void)cact_report(test_ctx(), id, CACT_ST_STARTED, NULL);
    (void)cact_report(test_ctx(), id, CACT_ST_COMPLETED, NULL);
    return CACT_RES_OK;
}
static bool ev_known_stub(const char *ref, void *ud)
{
    (void)ud;
    return strcmp(ref, "evt:1") == 0 || strcmp(ref, "mem:2") == 0;
}

static bool parse_ok(const char *s, cact_proposal_t *p)
{
    return cact_proposal_parse(s, strlen(s), NULL, p) == CACT_RES_OK;
}
static bool parse_reject(const char *s)
{
    cact_proposal_t p;
    return cact_proposal_parse(s, strlen(s), NULL, &p) != CACT_RES_OK;
}

/* ---- 有效输入 ---- */
static void scenario_valid(void)
{
    cact_proposal_t p;
    CHECK(parse_ok("{\"act\":\"music.play\",\"params\":{},"
                   "\"confidence\":0.8,\"evidence\":[\"evt:1\"],"
                   "\"ttl_s\":30,\"persona\":7}", &p), "valid full");
    CHECK(strcmp(p.act, "music.play") == 0, "act");
    CHECK(p.confidence_milli == 800, "confidence 0.8");
    CHECK(p.evidence_n == 1 && strcmp(p.evidence[0], "evt:1") == 0, "evidence");
    CHECK(p.ttl_s == 30 && p.persona_epoch == 7, "ttl/persona");

    CHECK(parse_ok("{\"act\":\"face.show\",\"params\":{\"asset\":\"happy\","
                   "\"ms\":2000,\"loop\":false}}", &p), "valid params");
    CHECK(p.params_n == 3, "param count");
    CHECK(strcmp(p.params[0].key, "asset") == 0 &&
          strcmp(p.params[0].val, "happy") == 0, "string param");
    CHECK(strcmp(p.params[1].val, "2000") == 0, "int param");
    CHECK(strcmp(p.params[2].val, "false") == 0, "bool param");
    CHECK(p.confidence_milli == -1 && p.ttl_s == 0, "optional absent");

    CHECK(parse_ok("{\"act\":\"a.b_09\",\"params\":{}}", &p), "charset act");
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{},\"confidence\":1}", &p),
          "confidence 1");
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{},\"confidence\":0.123}", &p),
          "confidence 3 decimals");
    CHECK(cact_proposal_evidence_known(&p, ev_known_stub, NULL),
          "no evidence = known");

    /* 中文参数值按字节透传 */
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{\"q\":\"周杰伦\"}}", &p),
          "utf8 param");
    CHECK(strcmp(p.params[0].val, "周杰伦") == 0, "utf8 roundtrip");
}

/* ---- 拒绝语料(docs/18 T11) ---- */
static void scenario_rejects(void)
{
    /* 结构与白名单 */
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"act\":\"b\"}"),
          "duplicate top key");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"k\":1,\"k\":2}}"),
          "duplicate param key");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"evil\":1}"),
          "unknown key");
    CHECK(parse_reject("{\"params\":{}}"), "missing act");
    CHECK(parse_reject("{}"), "empty object");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{}}extra"),
          "trailing garbage");
    CHECK(parse_reject("{\"act\" \"a\",\"params\":{}}"), "missing colon");
    CHECK(parse_reject("{act:\"a\",\"params\":{}}"), "unquoted key");
    CHECK(parse_reject("{'act':'a','params':{}}"), "single quotes");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":{\"n\":1}}}"),
          "nested param object");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":null}}"),
          "null param");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":[1]}}"),
          "array param");
    CHECK(parse_reject("{\"act\":\"a b\",\"params\":{}}"), "space in act");
    CHECK(parse_reject("{\"act\":\"Music.Play\",\"params\":{}}"),
          "uppercase act");

    /* 转义与控制字符(docs/15 教训: 不做宽松转义) */
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":\"l\\\"l\"}}"),
          "escape in string");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":\"l\tl\"}}"),
          "raw tab in string");

    /* 数字 */
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":NaN}"),
          "NaN literal");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":Infinity}"),
          "Infinity literal");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":0.8e2}"),
          "exponent form");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":1.5}"),
          "confidence >1");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":-0.1}"),
          "confidence <0");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"confidence\":0.1234}"),
          "confidence 4 decimals");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"ttl_s\":0}"),
          "ttl zero");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"ttl_s\":-5}"),
          "ttl negative");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"ttl_s\":99999999999999}"),
          "ttl huge");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{\"x\":3.5}}"),
          "float param value");

    /* evidence */
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"evidence\":"
                       "[\"e1\",\"e2\",\"e3\",\"e4\",\"e5\"]}"),
          "evidence overflow");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"evidence\":[\"\"]}"),
          "empty evidence ref");
    CHECK(parse_reject("{\"act\":\"a\",\"params\":{},\"evidence\":[1]}"),
          "non-string evidence");

    /* 超长(docs/15: 旧解析器 CONTENT_LONG 被上层接受) */
    char big[1200];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    cact_proposal_t p;
    CHECK(cact_proposal_parse(big, strlen(big), NULL, &p) != CACT_RES_OK,
          "overlong text rejected");
}

/* 伪引用: 引用不存在 → evidence_known false */
static void scenario_fake_evidence(void)
{
    cact_proposal_t p;
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{},"
                   "\"evidence\":[\"evt:1\",\"mem:2\"]}", &p), "parse refs");
    CHECK(cact_proposal_evidence_known(&p, ev_known_stub, NULL),
          "known refs pass");
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{},"
                   "\"evidence\":[\"evt:1\",\"mem:999\"]}", &p), "parse fake");
    CHECK(!cact_proposal_evidence_known(&p, ev_known_stub, NULL),
          "fake ref detected");
}

/* to_req: TTL→expires, 参数序列化, epoch 透传 */
static void scenario_to_req(void)
{
    cact_proposal_t p;
    CHECK(parse_ok("{\"act\":\"music.play\",\"params\":{\"q\":\"jazz\",\"n\":3},"
                   "\"ttl_s\":30,\"persona\":7}", &p), "parse");
    cact_req_t r;
    CHECK(cact_proposal_to_req(&p, CACT_ORIGIN_AUTONOMOUS_LLM, 1000, &r) ==
          CACT_RES_OK, "to_req");
    CHECK(strcmp(r.capability, "music.play") == 0, "capability");
    CHECK(r.expires_ms == 31000, "expires now+ttl");
    CHECK(r.persona_epoch == 7, "epoch carried");
    /* 参数 blob: "q\0jazz\0n\03\0" = (1+1)+(4+1)+(1+1)+(1+1) = 11 字节 */
    CHECK(r.params_len == 11, "params blob size");
    CHECK(memcmp(r.params, "q\0jazz\0", 7) == 0, "params head");

    /* 无 ttl → 不过期 */
    CHECK(parse_ok("{\"act\":\"x\",\"params\":{}}", &p), "parse no ttl");
    CHECK(cact_proposal_to_req(&p, CACT_ORIGIN_PHYSICAL_USER, 1000, &r) ==
          CACT_RES_OK && r.expires_ms == 0, "no expiry");
}

/* 集成: 角色变更在提交与出队双重失效(docs/17 §7.3) */
static void scenario_epoch_gate(void)
{
    cact_ctx_t c;
    cact_init(&c);
    cact_set_persona_epoch(&c, 7);
    cact_boot_gate_open(&c, true);            /* 场景前提: 意图已恢复 */
    cact_desc_t d;
    memset(&d, 0, sizeof(d));
    d.cap_id = "music.play";
    d.group_id = 1;
    d.category = CACT_CAT_AUDIO;
    d.risk = CACT_RISK_LOW;
    d.validate = v_ok;
    d.execute = ex_stub;
    CHECK(cact_register(&c, &d) == CACT_RES_OK, "register");
    g_ctx = &c;                                 /* ex_stub 回报通道 */

    cact_proposal_t p;
    CHECK(parse_ok("{\"act\":\"music.play\",\"params\":{},\"persona\":7}",
                   &p), "parse epoch7");
    cact_req_t r;
    CHECK(cact_proposal_to_req(&p, CACT_ORIGIN_AUTONOMOUS_LLM, 0, &r) ==
          CACT_RES_OK, "to_req");
    uint32_t id = 0;
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit epoch7");

    /* 切角色 → 出队复检失效 */
    cact_set_persona_epoch(&c, 8);
    cact_pump(&c);
    cact_receipt_t rc;
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_REJECTED &&
          strcmp(rc.reason, "stale_epoch") == 0, "stale_epoch rejected");

    /* 新提案带旧代次 → 提交即拒 */
    CHECK(parse_ok("{\"act\":\"music.play\",\"params\":{},\"persona\":7}",
                   &p), "parse stale proposal");
    CHECK(cact_proposal_to_req(&p, CACT_ORIGIN_AUTONOMOUS_LLM, 0, &r) ==
          CACT_RES_OK, "to_req stale");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_STALE, "submit stale rejected");

    /* 当代次提案可执行 */
    CHECK(parse_ok("{\"act\":\"music.play\",\"params\":{},\"persona\":8}",
                   &p), "parse epoch8");
    CHECK(cact_proposal_to_req(&p, CACT_ORIGIN_AUTONOMOUS_LLM, 0, &r) ==
          CACT_RES_OK, "to_req cur");
    CHECK(cact_submit(&c, &r, &id) == CACT_RES_OK, "submit ok");
    cact_pump(&c);
    CHECK(cact_receipt_last(&c, id, &rc) == CACT_RES_OK &&
          rc.status == CACT_ST_COMPLETED, "current epoch executes");
    g_ctx = NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    scenario_valid();
    scenario_rejects();
    scenario_fake_evidence();
    scenario_to_req();
    scenario_epoch_gate();

    if (s_fail == 0) {
        printf("test_cact_proposal: ALL PASS\n");
        return 0;
    }
    printf("test_cact_proposal: %d FAILURE(S)\n", s_fail);
    return 1;
}
