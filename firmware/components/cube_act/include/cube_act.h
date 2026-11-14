/*
 * cube_act.h —— 统一动作契约(S0 基础契约; docs/17 §5.1/§5.2/§15.5)
 *
 * 目标: 用户/App/触摸/会话工具/LLM 自主/系统请求共用同一份
 * 能力注册、状态机、回执与拒绝语义; 入队不等于开始, 开始不等于完成。
 *
 * 纪律(docs/17 §3.1/§5.3):
 *   - origin 由本机受信输入适配器判定后传入; 网关/模型/外部报文不得
 *     直接指定 origin/优先级/风险 —— 这些字段在本层视为内部事实。
 *   - 风险等级不能被模型降低; SENSITIVE 必须显式确认才进入队列。
 *   - 每次状态变化生成带序号回执; 终态不可再迁移。
 *   - 纯 C 无 OS 依赖: 时间由调用方注入(now_ms), 并发由上层 owner
 *     任务串行化(骨架单线程语义; docs/17 §15.5 线程预算)。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量常量(骨架工作初值; 板上按 RAM 实测调整) ---- */
typedef uint32_t cact_id_t;   /* 动作 id(0 无效, 1 起) */

#define CACT_CAPS_MAX      12     /* 能力注册上限(现技能 8 个) */
#define CACT_QUEUE_MAX     8      /* 有界动作队列 */
#define CACT_IDEM_MAX      16     /* 幂等记录保留条数 */
#define CACT_RECEIPTS_MAX  16     /* 回执环形保留条数 */
#define CACT_GROUPS_MAX    8      /* 资源组(停止锁存粒度) */
#define CACT_CAP_ID_MAX    24     /* 能力 id 长度(含 NUL) */
#define CACT_REASON_MAX    32     /* 回执原因长度(含 NUL) */
#define CACT_PARAMS_MAX    256    /* 参数内联上限 */

/* ---- 来源(本机判定; docs/17 §3.1) ---- */
typedef enum {
    CACT_ORIGIN_UNKNOWN = 0,
    CACT_ORIGIN_PHYSICAL_USER,      /* 触摸/按键: 受信本机输入 */
    CACT_ORIGIN_PAIRED_APP,         /* 已认证 App(网关映射) */
    CACT_ORIGIN_VOICE_UNVERIFIED,   /* 语音意图可信, 身份未证 */
    CACT_ORIGIN_MODEL_TOOL,         /* 会话工具轮(模型提议) */
    CACT_ORIGIN_AUTONOMOUS_LOCAL,   /* 本地自主候选 */
    CACT_ORIGIN_AUTONOMOUS_LLM,     /* 慢思考提议 */
    CACT_ORIGIN_SYSTEM_SAFETY,      /* 系统安全/故障保护 */
    CACT_ORIGIN_DEBUG,              /* CLI/台调 */
} cact_origin_t;

static inline bool cact_origin_is_autonomous(cact_origin_t o)
{
    return o == CACT_ORIGIN_AUTONOMOUS_LOCAL ||
           o == CACT_ORIGIN_AUTONOMOUS_LLM;
}

/* ---- 行为分类(docs/16 FR-14: 总开关+分类控制) ---- */
typedef enum {
    CACT_CAT_GENERIC = 0,
    CACT_CAT_AUDIO,        /* 主动出声: 音乐/播报 */
    CACT_CAT_SPEECH,       /* 主动搭话 */
    CACT_CAT_FACE,         /* 表情/动画 */
    CACT_CAT_MAX
} cact_category_t;

/* ---- 风险等级(能力注册时声明; 不可被请求方降低) ---- */
typedef enum {
    CACT_RISK_EMERGENCY = 0,   /* 紧急停止等: 低延迟通路 */
    CACT_RISK_LOW,             /* 表达/播放: 自主可运行 */
    CACT_RISK_CONFIG,          /* 可恢复配置: 演化/用户提交 */
    CACT_RISK_SENSITIVE,       /* 隐私/管理: 需一次性确认 */
    CACT_RISK_FORBIDDEN,       /* 本版禁止自动执行 */
} cact_risk_t;

/* ---- 状态机(docs/17 §5.2; 终态单次尝试不可逆) ----
 * queued -> accepted -> started -> completed | failed
 *                            |-> cancel_requested -> cancelled |
 *                            |    interrupted | failed | unknown_outcome
 * queued/accepted -> cancel_requested -> ...
 * queued -> rejected(过期/策略)
 * awaiting_confirm -> queued(确认) | rejected(过期/拒绝) */
typedef enum {
    CACT_ST_NONE = 0,
    CACT_ST_QUEUED,
    CACT_ST_ACCEPTED,
    CACT_ST_STARTED,
    CACT_ST_COMPLETED,          /* 终态 */
    CACT_ST_FAILED,             /* 终态 */
    CACT_ST_CANCEL_REQUESTED,
    CACT_ST_CANCELLED,          /* 终态 */
    CACT_ST_INTERRUPTED,        /* 终态 */
    CACT_ST_REJECTED,           /* 终态 */
    CACT_ST_UNKNOWN_OUTCOME,    /* 终态: 无法确认的外部副作用 */
    CACT_ST_AWAITING_CONFIRM,   /* 敏感动作等待确认 */
} cact_status_t;

