/*
 * persona.h —— 人格服务骨架(docs/17 §7.1/§7.3; docs/18 T19, S3 纯 C)
 *
 * 三类数据严格分账(docs/17 §7.1): 底座设定(PersonaBase) ≠ 演化 ≠
 * 记忆 —— 本组件只管底座 + 代次 + 授权 + 切换屏障; 演化/记忆归
 * T23/T24 域, 通过本组件的 id/epoch/consent 检查点接入。
 *
 * 不变量(docs/18 T19 验收):
 *   1. persona_id 由注入随机源生成, 改名/编辑不变 —— 数据域锚点。
 *   2. AI 生成/用户编辑都是草稿(draft=true), 用户 confirm 才生效;
 *      生效版不可原地改, 编辑自动走草稿副本。
 *   3. 用户锁定字段(locked)不可被编辑/确认覆盖(FORBIDDEN)。
 *   4. 切换走屏障(begin/commit/abort): commit 时 active_epoch++,
 *      晚到结果用 persona_epoch_current() 复核即失效。
 *   5. 授权(consent)默认双关(记忆/演化都要用户同意, docs/16 FR-25);
 *      每次变更 consent_epoch++。
 * 纯 C 零 OS 依赖; 时间无关(代次计数, 不用墙钟); 并发由上层 owner
 * 串行化(PersonaService 单 owner, docs/17 §15.4)。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store_slot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSN_NAME_MAX        24
#define PSN_DESC_MAX        128
#define PSN_VOICE_MAX       24
#define PSN_LOCKED_MAX      8
#define PSN_LOCKED_NAME_MAX 24
#define PSN_LIST_MAX        8     /* 角色上限(首版工作初值) */
#define PSN_DIM_MAX         4     /* 演化维度(docs/17 §7.4) */
#define PSN_EVO_MAX_DELTA   0.05f /* 单提案限幅(工作初值, 离线回归校准) */
#define PSN_EVO_MIN_EVID    1     /* 最小证据数(工作初值) */
#define PSN_REVISIONS_MAX   4     /* 每角色保留的演化修订数(环形) */
#define PSN_EVO_REASON_MAX  32
#define PSN_EVO_EVID_MAX    2
#define PSN_EVO_EVID_LEN    16
#define PSN_EVO_ALGO_MAX    8

/* 演化维度(docs/16 FR-29 首版四类; 维度注册表可扩展) */
typedef enum {
    PSN_DIM_SPEECH = 0,       /* 语言表现风格 */
    PSN_DIM_ACTION,           /* 动作/表情偏好 */
    PSN_DIM_INTEREST,         /* 伙伴话题/音乐兴趣 */
    PSN_DIM_RHYTHM,           /* 互动节奏 */
} psn_dim_t;   /* 上限用宏 PSN_DIM_MAX(避免宏/枚举同名冲突) */

typedef enum {
    PSN_RES_OK = 0,
    PSN_RES_INVALID,          /* 参数/状态非法 */
    PSN_RES_NO_SPACE,         /* 角色数达上限 */
    PSN_RES_NOT_FOUND,        /* id 不存在 */
    PSN_RES_STALE,            /* expected_epoch 不符(晚到/并发) */
    PSN_RES_FORBIDDEN,        /* 锁定字段/激活草稿/删激活角色 */
    PSN_RES_BUSY,             /* 屏障中(switching)不接受新操作 */
    PSN_RES_IO_ERROR,         /* 持久层故障/RECOVERY_REQUIRED(停写保护) */
    PSN_RES_FORBIDDEN_LOCKED, /* 演化: 授权关/暂停(与 FORBIDDEN 分码便于审计) */
} psn_res_t;

/* 演化修订(T24; docs/17 §7.4): 一次有界变化 = 一条可审计记录 */
typedef struct {
    uint32_t revision;        /* 全 svc 递增 */
    uint32_t persona_id;
    uint8_t  dim;             /* psn_dim_t */
    float    old_value, new_value;   /* [0,1] */
    float    delta_applied;   /* 实际应用量(钳制后) */
    char     reason[PSN_EVO_REASON_MAX];
    char     evidence[PSN_EVO_EVID_MAX][PSN_EVO_EVID_LEN];
    uint8_t  evidence_n;
    char     algo_ver[PSN_EVO_ALGO_MAX];
} psn_evo_rev_t;

