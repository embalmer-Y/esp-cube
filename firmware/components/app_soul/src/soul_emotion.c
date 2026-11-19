/*
 * kaleido_emotion.c — 九维感受态动力学实现
 *
 * 移植自 Galatea L05 affect（MIT；vendor-licenses.md 义务 1：保留出处）。
 * 全部确定性：零 RNG、零墙钟、零分配。
 * 返工记录（评审 A1/S6/G1/G2/B2/B5，2026-10-25；W0-2 对源偏差修正
 * D1-D7，2026-10-28，依据 docs/research/28 §4.3 逐项审计）：
 *  - A1：valence 差分先算后更新 s_prev（!have_drive 不更新）
 *  - S6：积分器移文件级 static，ke_reset 全清（含 init 注入）
 *  - G1：回执三态（无事件拍保持）
 *  - B2：ke_step 拆三段
 *  - D1：drive_error_sum 语义（源 E=Σw·e 无归一；mean 为移植发明，
 *        曾把 valence 域压 1/3 且永不饱和）
 *  - D2/D3：伦理阈仿真定档 −0.35/3 拍（见下注）+ ke_ethics_clear()
 *  - D4：重评价一次性精确施加 + applied 审计（TTL 斜坡废）
 *  - D5：!have_drive 拍 arousal/urgency 保持（源缺席语义，禁衰减）
 *  - D6：ke_hysteresis 半带宽（源 band=0.1 ⇒ 0.55/0.45 边界含）
 *  - D7：novelty/uncertainty 有效性位（无效拍保持，禁主动衰减）
 *
 * 伦理阈定档注（D2/D3，V-48a 仿真）：契约域 E∈[0,3]（三驱力等权）
 * 内瞬时全崩 raw=−3 → 轨迹 −0.75/−0.5625/−0.4219（连续 3 拍 ≤−0.35）
 * 恰好闩锁；双驱力崩最深 −0.5 仅 2 拍不触发——单轮失败不误闩。
 * 可达性收敛（评审 C9）：kd 脉冲全为正向恢复、代谢 1e-4 量级，现
 * 动力学下唯一自然通路是 reappraisal 连续负注入（≥6 拍 −0.2 →
 * published 稳态 −0.45 以下）——符合"极端持续负面=认知层持续自贬"
 * 安全底座语义。源阈 −0.8/64 拍双条件为构造不可达，Kaleido 事件直注
 * 语义下按上法定档，正式值留 H6 产品评审。
 */
#include "soul_emotion.h"

#include <string.h>

/* ---- 冻结参数 ---- */

#define KE_TAU_INT	4.0f
#define KE_TAU_DEC	16.0f
#define KE_URG_ON	0.10f
#define KE_URG_OFF	0.05f /* 激活门迟滞：Kaleido 附加（防点火抖动），
				 非源语义（源纯比较 >0.1） */
/* E2b(docs/23 §2.5): 闩锁负荷参数。LOAD_T=2.5(≈v=-0.25 持续 10s 或
 * v=-0.5 持续 5s 即闩); TAU=负荷积分折算(每拍 load += |v|/TAU);
 * LEAK=每拍回漏(轻负面 ~30s 自愈, 深负面净增必然达阈)。 */
#define KE_ETHICS_LOAD_T	2.5f
#define KE_ETHICS_TAU		10.0f
#define KE_ETHICS_LEAK		0.008f
#define KE_REAP_MAX	0.2f
/* E4-R3(docs/23 §2.5): bond 参数。升=ALPHA_UP 0.02/次(30 次互动近满);
 * 降=ALPHA_DOWN 5e-6/拍(τ≈2.3 天, **过夜存活 81%**——依恋要能过夜,
 * R2 教训: 83min τ 使每晚清零月末回地板); GRACE_S=900(15min 宽限,
 * Tamagotchi 口径); FLOOR=0.05(依恋不灭)。 */
#define KE_BOND_ALPHA_UP	0.02f
#define KE_BOND_ALPHA_DOWN	5.0e-6f
#define KE_BOND_GRACE_S	900u
#define KE_BOND_FLOOR	0.05f
static uint32_t s_bond_grace;
/* E1(docs/23 §2.5): arousal 回归基线/时间常数(秒)/冲量增益。基线取低
 * 水平(清醒不惊恐); τ=64s≈"情绪事件过去后一分钟回落"的 ALMA 情绪层
 * 口径; GAIN=2.0→单次 round(Δemax≈0.15)推 arousal +0.3, 连续社交可到
 * 高唤醒但不驻留(代谢只升 emax 不升 arousal, 回归必拉回)。
 * s_ar_prev_emax=冲量通道的差分记忆(ke_reset 归零)。 */
