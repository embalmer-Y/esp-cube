/*
 * cube_act_core.c —— 统一动作核心(注册表/有界队列/状态机/回执/停止锁存)
 *
 * 纯 C 单线程语义(docs/17 §15.4 ActionScheduler 骨架):
 *   - 所有 API 由动作 owner 任务串行调用; 核心不自取时间, now_ms 注入。
 *   - 提交时做策略预检, 出队 pump 时对自主来源复检(关开关/停止锁存/
 *     到期), 执行前状态才置 ACCEPTED 并调用 execute。
 *   - cact_report 是唯一状态推进通道; 迁移合法性按 docs/17 §5.2 图校验。
 *   - 回执环形保留 CACT_RECEIPTS_MAX 条; 幂等表满逐最旧。
 */
#include <string.h>

#include "cube_act.h"

/* ---- 并发 port(递归锁; 见 cube_act.h 注释) ---- */
static const cact_lock_port_t *s_lk;
static cact_ctx_t s_default_ctx;

void cact_set_lock_port(const cact_lock_port_t *port) { s_lk = port; }
void cact_default_init(void) { cact_init(&s_default_ctx); }
cact_ctx_t *cact_default(void) { return &s_default_ctx; }

#define CA_LOCK() do { if (s_lk) s_lk->enter(); } while (0)
#define CA_UNLOCK() do { if (s_lk) s_lk->exit(); } while (0)

/* 包装器在前, 实现(_locked)在后 —— 前向声明 */
static cact_res_t cact_submit_locked(cact_ctx_t *c, const cact_req_t *req,
                                     uint32_t *out_action_id);
static cact_res_t cact_confirm_locked(cact_ctx_t *c, uint32_t action_id);
static void cact_pump_locked(cact_ctx_t *c);
static cact_res_t cact_report_locked(cact_ctx_t *c, uint32_t action_id,
                                     cact_status_t st, const char *reason);
static cact_res_t cact_cancel_locked(cact_ctx_t *c, uint32_t action_id,
                                     const char *reason);
static cact_res_t cact_receipt_last_locked(cact_ctx_t *c, uint32_t id,
                                           cact_receipt_t *out);
static cact_res_t cact_user_stop_locked(cact_ctx_t *c, int group_id,
                                        const char *reason);
static cact_res_t cact_user_resume_locked(cact_ctx_t *c, int group_id);

bool cact_status_is_terminal(cact_status_t s)
{
    return s == CACT_ST_COMPLETED || s == CACT_ST_FAILED ||
           s == CACT_ST_CANCELLED || s == CACT_ST_INTERRUPTED ||
           s == CACT_ST_REJECTED || s == CACT_ST_UNKNOWN_OUTCOME;
}


void cact_init(cact_ctx_t *c)
{
    memset(c, 0, sizeof(*c));
    c->autonomous_enabled = true;   /* 产品默认(docs/16 FR-14) */
    for (int i = 0; i < CACT_CAT_MAX; i++) c->cat_enabled[i] = true;
    for (int g = 1; g < CACT_GROUPS_MAX; g++) c->quiet.scope[g] = true;
    c->quiet.enabled = false;       /* 勿扰默认不启用(docs/16 FR-21) */
    c->boot_gate_open = false;      /* 启动安全闸: 意图恢复前不开(docs/17 §5.5) */
    c->next_id = 1;
}

/* 勿扰是否对该组生效: 未启用→否; 启用且时间不可信→保守视为是;
 * 启用且可信→按当日分钟窗(支持跨夜)判定。组 0(无组)不受勿扰约束。 */
static bool quiet_active_for(const cact_ctx_t *c, int group_id)
{
    if (!c->quiet.enabled) return false;
    if (group_id <= 0 || group_id >= CACT_GROUPS_MAX) return false;
    if (!c->quiet.scope[group_id]) return false;
    if (!c->quiet_time_trusted) return true;      /* 时间未知: 保守(docs/17 §5.5) */
    int m = c->quiet_minute;
    int s = c->quiet.start_min, e = c->quiet.end_min;
    if (s == e) return true;                      /* 全天 */
    if (s < e) return m >= s && m < e;
    return m >= s || m < e;                       /* 跨夜 */
}

