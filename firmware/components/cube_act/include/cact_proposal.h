/*
 * cact_proposal.h —— LLM 提案严格解析(docs/17 §5.1/§4.4; docs/18 T11)
 *
 * 目标: 模型自由文本 → 受控 ActionRequest 的唯一入口。解析器按白名单
 * 键集与严格文法处理, 任何歧义即整体拒绝(不存在"部分接受"):
 *   - 键白名单: act/params/confidence/evidence/ttl_s/persona;
 *     未知键、重复键、缺 act 均拒绝。
 *   - 字符串不允许任何转义与控制字符(docs/15 对宽松解析的教训)。
 *   - 数字按固定文法: confidence 0..1(≤3 位小数), ttl_s/persona 为
 *     整数; NaN/Infinity/指数/超长数字一律拒绝。
 *   - params 仅一层对象, 值限 string/整数/bool; 嵌套即拒绝。
 *   - evidence 仅为字符串数组(有界); 引用是否真实存在由
 *     cact_proposal_evidence_known 注入回调核验 —— 引用存在不代表
 *     结论为真(docs/17 §4.4), 只是不因虚构 ID 获得额外话语权。
 *   - persona 代次随 to_req 进入 cact_req_t, 提交与出队双重复检
 *     (docs/17 §7.3 切角色屏障)。
 * 解析通过 ≠ 获得权限: 最终执行仍由 cube_act 策略/资源/回执裁定。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cube_act.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CACT_PROPOSAL_PARAMS_MAX  8
#define CACT_PARAM_KEY_MAX        16
#define CACT_PARAM_VAL_MAX        48
#define CACT_EVIDENCE_MAX         4
#define CACT_EVIDENCE_LEN_MAX     48

typedef struct {
    int64_t max_text_len;      /* 输入总长上限(含 NUL 前) */
    int64_t max_ttl_s;         /* ttl_s 上限 */
} cact_proposal_limits_t;

extern const cact_proposal_limits_t CACT_PROPOSAL_LIMITS_DEF;

typedef struct {
    char act[CACT_CAP_ID_MAX];
    struct {
        char key[CACT_PARAM_KEY_MAX];
        char val[CACT_PARAM_VAL_MAX];   /* int/bool 已转十进制字符串 */
    } params[CACT_PROPOSAL_PARAMS_MAX];
    int params_n;
    int confidence_milli;      /* 0..1000; -1=未提供 */
    char evidence[CACT_EVIDENCE_MAX][CACT_EVIDENCE_LEN_MAX];
    int evidence_n;
    int64_t ttl_s;             /* 0=未提供(不过期) */
    uint32_t persona_epoch;    /* 0=未指定 */
} cact_proposal_t;

/* 严格解析; 任何歧义/越界/未知键返回非 OK 且不写 out */
cact_res_t cact_proposal_parse(const char *text, size_t len,
                               const cact_proposal_limits_t *lim,
                               cact_proposal_t *out);

/* 证据核验: 全部引用通过 known() 返回 true; 无证据亦返回 true */
bool cact_proposal_evidence_known(const cact_proposal_t *p,
                                  bool (*known)(const char *ref, void *ud),
                                  void *ud);

/* → cube_act 请求: params 序列化为 "key\0value\0" 连接; expires=now+ttl */
cact_res_t cact_proposal_to_req(const cact_proposal_t *p, cact_origin_t origin,
                                int64_t now_ms, cact_req_t *req);

#ifdef __cplusplus
}
#endif
