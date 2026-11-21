/*
 * kaleido_ignition.h — 点火机制方案 A：显著性打分 + 防骚扰门 + 滞回
 *
 * 设计依据：docs/research/22-点火机制与决策模型调研.md §4.1（方案 A）
 *  - 五因子打分（时间节律/情感动机/记忆钩子/事件钩子/在场置信）
 *  - AND 硬门：勿扰外 ∧ 冷静期过 ∧ 配额未用 ∧ 用户未说话
 *  - 双阈值滞回（T_high 触发 / T_low 复位）防抖
 *  - 结算反馈：被忽略 → 冷静期指数翻倍；被回应 → 复位（22号 §5.5）
 *  - 日志透明：decision 携带全因子与门状态（供 /ignite 云裁决 prompt）
 * 方案 B（Pareto 仲裁/慢环）演进时本组件降级为因子供给器。
 */
#ifndef KALEIDO_IGNITION_H
#define KALEIDO_IGNITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KI_FACTOR_CNT	5

struct ki_factors {
	float time_rhythm;   /* 时间节律（作息匹配度）        */
	float affect_motive; /* 情感动机（ke urgency 门输出） */
	float memory_hook;   /* 记忆钩子（到期承诺/三因子Top1） */
	float event_hook;    /* 事件钩子（充电完成/长时间无互动） */
	float presence;      /* 在场置信（IMU 拿起/VAD 人声） */
};

struct ki_config {
	float w[KI_FACTOR_CNT]; /* 权重（默认等权 0.2）        */
	float t_high;           /* 触发阈值（默认 0.70）       */
	float t_low;            /* 复位阈值（默认 0.50）       */
	uint16_t cooldown_min;  /* 基础冷静期（默认 30 分钟）  */
	uint16_t cooldown_max;  /* 上限（默认 240）            */
	uint8_t daily_quota;    /* 日配额（默认 4）            */
	uint8_t proactivity;    /* 主动度 0-10（persona 注入） */
};

struct ki_context {
	bool user_speaking;    /* VAD：用户说话中 → 禁火      */
	bool quiet_hours;      /* 勿扰时段（本地 RTC 裁决）   */
	uint32_t now_min;      /* 当日分钟数（0-1439）        */
	uint32_t last_fire_min;/* 上次点火（当日分钟）        */
	uint8_t fired_today;   /* 今日已用配额                */
	bool holding;          /* 滞回态（上次过 T_high）     */
};

struct ki_decision {
	bool candidate;        /* 过滞回 + 过硬门             */
	float score;
	struct ki_factors f;
	struct {
		uint8_t gate_mask; /* bit0..3 = quiet/cooldown/
				      quota/speaking，1=被拦 */
	} gates;
};

/* 默认参数（static const 冻结；persona 可覆写副本后传入） */
const struct ki_config *ki_config_default(void);

/*
 * 打分 + 滞回 + 硬门。proactivity<2 时 t_high 上浮
 * （"太黏了"降档语义：22号 §5.3 旋钮统一缩放）。
 */
void ki_step(const struct ki_context *ctx, const struct ki_config *cfg,
	     const struct ki_factors *f, struct ki_decision *out);

/*
 * 结算（点火执行后必调）：responded=false（被忽略）→ 冷静期 ×2 封顶；
 * responded=true → 冷静期复位。返回下一次冷静期（分钟）。
 */
uint16_t ki_settle(bool responded);

/* 当前生效冷静期（分钟） */
uint16_t ki_cooldown_current(void);

/* ---- W3：点火=思维触发开关（29 号 §9；ki_step 降格为因子供给） ----
 * 语义修订（负责人裁定）：点火只决定"是否思考"；思考后对外作何动作
 * 由 LLM 深思产出 typed 意图，机械面只守门（parse/出口门/结算）。
 * ki_step/ki_settle 保留：前者供 score 因子，后者被 ki_settle2 取代
 * （二值结算会把打断误算 ignored——"越感兴趣越沉默"缺陷）。 */