#define KE_AROUSAL_BASE	0.10f
#define KE_AROUSAL_TAU	64.0f
#define KE_AROUSAL_GAIN	2.0f
static float s_ar_prev_emax;

/* E2a(docs/23 §2.5): valence 构成项增益。DIFF_GAIN=1 保持原差分口径
 * 幅度不变(锚定); 水平项=SATISFY×(NEUTRAL_ESUM−e_sum)。**水位校准
 * (E2 首轮月测教训)**: 三驱力在"无恢复"下的代谢稳态是 e→1 各自封顶
 * (sim 实测 m_social+m_stim≈2.0 为主项), 首轮取 band 口径 0.3 不可达
 * → 全场景均值 -0.34 贴底(sigh 99% 新单态)。重校准: 期望水位=
 * 1.2(≈0.6×日常稳态 2.0)——白天正常互动(事件恢复压制代谢)应在水位
 * 上下摆动, 冷落长期低于水位; SATISFY=0.18: e_sum=0 全满足→+0.22,
 * e_sum=3 极深→−0.32(叠加差分负值可越旧阈 -0.35); PULSE=2.0×Δemax
 * 骤降(好事发生即时快乐)。 */
#define KE_V_DIFF_GAIN	1.0f
#define KE_V_SATISFY	0.18f
#define KE_V_NEUTRAL_ESUM	1.2f
#define KE_V_PULSE	2.0f

/* ---- 引擎状态（文件级，ke_reset 可全清：S6）---- */

static struct ke_state s_st;
static bool s_inited;
static bool s_urg_on;
static float s_ethics_load;
static float s_prev_e;
static float s_prev_emax;      /* D5：缺席拍 arousal/urgency 的保持源 */
static float s_val, s_urg, s_per, s_ctl, s_nov, s_unc, s_bond;
static float s_reap_pending;   /* 待施加（下一拍生效） */
static uint64_t s_reap_receipt;
static float s_reap_applied;   /* 最近一次实际施加量（D4 审计） */
static bool s_reap_done;

float ke_lowpass(float s, float x, float tau)
{
	return s + (x - s) / tau;
}

/* 半带宽迟滞（D6）：ON ≥ 0.5+band/2、OFF ≤ 0.5−band/2，边界含 */
bool ke_hysteresis(bool on, float x, float band)
{
	float h = band * 0.5f;

	if (!on && x >= 0.5f + h)
		return true;
	if (on && x <= 0.5f - h)
		return false;
	return on;
}

int ke_reappraisal_request(const struct ke_reappraisal *req)
{
	if (!req || req->receipt_id == 0)
		return -1;
	if (req->delta > KE_REAP_MAX || req->delta < -KE_REAP_MAX)
		return -2;
	s_reap_pending = req->delta;   /* 覆盖未生效的上一请求（单槽） */
	s_reap_receipt = req->receipt_id;
	s_reap_done = false;
	return 0;
}

bool ke_reappraisal_receipt(uint64_t receipt_id)
{
	return receipt_id != 0 && receipt_id == s_reap_receipt &&
	       s_reap_done;
}

float ke_reappraisal_applied(void)
{
	return s_reap_applied;
}

void ke_ethics_clear(void)
{
	s_st.ethics_latched = false;
	s_ethics_load = 0.0f;
}

static const char *anim_tag(float v, float a)
{
	if (v >= 0.15f)
		return a >= 0.55f ? "happy" : "relaxed";
	if (v <= -0.15f)
		return a >= 0.55f ? "angry" : "sad";
	if (a >= 0.55f)
		return "surprised";
	if (a >= 0.30f)
		return "confused";
	return "sleepy";
}

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

/* valence：内稳态误差变化率负值（A1：先差分后更新）。
 * 误差水平只表达在 arousal/urgency；valence 只表达变化率
 * （"持续饥渴不难过，恶化中才难过"，源测试钉死语义）
 *
 * E2a(docs/23 §2.5, 2026-11-19): 30 天 sim 实证纯差分口径 valence 窄带
 * [-0.15, 0.11](恒定代谢率→恒定微小负值; mood 注入被 τ 低通快速消化),
 * 三种对待(爱/冷落/虐待)表情无区分度+ethics 闩锁不可达。叠加两项构成项
 * (差分口径保留, 不破坏锚定语义):
 *   ① 水平项(AIBO 口径: 效价是稳态满足度的导出量)——驱力越满足越正,
 *      缺口越深越负。量级: e_sum∈[0,3], 经 KE_V_SATISFY 折算, 使
 *      "全满足"≈+0.3 / "深缺口"≈-0.45(可越过闩锁阈 -0.35)。
 *   ② 事件冲量项——kd 事件恢复(pulse)造成的 emax 骤降给正冲量, 幅度
 *      直达不再被低通抹平(KE_V_PULSE), 代表"好事发生"的即时快乐。
 * 月测目标: v 带宽 ≥0.5 且 abuse 场景闩锁可达。 */
