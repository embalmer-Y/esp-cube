/*
 * persona_core.c —— 人格服务骨架实现(见 persona.h)
 */
#include <stdio.h>
#include <string.h>

#include "persona.h"

/* 自包含小拷贝(避免依赖平台 strlcpy; 组件保持纯 C 可宿主直编) */
static size_t psn_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

static bool id_valid(uint32_t id) { return id != 0; }

static int find_idx(const persona_svc_t *s, uint32_t id)
{
    if (!id_valid(id)) return -1;
    for (int i = 0; i < s->n; i++) {
        if (s->slot[i].persona_id == id) return i;
    }
    return -1;
}

psn_res_t persona_init(persona_svc_t *s, uint64_t (*rng)(void))
{
    if (s == NULL || rng == NULL) return PSN_RES_INVALID;
    memset(s, 0, sizeof(*s));
    s->n = 0;
    s->active = -1;
    s->active_epoch = 1;      /* 0 留作"未携带代次"哨兵 */
    s->switching = false;
    s->rng = rng;
    return PSN_RES_OK;
}

static bool name_ok(const char *s_)
{
    return s_ != NULL && s_[0] != '\0' && strlen(s_) < PSN_NAME_MAX;
}

psn_res_t persona_create_draft(persona_svc_t *s, const char *display_name,
                               uint32_t *out_id)
{
    if (s == NULL || out_id == NULL || !name_ok(display_name)) {
        return PSN_RES_INVALID;
    }
    if (s->n >= PSN_LIST_MAX) return PSN_RES_NO_SPACE;
    persona_base_t *b = &s->slot[s->n];
    memset(b, 0, sizeof(*b));
    b->persona_id = (uint32_t)(s->rng() & 0x7FFFFFFFu);
    if (!id_valid(b->persona_id)) b->persona_id = 1;
    b->draft = true;                     /* 新建即草稿, confirm 后生效 */
    b->version = 0;
    psn_strlcpy(b->display_name, display_name, sizeof(b->display_name));
    memset(&s->consent[s->n], 0, sizeof(s->consent[0]));   /* 授权默认双关 */
    memset(&s->evo[s->n], 0, sizeof(s->evo[0]));
    for (int d = 0; d < PSN_DIM_MAX; d++) {
        s->evo[s->n].evo_value[d] = 0.5f;   /* 中性起点 */
    }
    s->n++;
    *out_id = b->persona_id;
    return PSN_RES_OK;
}

const persona_base_t *persona_get(const persona_svc_t *s, uint32_t id)
{
    int i = find_idx(s, id);
    return i >= 0 ? &s->slot[i] : NULL;
}

bool persona_field_locked(const persona_base_t *b, const char *field)
{
    if (b == NULL || field == NULL) return false;
    for (int i = 0; i < b->locked_n; i++) {
        if (strcmp(b->locked[i], field) == 0) return true;
    }
    return false;
}

psn_res_t persona_set_locked(persona_svc_t *s, uint32_t id,
                             const char *field, bool on)
{
    if (s == NULL || field == NULL || strlen(field) >= PSN_LOCKED_NAME_MAX) {
        return PSN_RES_INVALID;
    }
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    persona_base_t *b = &s->slot[i];
    bool has = persona_field_locked(b, field);
    if (on && !has) {
        if (b->locked_n >= PSN_LOCKED_MAX) return PSN_RES_NO_SPACE;
        psn_strlcpy(b->locked[b->locked_n++], field, PSN_LOCKED_NAME_MAX);
    } else if (!on && has) {
        for (int k = 0; k < b->locked_n; k++) {
            if (strcmp(b->locked[k], field) == 0) {
                memmove(&b->locked[k], &b->locked[k + 1],
                        sizeof(b->locked[0]) * (size_t)(b->locked_n - k - 1));
                b->locked_n--;
                break;
            }
        }
    }
    return PSN_RES_OK;
}