enum ki_act {
	KI_STAY_SILENT = 0,	/* 一等输出：想了但决定不说           */
	KI_SPEAK,		/* 说一句话（M2 唯一实装动作）        */
	KI_ACT,			/* 非语音动作（词表位；启用=W6c 后）  */
	KI_CNT
};

#define KI_CONTENT_MAX	80	/* UTF-8 字节钳位（超长截断+留痕）    */
#define KI_CONF_FLOOR	0.5f	/* 置信下限：<0.5 → stay_silent      */

/* [S5→B-6a 退役] ki_think_gate/ki_think_ctx/ki_think_gate 结构与
 * KI_THINK_GAP_MIN/KI_THINK_DAILY_MAX/KI_SALIENT_T 宏已删——触发面
 * 由 soul_motiv sm_gate 六维显著性仲裁取代(docs/24 §4.2/§14)。
 * 深思节流(间隔/日帽)现由 sm 令牌桶与 ki_fire 门承担。 */

/* 深思产出（29 号 §9.5；单槽意图=OwnedGoal 退化版） */
struct ki_intent {
	enum ki_act act;
	char content[KI_CONTENT_MAX + 1];/* speak 必填；≤80B 载荷+NUL */
	float confidence;               /* 钳位 [0,1]                      */
	uint16_t ttl_ticks;             /* 缺省 30（1Hz；TTL 在调用方）    */
	uint8_t evidence_mask;          /* bit0 drive 1 affect 2 mem
					 * 3 dialog 4 bin（C-5 移植）      */
	char reason[64];                /* 叙事锚："它为什么开口"          */
};

/* LLM 意图解析：机械校验+钳位，**拒非降级**（schema 违例绝不猜）。
 * 期望单行 JSON：{"act":"speak","content":"…","confidence":0.8,
 * "evidence":["drive:…"],"reason":"…"}；evidence 前缀词表
 * drive|affect|mem|dialog|bin。content 含数字且无 mem 证据 → 拒
 * （UncitedClaim 移植，防幻觉统计/时间承诺）。 */
enum ki_parse_err {
	KI_PARSE_OK = 0,
	KI_PARSE_JSON_BAD,      /* schema 违例                         */
	KI_PARSE_ACT_ILLEGAL,
	KI_PARSE_CONTENT_EMPTY, /* speak 必填                          */
	KI_PARSE_CONTENT_LONG,  /* >80B：已截断+留痕（非拒）           */
	KI_PARSE_NO_EVIDENCE,   /* C-5：无内部状态证据                 */
	KI_PARSE_UNCITED_NUMBER,
	KI_PARSE_DUPLICATE      /* 5min 去重窗（调用方判，码位预留）   */
};

enum ki_parse_err ki_intent_parse(const char *json, size_t len,
				  struct ki_intent *out);

/* 出口硬门（原 ki_step 四门原样搬+ethics/tts；只门动作不门思维）。
 * 纯函数：gate_mask bit0 quiet 1 cooldown 2 quota 3 speaking
 * 4 ethics 5 tts；candidate=全过。TTL/去重在调用方（秒级粒度）。 */
struct ki_fire_ctx {
	bool user_speaking;
	bool quiet_hours;
	bool ethics_latched;
	bool tts_ready;
	uint32_t now_min;
	uint32_t last_fire_min;
	uint8_t fired_today;
};

void ki_intent_fire(const struct ki_intent *in,
		    const struct ki_fire_ctx *ctx,
		    const struct ki_config *cfg,
		    struct ki_decision *out);

/* 结算六态（Galatea SettlementKind 平移）：仅 IGNORED 翻倍（唯一
 * 惩罚路径）；UNKNOWN/SILENT/INTERRUPTED/GATE_BLOCKED 合法不罚——
 * 禁把未解决归 FAILED（负偏置自欺）。返回下一次说话冷静期（分钟） */
enum ki_settle {
	KI_SETTLE_RESPONDED,
	KI_SETTLE_IGNORED,
	KI_SETTLE_INTERRUPTED,
	KI_SETTLE_GATE_BLOCKED,
	KI_SETTLE_SILENT,
	KI_SETTLE_TTL_EXPIRED,
};

uint16_t ki_settle2(enum ki_settle s);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_IGNITION_H */
