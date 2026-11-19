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
 *
 * S2 重构（docs/24 §2, 2026-11-19 用户裁决定案）——三层栈:
 *   trait(天级, S4 接入) → mood(慢层, τ=3h, 三水库满足度导出, AIBO
 *   口径) → emotion(快层, 变化率+冲量, Joffily 自由能口径=源锚保留)。
 *   E2a 水平项从快层撤出、升级为独立 mood 层(D2 终案);
 *   显示效价 = 0.65×快层(负性偏置×2) + 0.35×mood(WASABI 合成);
 *   ethics 闩锁解锁: 事件剂量+深度门负荷(纯冷落不可达)+比例泄漏
 *   τ=3 天+安抚加速(−0.20/次)+施密特(≤0.9 且 ≥30min)+再闩免疫
 *   30min——消除"永久闩锁"(行业弃用项)。
 *   arousal 缺席语义随 E1 更新: 回归基线(不再向 held emax 收敛)。
 */
#include "soul_emotion.h"

#include <math.h>
#include <string.h>

/* ---- 冻结参数 ---- */

#define KE_TAU_INT	4.0f
#define KE_TAU_DEC	16.0f
#define KE_URG_ON	0.10f
#define KE_URG_OFF	0.05f /* 激活门迟滞：Kaleido 附加（防点火抖动），
				 非源语义（源纯比较 >0.1） */
/* E2b(docs/23 §2.5)→S2 迁移(docs/24 §2.2 修订, 见 §9 版本注):
 * 负荷=事件剂量+深度门充电, 泄漏=比例式(τ=3 天)。
 *   - 剂量: 负面重评价 0.30×|Δ|(LLM 情绪标签)/轮失败 +0.05——虐待
 *     (10×mood-0.2+10×fail/日)日充 ~1.1, 稳态=剂量率×τ≈3.3>LOAD_T
 *     → 2-3 天必闩;
 *   - 深度门 GATE=0.35(=mood 单独可达上限): 纯冷落(valence≥-0.35)
 *     永远充不进——冷落≠虐待(沉默不触发安全模式); 快层负脉冲叠深
 *     mood 越门才按超出部分计费;
 *   - 泄漏 τ=3 天: 自然等待解锁 ~3 天量级(应激慢性时标), 安抚
 *     (SOOTHE 0.20/次, Furby 口径 ~12 次清完)才是快路径。
 * LOAD_T/SOOTHE/OFF_T(施密特)/MIN_LATCH/IMMUNE 沿用 A2 定案。 */
#define KE_ETHICS_LOAD_T	2.5f
#define KE_ETHICS_TAU		10.0f
#define KE_ETH_V_GATE		0.35f
#define KE_ETH_DOSE_REAP	0.30f
#define KE_ETH_DOSE_FAIL	0.05f
#define KE_ETH_LEAK_TAU		259200.0f
#define KE_ETH_SOOTHE		0.20f
#define KE_ETH_OFF_T		0.9f
#define KE_ETH_MIN_LATCH_S	1800u
#define KE_ETH_IMMUNE_S		1800u
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

/* E2a(docs/23 §2.5)→S2 迁移(docs/24 §2.1): 水平项撤出快层、升级为
 * 独立 mood 慢层(参数见 step_mood)。快层保留差分口径(源锚"持续
 * 饥渴不难过"不破)+事件冲量; PULSE=2.0×Δemax 骤降(好事即时快乐)。
 * NEG_BIAS=负性偏置(ROBIN 实证 6.7×上界取保守 2; 仅显示合成时施加,
 * s_val 积分器保持无偏)。DISP_*=显示合成权重(WASABI ½(x+y) 结构)。 */
#define KE_V_DIFF_GAIN	1.0f
#define KE_V_PULSE	2.0f
#define KE_V_NEG_BIAS	2.0f
#define KE_DISP_FAST	0.65f
#define KE_DISP_MOOD	0.35f