/* 字段覆盖 + 锁定检查(fields 中非空字段覆盖草稿; 锁定字段不可变) */
static psn_res_t apply_fields(persona_base_t *draft,
                              const persona_base_t *cur_effective,
                              const persona_base_t *fields)
{
    static const char *f_name = "display_name";
    static const char *f_desc = "description";
    static const char *f_voice = "default_voice";
    if (fields->display_name[0] != '\0') {
        if (cur_effective && persona_field_locked(cur_effective, f_name) &&
            strcmp(fields->display_name, cur_effective->display_name) != 0) {
            return PSN_RES_FORBIDDEN;
        }
        if (!name_ok(fields->display_name)) return PSN_RES_INVALID;
        psn_strlcpy(draft->display_name, fields->display_name,
                   sizeof(draft->display_name));
    }
    if (fields->description[0] != '\0') {
        if (cur_effective && persona_field_locked(cur_effective, f_desc) &&
            strcmp(fields->description, cur_effective->description) != 0) {
            return PSN_RES_FORBIDDEN;
        }
        psn_strlcpy(draft->description, fields->description,
                   sizeof(draft->description));
    }
    if (fields->default_voice[0] != '\0') {
        if (cur_effective && persona_field_locked(cur_effective, f_voice) &&
            strcmp(fields->default_voice, cur_effective->default_voice) != 0) {
            return PSN_RES_FORBIDDEN;
        }
        psn_strlcpy(draft->default_voice, fields->default_voice,
                   sizeof(draft->default_voice));
    }
    return PSN_RES_OK;
}