static void step_valence(const struct ke_inputs *in)
{
	float raw_v = 0.0f;

	if (in->have_drive) {
		float e_sum = in->drive_error_sum;
		raw_v = -(e_sum - s_prev_e) * KE_V_DIFF_GAIN +
			(KE_V_NEUTRAL_ESUM - e_sum) * KE_V_SATISFY +
			(s_prev_emax - s_ar_prev_emax < -0.01f ?
			 (s_prev_emax - s_ar_prev_emax) * KE_V_PULSE : 0.0f);
		s_prev_e = e_sum;
	}
	s_val = ke_lowpass(s_val, raw_v, KE_TAU_INT);
	s_st.valence = clampf(s_val, -1.0f, 1.0f);
}

/* arousal/urgency/persistence/controllability/novelty/uncertainty/bond */
static void step_rest(const struct ke_inputs *in)
{
	float urg_raw;
	float vabs = s_st.valence < 0 ? -s_st.valence : s_st.valence;

	/* D5：缺席拍保持（源 dynamics.py:96-108——误差保持 ⇒ 积分器
	 * 保持；arousal/urgency 吃 s_prev_emax，缺席拍不更新它即保持） */
	urg_raw = (s_prev_emax - KE_URG_ON) / (1.0f - KE_URG_ON);

	if (urg_raw < 0.0f)
		urg_raw = 0.0f;
	s_urg = ke_lowpass(s_urg, urg_raw, KE_TAU_INT);
	s_st.urgency = clampf(s_urg, 0.0f, 1.0f);
	s_urg_on = s_urg_on ? (s_st.urgency > KE_URG_OFF) :
			       (s_st.urgency > KE_URG_ON);

	/* E1(docs/23 §2.5, 2026-11-19): arousal = 事件冲量积分 + 基线回归。
	 * 旧实现 lowpass(→emax) 使 arousal 跟随驱力最大误差, 无互动时水库
	 * 只代谢 →error→1.0→arousal 单调吸附(30 天 sim 实证 shocked 100%)。
	 * 新方程: 冲量=本拍 emax 增量×独立增益(KE_AROUSAL_GAIN, 事件当拍
	 * 即感——round/barge 单次 Δemax≈0.1-0.15 应推 arousal 0.2-0.3 量级);
	 * 回归=向 KE_AROUSAL_BASE 指数回落(τ=64s, ALMA 情绪层口径)。
	 * 无输入单调回基线, 结构上不可能饱和。缺口水平表达不丢(urgency 仍走
	 * emax, bb M_* 仍直供 kd error)。 */
	s_st.arousal = clampf(
		s_st.arousal +
		KE_AROUSAL_GAIN * (s_prev_emax - s_ar_prev_emax) +
		(KE_AROUSAL_BASE - s_st.arousal) / KE_AROUSAL_TAU,
		0.0f, 1.0f);
	s_ar_prev_emax = s_prev_emax;

	s_per = ke_lowpass(s_per, clampf(vabs, 0.0f, 0.8f) / 0.8f,
			   KE_TAU_DEC);
	s_st.persistence = clampf(s_per, 0.0f, 1.0f);

	/* G1：三态回执——无事件拍保持 controllability；一轮结算一次 */
	if (in->receipt != KE_RECEIPT_NONE) {
		float x = in->receipt == KE_RECEIPT_SUCCESS ? 1.0f :
							 0.0f;

		s_ctl = ke_lowpass(s_ctl, x, KE_TAU_DEC);
	}
	s_st.controllability = clampf(s_ctl, 0.0f, 1.0f);

	/* D7：有效性位——无效拍保持（禁喂 0 造成的主动衰减） */
	if (in->have_residual)
		s_nov = ke_lowpass(s_nov, in->sensory_residual_mean,
				   KE_TAU_INT);
	s_st.novelty = clampf(s_nov, 0.0f, 1.0f);

	if (in->have_uncertainty)
		s_unc = ke_lowpass(s_unc, in->belief_uncertainty_mean,
				   KE_TAU_INT);
	s_st.uncertainty = clampf(s_unc, 0.0f, 1.0f);

	/* E4(docs/23 §2.5, 2026-11-19): bond 百分比更新+宽限窗+地板。
	 * 旧实现: 事件 +0.125 / 无事件 τ=1024s 指数趋零——月测"冷落即归零"
	 * (30 天无互动 bond→0), 与依恋语义(分离不灭)相悖。新实现三件:
	 *   ① 百分比式 ±α(1∓bond)(Hiolle): 天然减速, 越高越难升/越低越难降;
	 *   ② 宽限窗(Τamagotchi care mistake 口径): 静默 <KE_BOND_GRACE_S
	 *      不衰减(偶发间隔不扣分);
	 *   ③ 地板 KE_BOND_FLOOR: 依恋不灭(慢变量衰减到地板而非 0,
	 *      IWM 语义)。 */
	if (in->social_event) {
		s_bond += KE_BOND_ALPHA_UP * (1.0f - s_bond);
		s_bond_grace = KE_BOND_GRACE_S;
	} else if (s_bond_grace > 0) {
		s_bond_grace--;
	} else {
		s_bond -= KE_BOND_ALPHA_DOWN * s_bond;
		if (s_bond < KE_BOND_FLOOR) s_bond = KE_BOND_FLOOR;
	}
	s_st.social_bond = clampf(s_bond, 0.0f, 1.0f);

	/* 伦理闩锁(E2b 改泄漏积分负荷量): 旧判据"瞬时 v≤-0.35 连续 3 拍"
	 * 月测实证不可达(E2a 前动力学 v 窄带, 瞬时值被回拉永远够不到阈——
	 * 用秒级瞬时值触发分钟级持续状态的结构性错配, C 路诊断)。新判据=
	 * 非稳态负荷(应激医学 ∫stress·dt 同构): 超阈负效价按拍积分, 回漏
	 * 保证轻负面自愈, 深度/持续负面单调累积必然可达阈; 命中即粘滞
	 * (安全语义不变, 解锁唯 ke_ethics_clear)。 */
	if (s_st.valence < 0.0f) {
		s_ethics_load += (-s_st.valence) / KE_ETHICS_TAU;
	}
	s_ethics_load = s_ethics_load > KE_ETHICS_LEAK
		? s_ethics_load - KE_ETHICS_LEAK : 0.0f;
	if (s_ethics_load >= KE_ETHICS_LOAD_T) {
		s_st.ethics_latched = true;
	}
}