typedef struct {
    uint32_t persona_id;      /* 稳定随机; 0=无效; 改名不变 */
    uint16_t version;         /* 确认生效次数; 0=从未生效(草稿) */
    bool draft;               /* true=未生效草稿 */
    char display_name[PSN_NAME_MAX];
    char description[PSN_DESC_MAX];
    char default_voice[PSN_VOICE_MAX];
    char locked[PSN_LOCKED_MAX][PSN_LOCKED_NAME_MAX]; /* 锁定字段名 */
    int locked_n;
} persona_base_t;

typedef struct {
    persona_base_t slot[PSN_LIST_MAX];
    int n;
    int active;               /* 激活下标; -1=无激活角色 */
    uint32_t active_epoch;    /* 每次 commit 递增(晚到结果复核键) */
    bool switching;           /* 切换屏障中 */
    int pending_active;       /* 屏障目标下标 */
    struct {
        bool memory_enabled;      /* 默认 false(需用户同意) */
        bool evolution_enabled;   /* 默认 false */
        uint32_t consent_epoch;   /* 每次授权变更递增 */
    } consent[PSN_LIST_MAX];
    /* 演化(per persona; T24) */
    struct {
        float evo_value[PSN_DIM_MAX];       /* 默认 0.5 */
        bool  evolution_paused;             /* 暂停≠关闭授权(FR-30) */
        psn_evo_rev_t revs[PSN_REVISIONS_MAX];
        int   rev_n, rev_head;
    } evo[PSN_LIST_MAX];
    uint32_t evo_revision;    /* 全 svc 递增修订号 */
    uint64_t (*rng)(void);    /* 注入随机源(persona_id 生成) */
    const store_io_t *store;  /* 持久化 io(T20; bind 后非空) */
    uint8_t *scratch;
    uint32_t scratch_cap;
    store_slot_t store_ctx;   /* 双槽上下文(save/load 内部维护) */
} persona_svc_t;

/* 初始化; rng 必填(固件侧传 esp_random 包装, 宿主传固定序列) */
psn_res_t persona_init(persona_svc_t *s, uint64_t (*rng)(void));

/* ---- 草稿与生效(docs/16 FR-22/FR-23) ---- */
psn_res_t persona_create_draft(persona_svc_t *s, const char *display_name,
                               uint32_t *out_id);
/* 编辑草稿(新建空草稿或生效版的草稿副本); 锁定字段以草稿来源的
 * 生效版为准, 改动即 FORBIDDEN */
psn_res_t persona_edit_draft(persona_svc_t *s, uint32_t id,
                             const persona_base_t *fields);
psn_res_t persona_confirm(persona_svc_t *s, uint32_t id);   /* 草稿→生效 */
psn_res_t persona_discard(persona_svc_t *s, uint32_t id);   /* 仅草稿可弃 */
psn_res_t persona_set_locked(persona_svc_t *s, uint32_t id,
                             const char *field, bool on);

/* ---- 切换屏障(docs/17 §7.3) ----
 * begin: 校验 expected_epoch(0=不校验) → switching=true;
 * 调用方在屏障内取消旧会话/清缓存(用 epoch 复核);
 * commit: active=pending, active_epoch++, 屏障解除;
 * abort: 放弃切换, 状态不变。 */
psn_res_t persona_activate_begin(persona_svc_t *s, uint32_t id,
                                 uint32_t expected_epoch);
psn_res_t persona_activate_commit(persona_svc_t *s);
psn_res_t persona_activate_abort(persona_svc_t *s);

