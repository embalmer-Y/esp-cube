/* sessguard.c —— 会话守卫实现(见 sessguard.h; T31)
 *
 * 结构: 消息环形(首尾索引) + 单 in-flight 槽(会话串行: 同期至多一个
 * LLM 请求——双重运行防护即验收语义) + 工具 id 表(req 关联)。
 * 堆分配(SPIRAM_USE_MALLOC 下大块自动落 PSRAM)。
 */
#include <stdlib.h>
#include <string.h>

#include "sessguard.h"

typedef struct {
    char *text;                    /* msg_len + 1 */
    uint8_t role_user;             /* 0=assistant 1=user */
} sg_msg_t;

typedef struct {
    char *id;                      /* tool_cap × (id_len+1) */
    uint32_t req;
} sg_tool_t;

struct sessguard {
    int msg_cap, msg_len, tool_cap, id_len;
    sg_msg_t *msg;
    int head, count;               /* 环形: head=最旧, count=在环数 */
    /* in-flight(单槽) */
    uint32_t req_seq;              /* 请求 id 计数器(0 保留) */
    uint32_t req_id;
    uint32_t req_persona_ep, req_consent_ep;
    uint64_t req_deadline;
    sg_tool_t *tool;
};

int sg_init(sessguard_t **out, int msg_cap, int msg_len, int tool_cap)
{
    if (out == NULL || msg_cap <= 0 || msg_len <= 0 || tool_cap <= 0) {
        return -1;
    }
    sessguard_t *sg = calloc(1, sizeof(*sg));
    if (sg == NULL) return -1;
    sg->msg_cap = msg_cap;
    sg->msg_len = msg_len;
    sg->tool_cap = tool_cap;
    sg->id_len = 40;
    sg->msg = calloc((size_t)msg_cap, sizeof(sg_msg_t));
    sg->tool = calloc((size_t)tool_cap, sizeof(sg_tool_t));
    if (sg->msg == NULL || sg->tool == NULL) {
        sg_free(sg);
        return -1;
    }
    for (int i = 0; i < msg_cap; i++) {
        sg->msg[i].text = calloc((size_t)msg_len + 1, 1);
        if (sg->msg[i].text == NULL) {
            sg_free(sg);
            return -1;
        }
    }
    for (int i = 0; i < tool_cap; i++) {
        sg->tool[i].id = calloc((size_t)sg->id_len, 1);
        if (sg->tool[i].id == NULL) {
            sg_free(sg);
            return -1;
        }
    }
    *out = sg;
    return 0;
}

void sg_free(sessguard_t *sg)
{
    if (sg == NULL) return;
    if (sg->msg != NULL) {
        for (int i = 0; i < sg->msg_cap; i++) free(sg->msg[i].text);
        free(sg->msg);
    }
    if (sg->tool != NULL) {
        for (int i = 0; i < sg->tool_cap; i++) free(sg->tool[i].id);
        free(sg->tool);
    }
    free(sg);
}

int sg_append(sessguard_t *sg, const char *role, const char *text)
{
    if (sg == NULL || role == NULL || text == NULL) return -1;
    int slot;
    if (sg->count < sg->msg_cap) {
        slot = (sg->head + sg->count) % sg->msg_cap;
        sg->count++;
    } else {
        slot = sg->head;               /* 满: 覆写最旧, head 前移 */
        sg->head = (sg->head + 1) % sg->msg_cap;
    }
    sg->msg[slot].role_user =
        (strcmp(role, "user") == 0) ? 1u : 0u;
    strncpy(sg->msg[slot].text, text, (size_t)sg->msg_len);
    sg->msg[slot].text[sg->msg_len] = '\0';
    return 0;
}

int sg_count(const sessguard_t *sg) { return sg != NULL ? sg->count : 0; }

const char *sg_text_at(const sessguard_t *sg, int i)
{
    if (sg == NULL || i < 0 || i >= sg->count) return NULL;
    return sg->msg[(sg->head + i) % sg->msg_cap].text;
}

uint32_t sg_begin(sessguard_t *sg, const sg_epochs_t *ep, uint64_t now_ms,
                  uint64_t timeout_ms)
{
    if (sg == NULL || ep == NULL) return 0;
    if (sg->req_id != 0) return 0;      /* 已有 in-flight(串行) */
    sg->req_seq++;
    sg->req_id = sg->req_seq;
    sg->req_persona_ep = ep->persona_ep;
    sg->req_consent_ep = ep->consent_ep;
    sg->req_deadline = now_ms + timeout_ms;
    /* 工具表清位: 只清本请求关联的旧条目(全清亦可, 单槽语义) */
    for (int i = 0; i < sg->tool_cap; i++) sg->tool[i].req = 0;
    return sg->req_id;
}

static bool req_valid(const sessguard_t *sg, uint32_t req_id)
{
    return req_id != 0 && sg->req_id == req_id;
}

bool sg_emit_ok(const sessguard_t *sg, uint32_t req_id,
                const sg_epochs_t *cur, uint64_t now_ms)
{
    if (sg == NULL || cur == NULL || !req_valid(sg, req_id)) return false;
    if (now_ms >= sg->req_deadline) return false;   /* 超时 */
    /* 代次比对: 切人格/撤权限后旧请求自动失效(无需事件接线) */
    if (sg->req_persona_ep != cur->persona_ep ||
        sg->req_consent_ep != cur->consent_ep) return false;
    return true;
}

void sg_finish(sessguard_t *sg, uint32_t req_id)
{
    if (sg != NULL && req_valid(sg, req_id)) sg->req_id = 0;
}

void sg_cancel_all(sessguard_t *sg)
{
    if (sg == NULL) return;
    sg->req_id = 0;                     /* 未收口请求全部作废 */
    for (int i = 0; i < sg->tool_cap; i++) sg->tool[i].req = 0;
}

int sg_inflight(const sessguard_t *sg) { return sg != NULL && sg->req_id != 0 ? 1 : 0; }

bool sg_tool_first(sessguard_t *sg, uint32_t req_id,
                   const sg_epochs_t *cur, uint64_t now_ms, const char *id)
{
    if (!sg_emit_ok(sg, req_id, cur, now_ms)) {
        return false;                   /* 超时/代次变化/失效 → 工具一律拒 */
    }
    if (id == NULL || id[0] == '\0') return false;
    int free_slot = -1;
    for (int i = 0; i < sg->tool_cap; i++) {
        if (sg->tool[i].req == req_id &&
            strcmp(sg->tool[i].id, id) == 0) {
            return false;               /* 重复 */
        }
        if (sg->tool[i].req == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return false;    /* 表满(防御; tool_cap 足够) */
    sg->tool[free_slot].req = req_id;
    strncpy(sg->tool[free_slot].id, id, (size_t)sg->id_len - 1);
    sg->tool[free_slot].id[sg->id_len - 1] = '\0';
    return true;
}