/* ---- 回执 ---- */
static void emit_receipt(cact_ctx_t *c, const cact_job_t *j,
                         cact_status_t st, const char *reason)
{
    cact_receipt_t *r = &c->receipts[c->rc_head];
    c->rc_head = (c->rc_head + 1) % CACT_RECEIPTS_MAX;
    memset(r, 0, sizeof(*r));
    r->action_id = j->id;
    r->seq = ++c->rc_seq;
    r->status = st;
    r->source = j->req.origin;
    r->observed_ms = c->now_ms;
    if (reason != NULL) {
        strncpy(r->reason, reason, CACT_REASON_MAX - 1);
        r->reason[CACT_REASON_MAX - 1] = '\0';
    }
}

static cact_job_t *find_job(cact_ctx_t *c, uint32_t id)
{
    for (int i = 0; i < c->q_len; i++) {
        if (c->q[i].id == id) return &c->q[i];
    }
    return NULL;
}

cact_res_t cact_receipt_last(cact_ctx_t *c, uint32_t id, cact_receipt_t *out)
{
    CA_LOCK();
    cact_res_t r = cact_receipt_last_locked(c, id, out);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_receipt_last_locked(cact_ctx_t *c, uint32_t id,
                                           cact_receipt_t *out)
{
    for (int k = 0; k < CACT_RECEIPTS_MAX; k++) {
        int idx = (c->rc_head - 1 - k + CACT_RECEIPTS_MAX * 2) %
                  CACT_RECEIPTS_MAX;
        if (c->receipts[idx].action_id == id &&
            c->receipts[idx].status != CACT_ST_NONE) {
            *out = c->receipts[idx];
            return CACT_RES_OK;
        }
    }
    return CACT_RES_INVALID;
}

/* ---- 幂等 ---- */
static int idem_find(cact_ctx_t *c, uint32_t key)
{
    if (key == 0) return -1;
    for (int i = 0; i < c->idem_n; i++) {
        if (c->idem[i].key == key) return i;
    }
    return -1;
}

static void idem_put(cact_ctx_t *c, uint32_t key, cact_id_t id)
{
    if (key == 0) return;
    int idx = idem_find(c, key);
    if (idx >= 0) { c->idem[idx].id = id; return; }
    if (c->idem_n == CACT_IDEM_MAX) {         /* 满则逐最旧(FIFO) */
        memmove(&c->idem[0], &c->idem[1],
                sizeof(c->idem[0]) * (CACT_IDEM_MAX - 1));
        c->idem_n--;
    }
    c->idem[c->idem_n].key = key;
    c->idem[c->idem_n].id = id;
    c->idem[c->idem_n].known_final = false;
    c->idem_n++;
}

static void idem_mark_final(cact_ctx_t *c, cact_id_t id)
{
    for (int i = 0; i < c->idem_n; i++) {
        if (c->idem[i].id == id) c->idem[i].known_final = true;
    }
}

/* ---- 注册 ---- */
cact_res_t cact_register(cact_ctx_t *c, const cact_desc_t *d)
{
    if (d == NULL || d->cap_id == NULL || d->validate == NULL ||
        d->execute == NULL) {
        return CACT_RES_INVALID;
    }
    if (c->caps_n >= CACT_CAPS_MAX) return CACT_RES_NO_SPACE;
    for (int i = 0; i < c->caps_n; i++) {     /* 不允许覆盖同名能力 */
        if (strcmp(c->caps[i]->cap_id, d->cap_id) == 0) {
            return CACT_RES_INVALID;
        }
    }
    c->caps[c->caps_n++] = d;
    return CACT_RES_OK;
}

static int cap_find(cact_ctx_t *c, const char *cap_id)
{
    for (int i = 0; i < c->caps_n; i++) {
        if (strcmp(c->caps[i]->cap_id, cap_id) == 0) return i;
    }
    return -1;
}

/* ---- 策略预检(提交与出队复检共用) ---- */
static cact_res_t policy_check(cact_ctx_t *c, int cap_idx,
                               const cact_req_t *req)
{
    const cact_desc_t *d = c->caps[cap_idx];
    if (cact_origin_is_autonomous(req->origin)) {
        if (!c->boot_gate_open) {
            return CACT_RES_FORBIDDEN;   /* 启动安全闸未开(意图未恢复) */
        }
        if (!c->autonomous_enabled ||
            (d->group_id > 0 && c->group_stopped[d->group_id])) {
            return CACT_RES_FORBIDDEN;   /* 总开关/停止锁存(docs/17 §5.5) */
        }
        if (d->category >= 0 && d->category < CACT_CAT_MAX &&
            !c->cat_enabled[d->category]) {
            return CACT_RES_FORBIDDEN;   /* 分类控制(docs/18 T12) */
        }
        if (quiet_active_for(c, d->group_id)) {
            return CACT_RES_FORBIDDEN;   /* 勿扰: 仅限新自主(docs/16 FR-21) */
        }
    }
    if (d->risk == CACT_RISK_FORBIDDEN) {
        return CACT_RES_FORBIDDEN;   /* 本版禁止自动执行 */
    }
    if (req->persona_epoch != 0 && req->persona_epoch != c->persona_epoch) {
        return CACT_RES_STALE;       /* 角色代次已变(docs/18 T11) */
    }
    return CACT_RES_OK;
}

/* ---- 提交 ---- */
static void supersede_same_cap_locked(cact_ctx_t *c, int cap_idx)
{
    for (int i = 0; i < c->q_len; ) {
        cact_job_t *j = &c->q[i];
        if (j->cap_idx == cap_idx && j->st != CACT_ST_AWAITING_CONFIRM) {
            j->st = CACT_ST_INTERRUPTED;       /* 被新动作替换(首段单资源) */
            emit_receipt(c, j, CACT_ST_INTERRUPTED, "superseded");
            idem_mark_final(c, j->id);
            memmove(&c->q[i], &c->q[i + 1],
                    sizeof(c->q[0]) * (size_t)(c->q_len - i - 1));
            c->q_len--;
            continue;
        }
        i++;
    }
}

cact_res_t cact_submit(cact_ctx_t *c, const cact_req_t *req,
                       uint32_t *out_action_id)
{
    CA_LOCK();
    cact_res_t r = cact_submit_locked(c, req, out_action_id);
    CA_UNLOCK();
    return r;
}

const char *cact_status_name(cact_status_t s)
{
    switch (s) {
    case CACT_ST_NONE:              return "none";
    case CACT_ST_QUEUED:            return "queued";
    case CACT_ST_ACCEPTED:          return "accepted";
    case CACT_ST_STARTED:           return "started";
    case CACT_ST_COMPLETED:         return "completed";
    case CACT_ST_FAILED:            return "failed";
    case CACT_ST_CANCEL_REQUESTED:  return "cancel_requested";
    case CACT_ST_CANCELLED:         return "cancelled";
    case CACT_ST_INTERRUPTED:       return "interrupted";
    case CACT_ST_REJECTED:          return "rejected";
    case CACT_ST_UNKNOWN_OUTCOME:   return "unknown_outcome";
    case CACT_ST_AWAITING_CONFIRM:  return "awaiting_confirm";
    default:                        return "?";
    }
}

static cact_res_t cact_submit_locked(cact_ctx_t *c, const cact_req_t *req,
                                     uint32_t *out_action_id)
{
    if (req == NULL || out_action_id == NULL) return CACT_RES_INVALID;
    int ci = cap_find(c, req->capability);
    if (ci < 0) return CACT_RES_UNSUPPORTED;

    /* 幂等: 同键已有记录 → 返回原 id, 不产生第二次副作用 */
    int ii = idem_find(c, req->idem_key);
    if (ii >= 0) {
        *out_action_id = c->idem[ii].id;
        return CACT_RES_STALE;   /* 重放无新执行(语义见 docs/17 §5.2) */
    }
    if (req->params_len > CACT_PARAMS_MAX) return CACT_RES_INVALID;
    if (c->caps[ci]->validate(c->caps[ci], req->params,
                              req->params_len) != CACT_RES_OK) {
        return CACT_RES_INVALID;
    }
    cact_res_t pol = policy_check(c, ci, req);
    if (pol != CACT_RES_OK) return pol;

    supersede_same_cap_locked(c, ci);   /* 同能力在途: 被新动作替换 */

    if (c->caps[ci]->risk == CACT_RISK_SENSITIVE && !req->confirmed) {
        /* 敏感动作: 未确认不入执行队列, 挂等待确认(不占队列槽) */
        if (c->q_len >= CACT_QUEUE_MAX) return CACT_RES_BUSY;
        /* 占一个槽挂起, 确认后原位转 QUEUED; 过期由 pump 回收 */
        cact_job_t *j = &c->q[c->q_len++];
        memset(j, 0, sizeof(*j));
        j->id = c->next_id++;
        j->st = CACT_ST_AWAITING_CONFIRM;
        j->cap_idx = ci;
        j->req = *req;
        idem_put(c, req->idem_key, j->id);
        emit_receipt(c, j, CACT_ST_AWAITING_CONFIRM, "need_confirm");
        *out_action_id = j->id;
        return CACT_RES_OK;
    }

    if (c->q_len >= CACT_QUEUE_MAX) return CACT_RES_BUSY;
    cact_job_t *j = &c->q[c->q_len++];
    memset(j, 0, sizeof(*j));
    j->id = c->next_id++;
    j->st = CACT_ST_QUEUED;
    j->cap_idx = ci;
    j->req = *req;
    idem_put(c, req->idem_key, j->id);
    emit_receipt(c, j, CACT_ST_QUEUED, NULL);
    *out_action_id = j->id;
    return CACT_RES_OK;
}

/* ---- 敏感动作确认(确认令牌语义的骨架: 动作 id 本身即凭据,
 * docs/17 §5.3 的绑定哈希/nonce 在网关层补齐) ---- */
cact_res_t cact_confirm(cact_ctx_t *c, uint32_t action_id)
{
    CA_LOCK();
    cact_res_t r = cact_confirm_locked(c, action_id);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_confirm_locked(cact_ctx_t *c, uint32_t action_id)
{
    cact_job_t *j = find_job(c, action_id);
    if (j == NULL || j->st != CACT_ST_AWAITING_CONFIRM) {
        return CACT_RES_STALE;
    }
    cact_res_t pol = policy_check(c, j->cap_idx, &j->req);
    if (pol != CACT_RES_OK) {                 /* 确认期间开关/锁存变化 */
        j->st = CACT_ST_REJECTED;
        emit_receipt(c, j, CACT_ST_REJECTED, "confirm_policy");
        idem_mark_final(c, j->id);
        return pol;
    }
    j->st = CACT_ST_QUEUED;
    emit_receipt(c, j, CACT_ST_QUEUED, "confirmed");
    return CACT_RES_OK;
}

/* ---- 出队泵(单动作 owner 循环调用) ---- */
void cact_pump(cact_ctx_t *c)
{
    CA_LOCK();
    cact_pump_locked(c);
    CA_UNLOCK();
}

static void cact_pump_locked(cact_ctx_t *c)
{
    if (c->q_len == 0) return;

    /* 1) 到期回收: QUEUED/AWAITING 超时 → rejected(docs/17 TTL) */
    for (int i = 0; i < c->q_len; ) {
        cact_job_t *j = &c->q[i];
        bool expiry = j->req.expires_ms != 0 &&
                      c->now_ms > j->req.expires_ms;
        bool expirable = j->st == CACT_ST_QUEUED ||
                         j->st == CACT_ST_AWAITING_CONFIRM;
        if (expiry && expirable) {
            j->st = CACT_ST_REJECTED;
            emit_receipt(c, j, CACT_ST_REJECTED, "expired");
            idem_mark_final(c, j->id);
            memmove(&c->q[i], &c->q[i + 1],
                    sizeof(c->q[0]) * (size_t)(c->q_len - i - 1));
            c->q_len--;
            continue;
        }
        i++;
    }
    if (c->q_len == 0) return;

    /* 2) 出队执行: 找首个 QUEUED(跳过 STARTED 在途与 AWAITING —— 不同
     *    能力互不阻塞(docs/17 §5.4 资源并发); 板上 T13 暴露的队头阻塞);
     *    复检失败 → rejected */
    int qi = -1;
    for (int i = 0; i < c->q_len; i++) {
        if (c->q[i].st == CACT_ST_QUEUED) { qi = i; break; }
    }
    if (qi < 0) return;
    cact_job_t *j = &c->q[qi];
    cact_res_t pol = policy_check(c, j->cap_idx, &j->req);
    if (pol != CACT_RES_OK) {
        j->st = CACT_ST_REJECTED;
        emit_receipt(c, j, CACT_ST_REJECTED,
                     pol == CACT_RES_FORBIDDEN ? "policy" :
                     pol == CACT_RES_STALE ? "stale_epoch" : "busy");
        idem_mark_final(c, j->id);
        memmove(&c->q[qi], &c->q[qi + 1],
                sizeof(c->q[0]) * (size_t)(c->q_len - qi - 1));
        c->q_len--;
        return;
    }
    j->st = CACT_ST_ACCEPTED;
    j->started_ms = c->now_ms;
    emit_receipt(c, j, CACT_ST_ACCEPTED, NULL);
    /* 参数拷贝后传入: 执行器同步 report 终态会触发队列前移,
     * 不得让其指针指向队列槽(docs/17 §15.5 参数所有权) */
    uint8_t pcopy[CACT_PARAMS_MAX];
    size_t plen = j->req.params_len;
    memcpy(pcopy, j->req.params, plen);
    uint32_t exec_id = j->id;
    const cact_desc_t *d = c->caps[j->cap_idx];
    int exec_qi = qi;
    cact_res_t er = d->execute(d, exec_id, pcopy, plen);
    /* execute 可能已同步推进到终态并移除 job —— 此后禁止解引用 j */
    if (c->q_len > exec_qi && c->q[exec_qi].id == exec_id &&
        er != CACT_RES_OK && !cact_status_is_terminal(c->q[exec_qi].st)) {
        c->q[exec_qi].st = CACT_ST_FAILED;    /* 执行器拒绝受理 */
        emit_receipt(c, &c->q[exec_qi], CACT_ST_FAILED, "exec_reject");
        idem_mark_final(c, exec_id);
    }
    /* 3) 已终结的执行位移出(执行器可能同步 report 到终态) */
    if (c->q_len > exec_qi && c->q[exec_qi].id == exec_id &&
        cact_status_is_terminal(c->q[exec_qi].st)) {
        memmove(&c->q[exec_qi], &c->q[exec_qi + 1],
                sizeof(c->q[0]) * (size_t)(c->q_len - exec_qi - 1));
        c->q_len--;
    }
}

/* ---- 状态推进(执行器唯一通道) ---- */
cact_res_t cact_report(cact_ctx_t *c, uint32_t action_id,
                       cact_status_t st, const char *reason)
{
    CA_LOCK();
    cact_res_t r = cact_report_locked(c, action_id, st, reason);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_report_locked(cact_ctx_t *c, uint32_t action_id,
                                     cact_status_t st, const char *reason)
{
    cact_job_t *j = find_job(c, action_id);
    if (j == NULL) {
        /* 已出队的动作: 回执仍需记录(终态可能晚到) → 只发回执 */
        /* 骨架: 查不到一律 INVALID; 真实执行器在终态前持引用 */
        return CACT_RES_INVALID;
    }
    cact_status_t cur = j->st;
    bool legal = false;
    switch (st) {
    case CACT_ST_STARTED:
        legal = (cur == CACT_ST_ACCEPTED);
        break;
    case CACT_ST_COMPLETED:
        legal = (cur == CACT_ST_STARTED);     /* docs/17: started 才能完成 */
        break;
    case CACT_ST_FAILED:
        legal = (cur == CACT_ST_STARTED || cur == CACT_ST_ACCEPTED ||
                 cur == CACT_ST_CANCEL_REQUESTED);
        break;
    case CACT_ST_CANCEL_REQUESTED:
        legal = (cur == CACT_ST_ACCEPTED || cur == CACT_ST_STARTED);
        break;
    case CACT_ST_CANCELLED:
    case CACT_ST_INTERRUPTED:
    case CACT_ST_UNKNOWN_OUTCOME:
        legal = (cur == CACT_ST_CANCEL_REQUESTED ||
                 cur == CACT_ST_STARTED || cur == CACT_ST_ACCEPTED);
        break;
    default:
        legal = false;                        /* 不允许 report 中间态/终态重放 */
        break;
    }
    if (!legal) return CACT_RES_INVALID;
    j->st = st;
    emit_receipt(c, j, st, reason);
    if (cact_status_is_terminal(st)) {
        idem_mark_final(c, j->id);
        /* 从队列移除(终态 job 不再占槽) */
        for (int i = 0; i < c->q_len; i++) {
            if ((uint32_t)c->q[i].id == action_id) {
                memmove(&c->q[i], &c->q[i + 1],
                        sizeof(c->q[0]) * (size_t)(c->q_len - i - 1));
                c->q_len--;
                break;
            }
        }
    }
    return CACT_RES_OK;
}

/* ---- 取消 ---- */
cact_res_t cact_cancel(cact_ctx_t *c, uint32_t action_id, const char *reason)
{
    CA_LOCK();
    cact_res_t r = cact_cancel_locked(c, action_id, reason);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_cancel_locked(cact_ctx_t *c, uint32_t action_id,
                                     const char *reason)
{
    cact_job_t *j = find_job(c, action_id);
    if (j == NULL) {
        /* 已出队: 回执为终态 → 稳定答"无可取消"(STALE); 完全未知 → INVALID */
        cact_receipt_t rc;
        return (cact_receipt_last(c, action_id, &rc) == CACT_RES_OK &&
                cact_status_is_terminal(rc.status))
                   ? CACT_RES_STALE : CACT_RES_INVALID;
    }
    switch (j->st) {
    case CACT_ST_QUEUED:
        j->st = CACT_ST_CANCELLED;            /* 未执行: 直接终态 */
        emit_receipt(c, j, CACT_ST_CANCELLED, reason);
        idem_mark_final(c, j->id);
        for (int i = 0; i < c->q_len; i++) {
            if ((uint32_t)c->q[i].id == action_id) {
                memmove(&c->q[i], &c->q[i + 1],
                        sizeof(c->q[0]) * (size_t)(c->q_len - i - 1));
                c->q_len--;
                break;
            }
        }
        return CACT_RES_OK;
    case CACT_ST_AWAITING_CONFIRM:
        j->st = CACT_ST_REJECTED;
        emit_receipt(c, j, CACT_ST_REJECTED, reason);
        idem_mark_final(c, j->id);
        for (int i = 0; i < c->q_len; i++) {
            if ((uint32_t)c->q[i].id == action_id) {
                memmove(&c->q[i], &c->q[i + 1],
                        sizeof(c->q[0]) * (size_t)(c->q_len - i - 1));
                c->q_len--;
                break;
            }
        }
        return CACT_RES_OK;
    case CACT_ST_ACCEPTED:
    case CACT_ST_STARTED:
        j->st = CACT_ST_CANCEL_REQUESTED;
        emit_receipt(c, j, CACT_ST_CANCEL_REQUESTED, reason);
        if (c->caps[j->cap_idx]->cancel != NULL) {
            c->caps[j->cap_idx]->cancel(c->caps[j->cap_idx], j->id);
        }
        return CACT_RES_OK;
    default:
        return CACT_RES_STALE;                /* 终态/取消中: 无新操作 */
    }
}

/* ---- 控制面 ---- */
/* ---- T38 设置持久化 ---- */
#define CACT_STORE_MAGIC 0x43414354u   /* "CACT" */
static const cact_store_t *s_store;

static void settings_pack(const cact_ctx_t *c, uint8_t buf[16])
{
    uint32_t scope = 0;
    for (int g = 0; g < CACT_GROUPS_MAX; g++) {
        if (c->quiet.scope[g]) scope |= (1u << g);
    }
    memset(buf, 0, 16);
    uint32_t magic = CACT_STORE_MAGIC;
    memcpy(buf, &magic, 4);
    buf[4] = c->autonomous_enabled ? 1u : 0u;
    buf[5] = c->quiet.enabled ? 1u : 0u;
    uint16_t se = (uint16_t)c->quiet.start_min, ee = (uint16_t)c->quiet.end_min;
    memcpy(buf + 6, &se, 2);
    memcpy(buf + 8, &ee, 2);
    uint32_t sb = scope;
    memcpy(buf + 10, &sb, 4);
}

static void settings_apply(cact_ctx_t *c, const uint8_t buf[16])
{
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != CACT_STORE_MAGIC) return;      /* 损毁 → 默认 */
    c->autonomous_enabled = buf[4] != 0;
    c->quiet.enabled = buf[5] != 0;
    uint16_t se, ee;
    memcpy(&se, buf + 6, 2);
    memcpy(&ee, buf + 8, 2);
    c->quiet.start_min = se;
    c->quiet.end_min = ee;
    uint32_t scope;
    memcpy(&scope, buf + 10, 4);
    for (int g = 0; g < CACT_GROUPS_MAX; g++) {
        c->quiet.scope[g] = (scope & (1u << g)) != 0;
    }
}

void cact_attach_store(const cact_store_t *st) { s_store = st; }

void cact_restore(cact_ctx_t *c)
{
    if (s_store == NULL || c == NULL) return;
    uint8_t buf[16];
    size_t len = 0;
    int rc = s_store->load(buf, sizeof(buf), &len);
    if (rc == 0 && len == sizeof(buf)) settings_apply(c, buf);
    /* 1=无数据(首次开机)/负=读错 → 保持默认 */
}

int cact_persist(const cact_ctx_t *c)
{
    if (s_store == NULL) return -2;
    uint8_t buf[16];
    settings_pack(c, buf);
    return s_store->save(buf, sizeof(buf));
}

void cact_set_autonomous(cact_ctx_t *c, bool on)
{
    c->autonomous_enabled = on;
    (void)cact_persist(c);   /* T38: 修改即持久 */
}

void cact_set_persona_epoch(cact_ctx_t *c, uint32_t epoch)
{
    c->persona_epoch = epoch;
}

uint32_t cact_persona_epoch(const cact_ctx_t *c)
{
    return c->persona_epoch;
}

cact_res_t cact_user_stop(cact_ctx_t *c, int group_id, const char *reason)
{
    CA_LOCK();
    cact_res_t r = cact_user_stop_locked(c, group_id, reason);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_user_stop_locked(cact_ctx_t *c, int group_id,
                                        const char *reason)
{
    if (group_id <= 0 || group_id >= CACT_GROUPS_MAX) return CACT_RES_INVALID;
    c->group_stopped[group_id] = true;
    c->stop_epoch++;
    /* 取消该组在途动作(含已 ACCEPTED/STARTED) */
    for (int i = 0; i < c->q_len; ) {
        cact_job_t *j = &c->q[i];
        bool in_group = c->caps[j->cap_idx]->group_id == group_id;
        bool in_flight = j->st == CACT_ST_ACCEPTED ||
                         j->st == CACT_ST_STARTED ||
                         j->st == CACT_ST_QUEUED;
        if (in_group && in_flight) {
            (void)cact_cancel_locked(c, j->id,
                                     reason != NULL ? reason : "user_stop");
            continue;                          /* cancel 可能移除条目 */
        }
        i++;
    }
    return CACT_RES_OK;
}

cact_res_t cact_user_resume(cact_ctx_t *c, int group_id)
{
    CA_LOCK();
    cact_res_t r = cact_user_resume_locked(c, group_id);
    CA_UNLOCK();
    return r;
}

static cact_res_t cact_user_resume_locked(cact_ctx_t *c, int group_id)
{
    if (group_id <= 0 || group_id >= CACT_GROUPS_MAX) return CACT_RES_INVALID;
    c->group_stopped[group_id] = false;
    return CACT_RES_OK;
}

bool cact_group_stopped(const cact_ctx_t *c, int group_id)
{
    if (group_id <= 0 || group_id >= CACT_GROUPS_MAX) return false;
    return c->group_stopped[group_id];
}

uint32_t cact_stop_epoch(const cact_ctx_t *c)
{
    return c->stop_epoch;
}

void cact_set_category(cact_ctx_t *c, cact_category_t cat, bool on)
{
    if (cat >= 0 && cat < CACT_CAT_MAX) c->cat_enabled[cat] = on;
}

bool cact_category_enabled(const cact_ctx_t *c, cact_category_t cat)
{
    return (cat >= 0 && cat < CACT_CAT_MAX) ? c->cat_enabled[cat] : false;
}

void cact_quiet_config(cact_ctx_t *c, bool enabled, int start_min, int end_min)
{
    c->quiet.enabled = enabled;
    c->quiet.start_min = start_min;
    c->quiet.end_min = end_min;
    (void)cact_persist(c);   /* T38: 修改即持久 */
}

void cact_quiet_scope(cact_ctx_t *c, int group_id, bool member)
{
    if (group_id > 0 && group_id < CACT_GROUPS_MAX) {
        c->quiet.scope[group_id] = member;
        (void)cact_persist(c);   /* T38: 修改即持久 */
    }
}

void cact_quiet_clock(cact_ctx_t *c, int minute_of_day, bool trusted)
{
    if (minute_of_day < 0) minute_of_day = 0;
    if (minute_of_day > 24 * 60 - 1) minute_of_day = 24 * 60 - 1;
    c->quiet_minute = minute_of_day;
    c->quiet_time_trusted = trusted;
}

bool cact_quiet_active(const cact_ctx_t *c, int group_id)
{
    return quiet_active_for(c, group_id);
}

void cact_boot_gate_open(cact_ctx_t *c, bool open)
{
    c->boot_gate_open = open;
}

bool cact_boot_gate_is_open(const cact_ctx_t *c)
{
    return c->boot_gate_open;
}