/* ---- 查询 ---- */
uint32_t persona_epoch(const persona_svc_t *s);
bool persona_epoch_current(const persona_svc_t *s, uint32_t epoch);
bool persona_switching(const persona_svc_t *s);
const persona_base_t *persona_get(const persona_svc_t *s, uint32_t id);
const persona_base_t *persona_active(const persona_svc_t *s);
uint32_t persona_active_id(const persona_svc_t *s);
/* 当前角色 prompt 块(T31): "[你正扮演...]" 形式, 不可信数据(权限不由
 * 提示担保, docs/17 §7.1); 无激活角色返回空串。返回静态缓冲, 调用方
 * 立即拼接。 */
const char *persona_prompt_suffix(void);
void persona_attach(persona_svc_t *s);   /* prompt 读当前激活角色用 */
bool persona_field_locked(const persona_base_t *b, const char *field);

/* ---- 授权(docs/16 FR-25/FR-30; 变更即 consent_epoch++) ---- */
/* ---- 持久化(T20; docs/17 §8): 底座+授权经 store_slot 双槽落地 ----
 * io 由装配层提供(SD 文件, 槽 a/b); scratch ≥ 载荷+STORE_HDR_SIZE
 * (8 角色 ≈ 3.1KB → 4KB)。load 后 active_epoch = 持久值+1 —— 单调
 * 递增, 重启前的旧代次(含缓存/在途结果)天然失效, 与 boot_id 联合
 * 判定等价(docs/17 §3.1)。load 失败(RECOVERY_REQUIRED)→运行态空,
 * 不写(保护现场, docs/17 §8.2)。 */
#define PSN_STORE_MAX_PAYLOAD 8000u  /* 8 角色载荷(含演化) ~6.3KB; scratch 需 +24 */

psn_res_t persona_store_bind(persona_svc_t *s, const store_io_t *io,
                             uint8_t *scratch, uint32_t scratch_cap);
psn_res_t persona_load(persona_svc_t *s);   /* 恢复; epoch=持久值+1 */
psn_res_t persona_save(persona_svc_t *s);   /* 提交新代; 屏障中拒 */

/* ---- 受控演化(T24; docs/17 §7.4, docs/16 FR-29/FR-30) ----
 * 有界提案: |delta| ≤ 0.05, 结果钳制 [0,1], 证据 ≥1 条; 即时应用并
 * 记录审计修订(old/new/evidence/algo_ver/epoch)。门: 授权开 且 未暂停。
 * 回滚恢复指定修订前的值并留审计; 在途"基于旧值"的上层缓存以
 * persona_epoch+最新 revision 复核。安全/隐私/用户锁定字段不在维度
 * 空间内 —— 结构上不可被演化触及。 */
psn_res_t persona_evo_propose(persona_svc_t *s, uint32_t id, psn_dim_t dim,
                              float delta, const char *reason,
                              const char *const *evidence, int evidence_n,
                              const char *algo_ver);
psn_res_t persona_evo_pause(persona_svc_t *s, uint32_t id, bool paused);
bool      persona_evo_paused(const persona_svc_t *s, uint32_t id);
float     persona_evo_value(const persona_svc_t *s, uint32_t id,
                            psn_dim_t dim);
int       persona_evo_revision_count(const persona_svc_t *s, uint32_t id);
const psn_evo_rev_t *persona_evo_revision(const persona_svc_t *s,
                                          uint32_t id, int idx);  /* 0=最新 */
psn_res_t persona_evo_rollback(persona_svc_t *s, uint32_t id,
                               uint32_t revision);   /* 恢复该修订前值 */

/* 信号聚合(T34): 可追溯执行回执 → 计数; 达阈值(±3)自动提案一次
 * (±0.03, 证据=最近信号 refs)。授权关/暂停 → 信号丢弃(计数不增)。 */
psn_res_t persona_evo_signal(persona_svc_t *s, uint32_t id, psn_dim_t dim,
                             int direction, const char *evidence_ref);

psn_res_t persona_consent_set(persona_svc_t *s, uint32_t id,
                              bool memory, bool evolution);
bool persona_consent_memory(const persona_svc_t *s, uint32_t id);
bool persona_consent_evolution(const persona_svc_t *s, uint32_t id);
uint32_t persona_consent_epoch(const persona_svc_t *s, uint32_t id);

#ifdef __cplusplus
}
#endif