bool cact_status_is_terminal(cact_status_t s);
const char *cact_status_name(cact_status_t s);

/* ---- 结果码(与 docs/17 §9.3 HTTP 语义对应) ---- */
typedef enum {
    CACT_RES_OK = 0,
    CACT_RES_INVALID,          /* 参数/状态非法 (400) */
    CACT_RES_UNAUTHENTICATED,  /* (401) */
    CACT_RES_FORBIDDEN,        /* 权限/自主关/停止锁存/风险 (403) */
    CACT_RES_STALE,            /* 代次过期/幂等重放无新执行 (409) */
    CACT_RES_BUSY,             /* 队列满/资源冲突 (429) */
    CACT_RES_NO_SPACE,
    CACT_RES_NOT_READY,        /* 能力存在但当前不可用 */
    CACT_RES_UNSUPPORTED,      /* 能力未注册 */
    CACT_RES_IO,
} cact_res_t;

/* ---- 回执(每次状态变化一条; docs/17 §5.2) ---- */
typedef struct {
    uint32_t action_id;
    uint16_t seq;              /* 全局递增 */
    cact_status_t status;
    cact_origin_t source;      /* 该动作的来源(内部判定值) */
    char reason[CACT_REASON_MAX];
    int64_t observed_ms;       /* 注入时钟 */
} cact_receipt_t;

/* ---- 能力描述符与执行端口 ---- */
struct cact_desc;
typedef struct cact_desc {
    const char *cap_id;        /* 如 "music.play" */
    int group_id;              /* 资源组(停止锁存粒度); 0=无组 */
    cact_category_t category;  /* 分类控制(docs/18 T12) */
    cact_risk_t risk;
    bool supports_cancel;
    int64_t max_duration_ms;   /* 0=不限(仅声明) */
    /* 参数校验: 返回 CACT_RES_OK / CACT_RES_INVALID */
    cact_res_t (*validate)(const struct cact_desc *d,
                           const uint8_t *params, size_t len);
    /* 执行: 核心置 ACCEPTED 后调用; 执行器自行经 cact_report 推进状态
     * (必须先 report(STARTED) 才允许 COMPLETED)。返回非 OK 视为
     * 立即 failed(reason="exec_reject")。 */
    cact_res_t (*execute)(const struct cact_desc *d, uint32_t action_id,
                          const uint8_t *params, size_t len);
    /* 取消请求: 核心置 CANCEL_REQUESTED 后调用; 实际终态由执行器
     * report(CANCELLED/INTERRUPTED/UNKNOWN_OUTCOME)。可空。 */
    void (*cancel)(const struct cact_desc *d, uint32_t action_id);
} cact_desc_t;

/* ---- 请求(调用方构造; origin 之外的字段不携带信任) ---- */
typedef struct {
    cact_origin_t origin;
    char capability[CACT_CAP_ID_MAX];
    uint8_t params[CACT_PARAMS_MAX];
    size_t params_len;
    uint32_t idem_key;         /* 幂等键(0=不参与幂等) */
    int64_t expires_ms;        /* 注入时钟到期; 0=永不过期 */
    bool confirmed;            /* SENSITIVE 动作的一次性确认标记 */
    uint32_t persona_epoch;    /* 0=不校验; 否则与 ctx 当前代不符即 STALE */
} cact_req_t;

/* ---- 上下文(调用方分配; 单 owner 串行访问) ---- */
typedef struct {
    cact_id_t id;
    cact_status_t st;
    int cap_idx;
    cact_req_t req;
    int64_t started_ms;
} cact_job_t;