/* S2 mood 慢层(docs/24 §2.1; 参数经三场景月测一轮回调): TAU=10800s
 * (3h, 调研带 2~4h 取中); L/H=舒适带(AIBO regulated range 口径;
 * L=0.2: 水位低于 0.2 满足度=0——0.3 在"life 也常见底"的日常节奏下
 * 过苛); WMIN=0.4 最缺者权重(0.5 使任一维见底即压死 μ, AIBO 最大
 * 剥夺语义取保守折半)。 */
#define KE_MOOD_TAU	10800.0f
#define KE_MOOD_L	0.20f
#define KE_MOOD_H	0.80f
#define KE_MOOD_WMIN	0.40f

/* S2 事件唤醒脉冲(Zillmann 残差唤醒口径): E1 的 Δemax 通道只覆盖
 * "恶化中"(被动), 事件本身的激活性(被骂=激动/被夸=兴奋)无通路——
 * 月测实证 arousal 全场景恒 0.10, 表情全部塌进低唤醒半区。补三路:
 * 重评价 |Δ|(双向)/轮成功/轮失败。 */
#define KE_AR_EVT_REAP	2.0f
#define KE_AR_EVT_OK	0.10f
#define KE_AR_EVT_FAIL	0.15f

/* S2.5: trait 调谐(默认=S2 中性常数, ke_tune_set 可改) */
static struct ke_tune s_tune = {
	.arousal_base = KE_AROUSAL_BASE,
	.valence_off = 0.0f,
	.tau_dec_mod = 1.0f,
	.bond_up_mod = 1.0f,
	.urg_on = KE_URG_ON,
	.novelty_gain_mod = 1.0f,
	.eth_gate = KE_ETH_V_GATE,
	.eth_dose_mod = 1.0f,
	.eth_load_t = KE_ETHICS_LOAD_T,
	.eth_heal_tau_mod = 1.0f,
	.eth_soothe_mod = 1.0f,
};

void ke_tune_set(const struct ke_tune *t)
{
	if (t == NULL) {
		s_tune = (struct ke_tune){
			.arousal_base = KE_AROUSAL_BASE,
			.urg_on = KE_URG_ON,
			.eth_gate = KE_ETH_V_GATE,
			.eth_load_t = KE_ETHICS_LOAD_T,
		};
		return;
	}
	s_tune = *t;
}

/* ---- 引擎状态（文件级，ke_reset 可全清：S6）---- */

static struct ke_state s_st;
static bool s_inited;
static bool s_urg_on;
static float s_ethics_load;
static float s_prev_e;
static float s_prev_emax;      /* D5：缺席拍 arousal/urgency 的保持源 */
static float s_val, s_urg, s_per, s_ctl, s_nov, s_unc, s_bond;
/* S2(docs/24 §2): mood 慢层积分器 + 闩锁机芯(起拍/免疫期) */
static float s_mood;
static float s_ar_evt_pulse;   /* 本拍待施加的事件唤醒脉冲(一次性) */
static uint64_t s_latch_tick;
static uint64_t s_immune_until;
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

/* 快层 valence(S2, docs/24 §2.1)：内稳态误差变化率负值（A1：先差分
 * 后更新）+ 事件冲量（Δemax 骤降=好事即时快乐）。误差水平语义已迁移
 * mood 慢层(step_mood)——快层保持源锚"持续饥渴不难过"口径。
 * 仅更新积分器 s_val; 显示合成(偏置+mood 混合)在 ke_step 统一做。 */
static void step_valence_fast(const struct ke_inputs *in)
{
	float raw_v = 0.0f;

	if (in->have_drive) {
		float e_sum = in->drive_error_sum;
		raw_v = -(e_sum - s_prev_e) * KE_V_DIFF_GAIN +
			(s_prev_emax - s_ar_prev_emax < -0.01f ?
			 (s_prev_emax - s_ar_prev_emax) * KE_V_PULSE : 0.0f);
		s_prev_e = e_sum;
	}
	s_val = ke_lowpass(s_val, raw_v, KE_TAU_INT);
}

