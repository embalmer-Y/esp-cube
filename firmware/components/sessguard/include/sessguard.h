/* sessguard.h —— 会话守卫: 有界工作上下文 + 旧请求失效门(T31; docs/18 T31)
 *
 * 职责(H+S0 验收语义):
 *   1. 有界工作上下文: 多轮消息环形(容量上限, 滴出最旧) —— 语境不无界增长;
 *   2. 请求失效门: in-flight 请求携带 (persona 代次, 授权代次, 截止时刻);
 *      发声/写记忆/执行工具前查 emit_ok —— 超时/切人格/撤权限后旧请求
 *      自动失效(代次比对, 无需事件接线), 显式 cancel 兜底;
 *   3. 工具回执去重: 同一 tool_call id 只执行一次(重试/轮回不重复执行);
 *      cancel 后未执行的 id 一律拒。
 *
 * 纯逻辑无 I/O(宿主可测); 缓冲 init 时堆分配(固件=PSRAM 路径)。
 */
#ifndef SESSGUARD_H
#define SESSGUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SG_RESULT 0

typedef struct sessguard sessguard_t;

/* 初始化(堆分配缓冲; msg_cap=保留轮数, msg_len=单条上限,
 * tool_cap=工具 id 表容量); 返回 0=OK, -1=参数/分配失败 */
int sg_init(sessguard_t **out, int msg_cap, int msg_len, int tool_cap);
void sg_free(sessguard_t *sg);

/* ---- 有界上下文 ---- */
/* 追加一轮(role: "user"/"assistant"; 超长截断; 满则滴出最旧) */
int sg_append(sessguard_t *sg, const char *role, const char *text);
/* 当前保留轮数(宿主断言用) */
int sg_count(const sessguard_t *sg);
/* 第 i 轮(0=最旧); 越界返回 NULL */
const char *sg_text_at(const sessguard_t *sg, int i);

/* ---- 请求生命周期 ----
 * 代次语义: persona/consent 代次由调用方维护(cube_act persona epoch +
 * persona consent epoch), begin 时快照, emit 时比对 —— 切人格/撤权限
 * 自动使旧请求失效, 无需事件接线 */
typedef struct { uint32_t persona_ep; uint32_t consent_ep; } sg_epochs_t;

/* 登记 in-flight 请求; 返回请求 id(>0), 0=已有未收口请求(串行会话) */
uint32_t sg_begin(sessguard_t *sg, const sg_epochs_t *ep, uint64_t now_ms,
                  uint64_t timeout_ms);
/* 发声/写记忆/执行工具前查: 请求有效且未超时且代次未变
 * (cur=调用方当前代次快照 —— 切人格/撤权限后 mismatch 自动失效) */
bool sg_emit_ok(const sessguard_t *sg, uint32_t req_id,
                const sg_epochs_t *cur, uint64_t now_ms);
/* 请求收口(正常结束; 之后 id 作废) */
void sg_finish(sessguard_t *sg, uint32_t req_id);
/* 全失效(会话终止/超时兜底; 未收口请求全部作废) */
void sg_cancel_all(sessguard_t *sg);
/* 当前未收口请求数(0/1; 宿主断言用) */
int sg_inflight(const sessguard_t *sg);

/* ---- 工具回执去重 ----
 * 首见 id → true(允许执行并登记); 重复 id → false; 超时/代次变化/
 * cancel 后全拒(cur 语义同 emit_ok) */
bool sg_tool_first(sessguard_t *sg, uint32_t req_id,
                   const sg_epochs_t *cur, uint64_t now_ms, const char *id);

#ifdef __cplusplus
}
#endif
#endif /* SESSGUARD_H */