psn_res_t persona_edit_draft(persona_svc_t *s, uint32_t id,
                             const persona_base_t *fields)
{
    if (s == NULL || fields == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    persona_base_t *b = &s->slot[i];

    persona_base_t tmp = *b;              /* 草稿上试改, 失败不影响原状 */
    /* 锁定基线 = 当前生效版(version>0 时字段取自己; 草稿即派生来源) */
    const persona_base_t *baseline = (b->version > 0) ? b : NULL;
    psn_res_t r = apply_fields(&tmp, baseline, fields);
    if (r != PSN_RES_OK) return r;
    *b = tmp;
    b->draft = true;                      /* 编辑即打回草稿 */
    return PSN_RES_OK;
}

psn_res_t persona_confirm(persona_svc_t *s, uint32_t id)
{
    if (s == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    persona_base_t *b = &s->slot[i];
    if (!b->draft && b->version > 0) return PSN_RES_INVALID;   /* 已生效 */
    b->draft = false;
    b->version++;                         /* 首确认=1, 再确认递增 */
    return PSN_RES_OK;
}

psn_res_t persona_discard(persona_svc_t *s, uint32_t id)
{
    if (s == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    if (!s->slot[i].draft) return PSN_RES_FORBIDDEN;   /* 生效版不可弃 */
    if (i == s->active) return PSN_RES_FORBIDDEN;
    memmove(&s->slot[i], &s->slot[i + 1],
            sizeof(s->slot[0]) * (size_t)(s->n - i - 1));
    memmove(&s->consent[i], &s->consent[i + 1],
            sizeof(s->consent[0]) * (size_t)(s->n - i - 1));
    s->n--;
    if (s->active > i) s->active--;
    return PSN_RES_OK;
}

psn_res_t persona_activate_begin(persona_svc_t *s, uint32_t id,
                                 uint32_t expected_epoch)
{
    if (s == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    if (s->slot[i].draft) return PSN_RES_FORBIDDEN;    /* 草稿不可激活 */
    if (expected_epoch != 0 && expected_epoch != s->active_epoch) {
        return PSN_RES_STALE;
    }
    s->switching = true;
    s->pending_active = i;
    return PSN_RES_OK;
}

psn_res_t persona_activate_commit(persona_svc_t *s)
{
    if (s == NULL || !s->switching) return PSN_RES_INVALID;
    s->active = s->pending_active;
    s->active_epoch++;                    /* 屏障提交: 代次递增点 */
    s->switching = false;
    s->pending_active = -1;
    return PSN_RES_OK;
}

psn_res_t persona_activate_abort(persona_svc_t *s)
{
    if (s == NULL || !s->switching) return PSN_RES_INVALID;
    s->switching = false;
    s->pending_active = -1;
    return PSN_RES_OK;
}

uint32_t persona_epoch(const persona_svc_t *s)
{
    return (s != NULL) ? s->active_epoch : 0;
}

bool persona_epoch_current(const persona_svc_t *s, uint32_t epoch)
{
    return (s != NULL) && epoch == s->active_epoch;
}

bool persona_switching(const persona_svc_t *s)
{
    return (s != NULL) && s->switching;
}

const persona_base_t *persona_active(const persona_svc_t *s)
{
    return (s != NULL && s->active >= 0 && s->active < s->n)
               ? &s->slot[s->active] : NULL;
}

uint32_t persona_active_id(const persona_svc_t *s)
{
    const persona_base_t *b = persona_active(s);
    return b ? b->persona_id : 0;
}

psn_res_t persona_consent_set(persona_svc_t *s, uint32_t id,
                              bool memory, bool evolution)
{
    if (s == NULL) return PSN_RES_INVALID;
    int i = find_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    s->consent[i].memory_enabled = memory;
    s->consent[i].evolution_enabled = evolution;
    s->consent[i].consent_epoch++;        /* 每次授权变更递增 */
    return PSN_RES_OK;
}

bool persona_consent_memory(const persona_svc_t *s, uint32_t id)
{
    int i = find_idx(s, id);
    return i >= 0 ? s->consent[i].memory_enabled : false;
}
bool persona_consent_evolution(const persona_svc_t *s, uint32_t id)
{
    int i = find_idx(s, id);
    return i >= 0 ? s->consent[i].evolution_enabled : false;
}
uint32_t persona_consent_epoch(const persona_svc_t *s, uint32_t id)
{
    int i = find_idx(s, id);
    return i >= 0 ? s->consent[i].consent_epoch : 0;
}


/* ================= 持久化(T20; store_slot 双槽落地) ================= */
#pragma pack(push, 1)
#pragma pack(push, 1)
typedef struct {
    uint32_t revision;
    uint32_t persona_id;
    uint8_t  dim;
    float    old_value, new_value, delta_applied;
    char     reason[PSN_EVO_REASON_MAX];
    char     evidence[PSN_EVO_EVID_MAX][PSN_EVO_EVID_LEN];
    uint8_t  evidence_n;
    char     algo_ver[PSN_EVO_ALGO_MAX];
} persona_rec_rev_t;
#pragma pack(pop)

typedef struct {
    uint32_t magic;        /* 'PSR1' */
    uint16_t ver;
    uint8_t  n;
    int8_t   active_idx;
    uint32_t last_epoch;   /* 恢复后 active_epoch = last_epoch + 1(单调) */
} persona_hdr_t;
typedef struct {
    uint32_t id;
    uint16_t version;
    uint8_t  draft;
    char     name[PSN_NAME_MAX];
    char     desc[PSN_DESC_MAX];
    char     voice[PSN_VOICE_MAX];
    uint8_t  locked_n;
    char     locked[PSN_LOCKED_MAX][PSN_LOCKED_NAME_MAX];
    uint8_t  mem_en, evo_en;
    uint32_t consent_epoch;
    float    evo_value[PSN_DIM_MAX];
    uint8_t  evo_paused;
    uint8_t  rev_n;
    persona_rec_rev_t revs[PSN_REVISIONS_MAX];   /* 按最新→最旧存 */
} persona_rec_t;
#pragma pack(pop)

_Static_assert(sizeof(persona_hdr_t) == 12, "hdr 12B");
_Static_assert(PSN_LIST_MAX * sizeof(persona_rec_t) + 16 < 8000,
               "8 角色载荷(含演化)应小于 8KB scratch(含槽头冗余)");

#define PSN_STORE_MAGIC 0x50535231u   /* 'PSR1' */
#define PSN_STORE_VER   2u   /* v2: +演化字段(T24); 旧版本作废重建(T22 用户裁决) */

psn_res_t persona_store_bind(persona_svc_t *s, const store_io_t *io,
                             uint8_t *scratch, uint32_t scratch_cap)
{
    if (s == NULL || io == NULL || scratch == NULL ||
        scratch_cap < 4096) {
        return PSN_RES_INVALID;
    }
    s->store = io;
    s->scratch = scratch;
    s->scratch_cap = scratch_cap;
    /* bind 即探测库状态, ctx 由 load/save 共用(RECOVERY_REQUIRED 状态
     * 保存在 ctx.status, save 时由 commit 拒绝 —— fail closed) */
    if (store_slot_init(&s->store_ctx, io, PSN_STORE_MAX_PAYLOAD, scratch,
                        scratch_cap) == STORE_STATUS_INVALID) {
        s->store = NULL;
        return PSN_RES_INVALID;   /* scratch 不足等配置错误 */
    }
    return PSN_RES_OK;
}

psn_res_t persona_load(persona_svc_t *s)
{
    if (s == NULL || s->store == NULL) return PSN_RES_INVALID;
    if (s->store_ctx.status == STORE_STATUS_EMPTY) {
        return PSN_RES_OK;                        /* 全新库 */
    }
    if (s->store_ctx.status != STORE_STATUS_OK) {
        return PSN_RES_IO_ERROR;    /* RECOVERY_REQUIRED: 停写保护(docs/17 §8.2) */
    }
    size_t len = 0;
    /* buf=NULL: 只校验, 随后经 store_slot_payload() 原地解析(免拷贝) */
    store_status_t rr = store_slot_read(&s->store_ctx, NULL, 0, &len);
    if (rr != STORE_STATUS_OK) {
        return PSN_RES_IO_ERROR;
    }
    if (len < sizeof(persona_hdr_t)) return PSN_RES_IO_ERROR;
    persona_hdr_t h;
    memcpy(&h, store_slot_payload(&s->store_ctx), sizeof(h));  /* payload 区头 */
    if (h.magic != PSN_STORE_MAGIC) return PSN_RES_IO_ERROR;
    if (h.ver != PSN_STORE_VER) {
        /* 旧版本注册表: T22 用户裁决(测试资料不保留)→作废, 按空库重建 */
        return PSN_RES_OK;
    }
    if (h.n > PSN_LIST_MAX) return PSN_RES_IO_ERROR;
    /* 静态: load 仅启动/SD-ready 一次(单 owner); 3KB 栈数组曾压破
     * SD mount 任务栈(板上 T20 实证) */
    static persona_rec_t recs[PSN_LIST_MAX];
    if (len != sizeof(h) + sizeof(persona_rec_t) * h.n) {
        return PSN_RES_IO_ERROR;
    }
    const persona_rec_t *disk = (const persona_rec_t *)
        ((const uint8_t *)store_slot_payload(&s->store_ctx) + sizeof(h));
    memcpy(recs, disk, sizeof(persona_rec_t) * h.n);

    for (int i = 0; i < h.n; i++) {
        persona_base_t *b = &s->slot[i];
        memset(b, 0, sizeof(*b));
        b->persona_id = recs[i].id;
        b->version = recs[i].version;
        b->draft = recs[i].draft != 0;
        memcpy(b->display_name, recs[i].name, sizeof(b->display_name));
        memcpy(b->description, recs[i].desc, sizeof(b->description));
        memcpy(b->default_voice, recs[i].voice, sizeof(b->default_voice));
        b->locked_n = recs[i].locked_n > PSN_LOCKED_MAX
                          ? PSN_LOCKED_MAX : recs[i].locked_n;
        memcpy(b->locked, recs[i].locked, sizeof(b->locked));
        s->consent[i].memory_enabled = recs[i].mem_en != 0;
        s->consent[i].evolution_enabled = recs[i].evo_en != 0;
        s->consent[i].consent_epoch = recs[i].consent_epoch;
        memcpy(s->evo[i].evo_value, recs[i].evo_value,
               sizeof(s->evo[i].evo_value));
        s->evo[i].evolution_paused = recs[i].evo_paused != 0;
        s->evo[i].rev_n = recs[i].rev_n > PSN_REVISIONS_MAX
                              ? PSN_REVISIONS_MAX : recs[i].rev_n;
        s->evo[i].rev_head = s->evo[i].rev_n;   /* 顺序队列: head=rev_n */
        for (int k = 0; k < s->evo[i].rev_n; k++) {
            /* 逐字段转换; 存序最新在前 → 队列最旧在前(反转), 使
             * persona_evo_revision(idx=0)=队列尾=最新 */
            psn_evo_rev_t *d = &s->evo[i].revs[s->evo[i].rev_n - 1 - k];
            const persona_rec_rev_t *src = &recs[i].revs[k];
            d->revision = src->revision;
            d->persona_id = src->persona_id;
            d->dim = src->dim;
            d->old_value = src->old_value;
            d->new_value = src->new_value;
            d->delta_applied = src->delta_applied;
            memcpy(d->reason, src->reason, sizeof(d->reason));
            memcpy(d->evidence, src->evidence, sizeof(d->evidence));
            d->evidence_n = src->evidence_n;
            memcpy(d->algo_ver, src->algo_ver, sizeof(d->algo_ver));
        }
        /* 重排后 head=rev_n: persona_evo_revision(idx=0) 回看 revs[rev_n-1]
         * —— 与"存序=最新在前"配对即最新(revs 存序最旧在前, 上一行存入
         * 顺序按 idx 最新→最旧拷贝, 故 revs[0]=最新; 修正 head 使回看
         * 从 revs[rev_n-1]=最旧开始不成立 —— 此处直接改为顺序队列:
         * head 从 0 起按最旧→最新排, 回看 idx0=head-1=最新) */
    }
    s->n = h.n;
    s->active = (h.active_idx >= 0 && h.active_idx < h.n &&
                 !s->slot[h.active_idx].draft) ? h.active_idx : -1;
    /* docs/17 §3.1: 重启后旧代次失效 —— epoch 取持久值+1(单调) */
    s->active_epoch = h.last_epoch + 1;
    return PSN_RES_OK;
}

psn_res_t persona_save(persona_svc_t *s)
{
    if (s == NULL || s->store == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    persona_hdr_t h = {
        .magic = PSN_STORE_MAGIC, .ver = PSN_STORE_VER,
        .n = (uint8_t)s->n,
        .active_idx = (int8_t)s->active,
        .last_epoch = s->active_epoch,
    };
    static persona_rec_t recs[PSN_LIST_MAX];   /* 单 owner 串行(同上) */
    memset(recs, 0, sizeof(recs));
    for (int i = 0; i < s->n; i++) {
        persona_base_t *b = &s->slot[i];
        recs[i].id = b->persona_id;
        recs[i].version = b->version;
        recs[i].draft = b->draft ? 1 : 0;
        memcpy(recs[i].name, b->display_name, sizeof(recs[i].name));
        memcpy(recs[i].desc, b->description, sizeof(recs[i].desc));
        memcpy(recs[i].voice, b->default_voice, sizeof(recs[i].voice));
        recs[i].locked_n = (uint8_t)b->locked_n;
        memcpy(recs[i].locked, b->locked, sizeof(recs[i].locked));
        recs[i].mem_en = s->consent[i].memory_enabled ? 1 : 0;
        recs[i].evo_en = s->consent[i].evolution_enabled ? 1 : 0;
        recs[i].consent_epoch = s->consent[i].consent_epoch;
        memcpy(recs[i].evo_value, s->evo[i].evo_value,
               sizeof(recs[i].evo_value));
        recs[i].evo_paused = s->evo[i].evolution_paused ? 1 : 0;
        recs[i].rev_n = (uint8_t)s->evo[i].rev_n;
        for (int k = 0; k < s->evo[i].rev_n && k < PSN_REVISIONS_MAX; k++) {
            const psn_evo_rev_t *r = persona_evo_revision(s,
                                                          s->slot[i].persona_id,
                                                          k);
            persona_rec_rev_t *d = &recs[i].revs[k];
            memset(d, 0, sizeof(*d));
            d->revision = r->revision;
            d->persona_id = r->persona_id;
            d->dim = r->dim;
            d->old_value = r->old_value;
            d->new_value = r->new_value;
            d->delta_applied = r->delta_applied;
            memcpy(d->reason, r->reason, sizeof(d->reason));
            for (int e = 0; e < r->evidence_n; e++) {
                memcpy(d->evidence[e], r->evidence[e],
                       sizeof(d->evidence[e]));
            }
            d->evidence_n = r->evidence_n;
            memcpy(d->algo_ver, r->algo_ver, sizeof(d->algo_ver));
        }
    }
    /* 两段式: head=hdr(栈), body=recs(静态单 owner); 免 scratch 中转 */
    static persona_rec_t recs_buf[PSN_LIST_MAX];
    memcpy(recs_buf, recs, sizeof(persona_rec_t) * (size_t)s->n);
    uint32_t gen = 0;
    store_status_t r = store_slot_commit_parts(&s->store_ctx, &h, sizeof(h),
                                               recs_buf,
                                               sizeof(persona_rec_t) *
                                                   (size_t)s->n, &gen);
    return r == STORE_STATUS_OK ? PSN_RES_OK : PSN_RES_IO_ERROR;
}


/* ================= 受控演化(T24; docs/17 §7.4) ================= */
static int evo_idx(const persona_svc_t *s, uint32_t id)
{
    return find_idx(s, id);
}

psn_res_t persona_evo_propose(persona_svc_t *s, uint32_t id, psn_dim_t dim,
                              float delta, const char *reason,
                              const char *const *evidence, int evidence_n,
                              const char *algo_ver)
{
    if (s == NULL || reason == NULL || reason[0] == '\0' ||
        algo_ver == NULL || algo_ver[0] == '\0' || dim >= PSN_DIM_MAX) {
        return PSN_RES_INVALID;
    }
    if (evidence == NULL || evidence_n < PSN_EVO_MIN_EVID) {
        return PSN_RES_INVALID;             /* 无证据不演化 */
    }
    if (delta != delta || delta > PSN_EVO_MAX_DELTA ||
        delta < -PSN_EVO_MAX_DELTA || delta == 0.0f) {
        return PSN_RES_INVALID;             /* NaN/超限幅/零 */
    }
    int i = evo_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    if (s->switching) return PSN_RES_BUSY;
    if (!s->consent[i].evolution_enabled) {
        return PSN_RES_FORBIDDEN_LOCKED;    /* 授权关(docs/16 FR-30) */
    }
    if (s->evo[i].evolution_paused) {
        return PSN_RES_FORBIDDEN_LOCKED;    /* 用户暂停 */
    }
    float old_v = s->evo[i].evo_value[dim];
    float new_v = old_v + delta;
    if (new_v > 1.0f) new_v = 1.0f;
    if (new_v < 0.0f) new_v = 0.0f;
    float applied = new_v - old_v;
    if (applied == 0.0f) return PSN_RES_INVALID;   /* 纯钳制: 无有效变化 */

    s->evo[i].evo_value[dim] = new_v;
    psn_evo_rev_t *r = &s->evo[i].revs[s->evo[i].rev_head];
    memset(r, 0, sizeof(*r));
    r->revision = ++s->evo_revision;
    r->persona_id = id;
    r->dim = (uint8_t)dim;
    r->old_value = old_v;
    r->new_value = new_v;
    r->delta_applied = applied;
    psn_strlcpy(r->reason, reason, sizeof(r->reason));
    for (int k = 0; k < evidence_n && k < PSN_EVO_EVID_MAX; k++) {
        psn_strlcpy(r->evidence[k], evidence[k], sizeof(r->evidence[k]));
        r->evidence_n++;
    }
    psn_strlcpy(r->algo_ver, algo_ver, sizeof(r->algo_ver));
    s->evo[i].rev_head = (s->evo[i].rev_head + 1) % PSN_REVISIONS_MAX;
    if (s->evo[i].rev_n < PSN_REVISIONS_MAX) s->evo[i].rev_n++;
    return PSN_RES_OK;
}

psn_res_t persona_evo_pause(persona_svc_t *s, uint32_t id, bool paused)
{
    int i = evo_idx(s, id);
    if (s == NULL || i < 0) return PSN_RES_NOT_FOUND;
    s->evo[i].evolution_paused = paused;
    return PSN_RES_OK;
}

bool persona_evo_paused(const persona_svc_t *s, uint32_t id)
{
    int i = evo_idx(s, id);
    return i >= 0 ? s->evo[i].evolution_paused : false;
}

float persona_evo_value(const persona_svc_t *s, uint32_t id, psn_dim_t dim)
{
    int i = evo_idx(s, id);
    return (i >= 0 && dim < PSN_DIM_MAX) ? s->evo[i].evo_value[dim] : -1.0f;
}

int persona_evo_revision_count(const persona_svc_t *s, uint32_t id)
{
    int i = evo_idx(s, id);
    return i >= 0 ? s->evo[i].rev_n : 0;
}

const psn_evo_rev_t *persona_evo_revision(const persona_svc_t *s,
                                          uint32_t id, int idx)
{
    int i = evo_idx(s, id);
    if (i < 0 || idx < 0 || idx >= s->evo[i].rev_n) return NULL;
    /* idx 0=最新: 环形从 head 往回走 */
    int slot = (s->evo[i].rev_head - 1 - idx + PSN_REVISIONS_MAX * 2) %
               PSN_REVISIONS_MAX;
    return &s->evo[i].revs[slot];
}

psn_res_t persona_evo_rollback(persona_svc_t *s, uint32_t id,
                               uint32_t revision)
{
    if (s == NULL) return PSN_RES_INVALID;
    if (s->switching) return PSN_RES_BUSY;
    int i = evo_idx(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    if (!s->consent[i].evolution_enabled || s->evo[i].evolution_paused) {
        return PSN_RES_FORBIDDEN_LOCKED;
    }
    const psn_evo_rev_t *target = NULL;
    for (int k = 0; k < s->evo[i].rev_n; k++) {
        const psn_evo_rev_t *r = persona_evo_revision(s, id, k);
        if (r->revision == revision) { target = r; break; }
    }
    if (target == NULL) return PSN_RES_NOT_FOUND;   /* 已滚出保留窗 */
    float cur = s->evo[i].evo_value[target->dim];
    if (cur == target->old_value) return PSN_RES_STALE;  /* 已在该值 */
    /* 回滚 = 新审计修订(值恢复; 不抹历史) */
    psn_evo_rev_t *r = &s->evo[i].revs[s->evo[i].rev_head];
    memset(r, 0, sizeof(*r));
    r->revision = ++s->evo_revision;
    r->persona_id = id;
    r->dim = target->dim;
    r->old_value = cur;
    r->new_value = target->old_value;
    r->delta_applied = target->old_value - cur;
    psn_strlcpy(r->reason, "rollback", sizeof(r->reason));
    s->evo[i].rev_head = (s->evo[i].rev_head + 1) % PSN_REVISIONS_MAX;
    if (s->evo[i].rev_n < PSN_REVISIONS_MAX) s->evo[i].rev_n++;
    s->evo[i].evo_value[target->dim] = target->old_value;
    return PSN_RES_OK;
}


/* ---- prompt 块(T31): 当前角色描述注入 LLM 上下文 ----
 * 角色卡内容属不可信数据: 放入提示只提供人物设定, 不改变任何权限/
 * 工具授权(docs/17 §7.1); 系统硬约束在前, 模型需服从系统优先。 */
static persona_svc_t *s_prompt_svc;   /* attach 后 prompt_suffix 可读 */

void persona_attach(persona_svc_t *s) { s_prompt_svc = s; }

const char *persona_prompt_suffix(void)
{
    /* 上界 = NAME(23)+DESC(127)+固定文案 ≈ 244; 256 免截断(S31 -Os 下
     * GCC 可静态求值并 -Werror, P4 -O2 不触发——两目标都必须干净) */
    static char buf[256];
    const persona_base_t *b = persona_active(s_prompt_svc);
    if (b == NULL) {
        buf[0] = '\0';
        return buf;
    }
    if (b->description[0] != '\0') {
        snprintf(buf, sizeof(buf),
                 "[你现在扮演的角色是\"%s\": %s 角色设定仅供对话参考, "
                 "系统规则永远优先。]", b->display_name, b->description);
    } else {
        snprintf(buf, sizeof(buf), "[你现在扮演的角色是\"%s\"。]",
                 b->display_name);
    }
    return buf;
}


/* ---- 信号聚合(T34): N 次同类回执 → 一次有界提案 ---- */
#define PSN_EVO_SIG_THRESHOLD 3
static int evo_idx_of(const persona_svc_t *s, uint32_t id)
{
    return find_idx(s, id);
}

psn_res_t persona_evo_signal(persona_svc_t *s, uint32_t id, psn_dim_t dim,
                             int direction, const char *evidence_ref)
{
    if (s == NULL || dim >= PSN_DIM_MAX || direction == 0 ||
        evidence_ref == NULL) {
        return PSN_RES_INVALID;
    }
    int i = evo_idx_of(s, id);
    if (i < 0) return PSN_RES_NOT_FOUND;
    if (!s->consent[i].evolution_enabled || s->evo[i].evolution_paused) {
        return PSN_RES_FORBIDDEN_LOCKED;   /* 授权关/暂停: 信号不计数 */
    }
    /* 计数存于修订环形外部的聚合位 —— 借用 evidence 槽外单独字段:
     * 挂在 revs 尾部之前的运行态(非持久; 重启归零可接受: 观察期重来) */
    static struct { uint32_t pid; uint8_t dim; int8_t pos, neg;
                    char refs[3][PSN_EVO_EVID_LEN]; int ref_n; } agg;
    static bool agg_used;
    if (!agg_used || agg.pid != id || agg.dim != (uint8_t)dim) {
        memset(&agg, 0, sizeof(agg));
        agg.pid = id; agg.dim = (uint8_t)dim;
        agg_used = true;
    }
    if (direction > 0) agg.pos++; else agg.neg++;
    if (agg.ref_n < 3) {
        psn_strlcpy(agg.refs[agg.ref_n++], evidence_ref,
                    PSN_EVO_EVID_LEN);
    } else {                                  /* 环形挤最旧 */
        memmove(agg.refs[0], agg.refs[1], sizeof(agg.refs[0]) * 2);
        psn_strlcpy(agg.refs[2], evidence_ref, PSN_EVO_EVID_LEN);
    }
    if (agg.pos >= PSN_EVO_SIG_THRESHOLD &&
        s->evo[i].evo_value[dim] < 1.0f) {
        const char *evs[3] = { agg.refs[0], agg.refs[1], agg.refs[2] };
        psn_res_t r = persona_evo_propose(s, id, dim, 0.03f,
                                          "聚合播放正向信号", evs, agg.ref_n,
                                          "aggr-1");
        memset(&agg, 0, sizeof(agg));        /* 提案后重新观察 */
        agg.pid = id; agg.dim = (uint8_t)dim; agg_used = true;
        return r;
    }
    if (agg.neg >= PSN_EVO_SIG_THRESHOLD &&
        s->evo[i].evo_value[dim] > 0.0f) {
        const char *evs[3] = { agg.refs[0], agg.refs[1], agg.refs[2] };
        psn_res_t r = persona_evo_propose(s, id, dim, -0.03f,
                                          "聚合用户停止信号", evs, agg.ref_n,
                                          "aggr-1");
        memset(&agg, 0, sizeof(agg));
        agg.pid = id; agg.dim = (uint8_t)dim; agg_used = true;
        return r;
    }
    return PSN_RES_OK;                        /* 观察中 */
}