/* mood 慢层(S2, docs/24 §2.1)：三水库区间满足度导出(AIBO 口径)。
 * s_i = 水位在舒适带 [L,H] 的归一位置; μ_v = 最缺者主导 50%+均值 50%,
 * 映射 [-1,1]; 一阶回归 τ=3h(SDT/CUMS: 持续剥夺→持续低落是慢变量;
 * WASABI/ALMA: mood 偏置认知, emotion 偏置行为)。缺席拍保持(D5 同源)。 */
static void step_mood(const struct ke_inputs *in)
{
	float s[3], mn, av, mu;

	if (!in->have_drive)
		return;
	for (int i = 0; i < 3; i++) {
		s[i] = (in->drive_sat[i] - KE_MOOD_L) / (KE_MOOD_H - KE_MOOD_L);
		s[i] = clampf(s[i], 0.0f, 1.0f);
	}
	mn = s[0] < s[1] ? (s[0] < s[2] ? s[0] : s[2])
			 : (s[1] < s[2] ? s[1] : s[2]);
	av = (s[0] + s[1] + s[2]) / 3.0f;
	mu = 2.0f * (KE_MOOD_WMIN * mn + (1.0f - KE_MOOD_WMIN) * av) - 1.0f;
	s_mood = ke_lowpass(s_mood, mu, KE_MOOD_TAU);
}

/* 显示合成(S2)：负性偏置只在显示侧施加(积分器保持无偏, 防反馈);
 * 快 0.65 + 慢 0.35(WASABI ½(x+y) 加权变体)。 */
static float compose_valence(void)
{
	float v_fast = s_val < 0.0f ? s_val * KE_V_NEG_BIAS : s_val;

	return clampf(KE_DISP_FAST * v_fast + KE_DISP_MOOD * s_mood +
		      s_tune.valence_off,
		      -1.0f, 1.0f);
}

/* arousal/urgency/persistence/controllability/novelty/uncertainty/bond
 * S2: 增参 tick(闩锁计时); ethics 段换解锁机芯。 */
static void step_rest(const struct ke_inputs *in, uint64_t tick)
{
	float urg_raw;
	float vabs = s_st.valence < 0 ? -s_st.valence : s_st.valence;
	(void)vabs;   /* persistence 仍用; 见下 */

	/* D5：缺席拍保持（源 dynamics.py:96-108——误差保持 ⇒ 积分器
	 * 保持；arousal/urgency 吃 s_prev_emax，缺席拍不更新它即保持） */
	urg_raw = (s_prev_emax - s_tune.urg_on) / (1.0f - s_tune.urg_on);

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
	{
		float evt_pulse = s_ar_evt_pulse;
		if (in->receipt == KE_RECEIPT_SUCCESS)
			evt_pulse += KE_AR_EVT_OK;
		else if (in->receipt == KE_RECEIPT_FAIL)
			evt_pulse += KE_AR_EVT_FAIL;
		s_ar_evt_pulse = 0.0f;
		s_st.arousal = clampf(
			s_st.arousal +
			KE_AROUSAL_GAIN * (s_prev_emax - s_ar_prev_emax) +
			evt_pulse +
			(s_tune.arousal_base - s_st.arousal) / KE_AROUSAL_TAU,
			0.0f, 1.0f);
	}
	s_ar_prev_emax = s_prev_emax;

	s_per = ke_lowpass(s_per, clampf(vabs, 0.0f, 0.8f) / 0.8f,
			   KE_TAU_DEC * s_tune.tau_dec_mod);
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
		s_nov = ke_lowpass(s_nov,
				   clampf(in->sensory_residual_mean *
					  s_tune.novelty_gain_mod, 0.0f, 1.0f),
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
		s_bond += KE_BOND_ALPHA_UP * s_tune.bond_up_mod * (1.0f - s_bond);
		s_bond_grace = KE_BOND_GRACE_S;
	} else if (s_bond_grace > 0) {
		s_bond_grace--;
	} else {
		s_bond -= KE_BOND_ALPHA_DOWN * s_bond;
		if (s_bond < KE_BOND_FLOOR) s_bond = KE_BOND_FLOOR;
	}
	s_st.social_bond = clampf(s_bond, 0.0f, 1.0f);

	/* 伦理闩锁(S2 L2 方案, 参数块注): 负荷=事件剂量(ke_step D4 段/
	 * 本段轮失败)+深度门充电(仅越 mood-only 上限部分), 泄漏比例式
	 * τ=3 天。解锁三路——时间自愈(泄漏)/安抚加速(−0.20/次)/施密特
	 * (≤0.9 且已闩锁≥30min); 再闩免疫 30min 防横跳。守恒保证:
	 * 纯冷落无剂量且充不进门 → 永不闩锁; 持续虐待剂量率×τ>LOAD_T
	 * → 必闩; 恶意等待(停虐不安抚)需 ~3 天泄漏才解锁。显式重置兜底
	 * = ke_ethics_clear(owner/出厂路径)。 */
	if (in->receipt == KE_RECEIPT_FAIL)
		s_ethics_load += KE_ETH_DOSE_FAIL * s_tune.eth_dose_mod;
	if (s_st.valence < -s_tune.eth_gate)
		s_ethics_load += (-s_st.valence - s_tune.eth_gate) /
				 KE_ETHICS_TAU * s_tune.eth_dose_mod;
	s_ethics_load -= s_ethics_load / (KE_ETH_LEAK_TAU * s_tune.eth_heal_tau_mod);
	if (s_ethics_load < 0.0f)
		s_ethics_load = 0.0f;
	if (!s_st.ethics_latched) {
		if (s_ethics_load >= s_tune.eth_load_t &&
		    tick >= s_immune_until) {
			s_st.ethics_latched = true;
			s_latch_tick = tick;
		}
	} else {
		if (in->soothe_event) {
			float so = KE_ETH_SOOTHE * s_tune.eth_soothe_mod;
			s_ethics_load = s_ethics_load > so
				? s_ethics_load - so : 0.0f;
		}
		if (s_ethics_load <= KE_ETH_OFF_T &&
		    tick - s_latch_tick >= KE_ETH_MIN_LATCH_S) {
			s_st.ethics_latched = false;
			s_immune_until = tick + KE_ETH_IMMUNE_S;
		}
	}
	s_st.ethics_load = s_ethics_load;
}