void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out)
{
	if (!in)
		return;
	if (!s_inited) {
		s_val = s_st.valence;
		s_urg = s_st.urgency;
		s_per = s_st.persistence;
		s_ctl = s_st.controllability;
		s_nov = s_st.novelty;
		s_unc = s_st.uncertainty;
		s_bond = s_st.social_bond;
		s_inited = true;
	}
	/* D4：重评价一次性施加（进积分器，随本拍低通走） */
	if (!s_reap_done && s_reap_receipt != 0) {
		s_val += s_reap_pending;
		s_reap_applied = s_reap_pending;
		s_reap_pending = 0.0f;
		s_reap_done = true;
	}
	if (in->have_drive)
		s_prev_emax = in->drive_error_max;   /* D5：缺席拍保持 */
	step_valence(in);
	step_rest(in);
	(void)tick;
	if (!out)
		return;
	out->state = s_st;
	strlcpy(out->anim_tag, anim_tag(s_st.valence, s_st.arousal),
		sizeof(out->anim_tag));
	out->tts_hint.rate = 0.8f + 0.4f * s_st.arousal; /* [0.8,1.2] */
	out->tts_hint.pitch = 2.0f * s_st.valence;
	out->ignition_candidate = s_urg_on && s_st.urgency > KE_URG_ON;
	out->hyst_saturated = 0;
}

const struct ke_state *ke_state_snapshot(void)
{
	return &s_st;
}

void ke_reset(const struct ke_state *init)
{
	memset(&s_st, 0, sizeof(s_st));
	s_inited = false;
	s_urg_on = false;
	s_prev_e = 0.0f;
	s_prev_emax = 0.0f;
	s_ar_prev_emax = 0.0f;
	s_val = s_urg = s_per = s_ctl = s_nov = s_unc = s_bond = 0.0f;
	s_ethics_load = 0.0f;
	s_bond_grace = 0;
	s_reap_pending = 0.0f;
	s_reap_receipt = 0;
	s_reap_applied = 0.0f;
	s_reap_done = false;
	if (init)
		s_st = *init;
}