typedef struct {
    /* 注入时钟: 每次调用前更新; 核心不自取时间 */
    int64_t now_ms;

    const cact_desc_t *caps[CACT_CAPS_MAX];
    int caps_n;

    bool autonomous_enabled;   /* 产品默认 true(docs/16 FR-14) */
    bool cat_enabled[CACT_CAT_MAX];        /* 分类开关, 默认全开 */
    /* 勿扰(docs/16 FR-21): 默认不启用; 仅抑制作用域内"新自主出声",
     * 不停止用户已开启音乐; 启用且时间不可信 → 保守视为生效 */
    struct {
        bool enabled;
        int start_min, end_min;            /* 当日分钟, 支持跨夜(start>end) */
        bool scope[CACT_GROUPS_MAX];       /* 默认组 1.. 全在作用域 */
    } quiet;
    int quiet_minute;                      /* 注入: 当日分钟 */
    bool quiet_time_trusted;               /* 注入: 时间可信(wall_time_trust) */
    bool boot_gate_open;                   /* 启动安全闸: 初始化=false,
                                              意图配置恢复验证后开启
                                              (docs/17 §5.5 重启恢复) */
    uint32_t persona_epoch;    /* 当前角色代次(切角色递增; docs/17 §7.3) */

    /* 用户停止锁存(docs/17 §5.5): 置位后该组新自主请求拒绝;
     * 用户显式来源不受影响。恢复由 cact_user_resume 显式调用。 */
    uint32_t stop_epoch;
    bool group_stopped[CACT_GROUPS_MAX];

    cact_job_t q[CACT_QUEUE_MAX];
    int q_len;

    /* 幂等表(有界, 满则最旧逐出) */
    struct {
        uint32_t key;
        cact_id_t id;
        bool known_final;      /* 终态已知才允许重放返回 */
    } idem[CACT_IDEM_MAX];
    int idem_n;

    /* 回执环 */
    cact_receipt_t receipts[CACT_RECEIPTS_MAX];
    int rc_head;               /* 下一写入位 */
    uint16_t rc_seq;

    cact_id_t next_id;
} cact_ctx_t;

/* ---- 并发与默认实例(docs/18 T13 首段) ----
 * 核心是纯 C; 多任务提交经注入的递归锁串行(装配层提供 FreeRTOS
 * 递归互斥量; 宿主测试用计数桩)。executor 回调在锁内执行, 需回报
 * 状态时再调 cact_report —— 递归锁保证同任务重入不死锁。 */
typedef struct {
    void (*enter)(void);
    void (*exit)(void);
} cact_lock_port_t;
void cact_set_lock_port(const cact_lock_port_t *port);

/* 默认上下文(装配层 init; 各提交方经 cact_default() 取用) */
void        cact_default_init(void);
cact_ctx_t *cact_default(void);

/* ---- 生命周期 API(返回值只表示"提交/操作"受理结果) ---- */
void        cact_init(cact_ctx_t *c);
cact_res_t  cact_register(cact_ctx_t *c, const cact_desc_t *d);
cact_res_t  cact_submit(cact_ctx_t *c, const cact_req_t *req,
                        uint32_t *out_action_id);
cact_res_t  cact_confirm(cact_ctx_t *c, uint32_t action_id);
cact_res_t  cact_cancel(cact_ctx_t *c, uint32_t action_id, const char *reason);
void        cact_pump(cact_ctx_t *c);
/* 执行器回报状态(唯一推进通道; 迁移非法返回 INVALID 且不改状态) */
cact_res_t  cact_report(cact_ctx_t *c, uint32_t action_id,
                        cact_status_t st, const char *reason);
cact_res_t  cact_receipt_last(cact_ctx_t *c, uint32_t action_id,
                              cact_receipt_t *out);

/* ---- 控制面 ---- */
void        cact_set_autonomous(cact_ctx_t *c, bool on);
/* ---- 设置持久化(T38; 存储函数注入保持 L0 纯度) ----
 * attach 后 setters 自动 persist; restore 在 init 后调用(固件装配处)。
 * blob 格式: [u32 magic "CACT"][u8 autonomous][u8 quiet_en][u16 start]
 *           [u16 end][u32 scope_bits] —— 布局演进需换 magic。 */
typedef struct {
    int (*save)(const void *buf, size_t len);            /* 0=OK */
    int (*load)(void *buf, size_t cap, size_t *len);     /* 1=无数据, 负=错 */
} cact_store_t;
void        cact_attach_store(const cact_store_t *st);
void        cact_restore(cact_ctx_t *c);        /* load+应用(损毁→默认) */
int         cact_persist(const cact_ctx_t *c);  /* 0=OK/负=错(未 attach=-2) */
void        cact_set_persona_epoch(cact_ctx_t *c, uint32_t epoch);
uint32_t    cact_persona_epoch(const cact_ctx_t *c);
void        cact_set_category(cact_ctx_t *c, cact_category_t cat, bool on);
bool        cact_category_enabled(const cact_ctx_t *c, cact_category_t cat);
void        cact_quiet_config(cact_ctx_t *c, bool enabled, int start_min,
                              int end_min);
void        cact_quiet_scope(cact_ctx_t *c, int group_id, bool member);
void        cact_quiet_clock(cact_ctx_t *c, int minute_of_day, bool trusted);
bool        cact_quiet_active(const cact_ctx_t *c, int group_id);
void        cact_boot_gate_open(cact_ctx_t *c, bool open);
bool        cact_boot_gate_is_open(const cact_ctx_t *c);
cact_res_t  cact_user_stop(cact_ctx_t *c, int group_id, const char *reason);
cact_res_t  cact_user_resume(cact_ctx_t *c, int group_id);
bool        cact_group_stopped(const cact_ctx_t *c, int group_id);
uint32_t    cact_stop_epoch(const cact_ctx_t *c);

#ifdef __cplusplus
}
#endif