void ke_step(uint64_t tick, const struct ke_inputs *in,
	     struct ke_output *out)
{
	if (!in)
		return;
	if (!s_inited) {
		s_val = s_st.valence;
		s_mood = s_st.mood;
		s_urg = s_st.urgency;
		s_per = s_st.persistence;
		s_ctl = s_st.controllability;
		s_nov = s_st.novelty;
		s_unc = s_st.uncertainty;
		s_bond = s_st.social_bond;
		s_inited = true;
	}
	/* D4：重评价一次性施加（进快层积分器，随本拍低通走）。
	 * S2: 负面重评价同时给伦理负荷直接剂量(事件计费, 参数块注)。 */
	if (!s_reap_done && s_reap_receipt != 0) {
		if (s_reap_pending < 0.0f)
			s_ethics_load += KE_ETH_DOSE_REAP * s_tune.eth_dose_mod *
					  (-s_reap_pending);
		s_ar_evt_pulse = fabsf(s_reap_pending) * KE_AR_EVT_REAP;
		s_val += s_reap_pending;
		s_reap_applied = s_reap_pending;
		s_reap_pending = 0.0f;
		s_reap_done = true;
	}
	if (in->have_drive)
		s_prev_emax = in->drive_error_max;   /* D5：缺席拍保持 */
	step_valence_fast(in);
	step_mood(in);
	s_st.mood = clampf(s_mood, -1.0f, 1.0f);
	s_st.valence = compose_valence();   /* 显示合成(S2) */
	step_rest(in, tick);
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

void ke_ethics_dose(float amt)
{
	s_ethics_load += amt;
	if (s_ethics_load < 0.0f)
		s_ethics_load = 0.0f;
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
	s_mood = 0.0f;
	s_ar_evt_pulse = 0.0f;
	s_latch_tick = 0;
	s_immune_until = 0;
	s_reap_pending = 0.0f;
	s_reap_receipt = 0;
	s_reap_applied = 0.0f;
	s_reap_done = false;
	if (init)
		s_st = *init;
}
