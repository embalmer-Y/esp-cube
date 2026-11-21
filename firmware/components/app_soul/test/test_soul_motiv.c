/*
 * test_soul_motiv.c — 四维动机+仲裁器+令牌桶黄金回归(S5, docs/24 §4)
 *
 * 锚(合同): kc 台账蒸发/缺席封顶/KL 门; ku 余辉+老化+被忽清仓;
 * kf 非对称 EMA+挫败线; kb 五态; 仲裁 S=U×M×H+习惯化; 令牌桶
 * 容量3/日滴灌12/S≥0.8 透支; 五通道修复(孤立 idle 不触发/去重/降采样)。
 */
#include <math.h>
#include <string.h>

#include "soul_motiv.h"
#include "unity.h"

static bool feq(float a, float b)
{
	return fabsf(a - b) < 1e-3f;
}

static struct sm_state S;
static void snap(void) { sm_snapshot(&S); }

/* 无事件门调用(纯环境) */
static bool gate(uint32_t now_s, uint32_t now_min, float arousal,
		 float e_soc, float e_stim, bool ai, bool play, bool net,
		 bool quiet, bool jump, bool mem, bool thinking,
		 struct sm_gate_out *out)
{
	struct sm_gate_in in = {
		.now_s = now_s, .now_min = now_min, .arousal = arousal,
		.e_social = e_soc, .e_stim = e_stim,
		.ai_active = ai, .playing = play, .net_up = net,
		.quiet_hours = quiet, .kd_sev_jump = jump,
		.mem_hook = mem, .thinking = thinking,
	};
	sm_gate(&in, out);
	return out->trigger;
}

/* kc: 台账蒸发 τ=480min + 缺席封顶 + KL 门 */
static void golden_kc(void)
{
	sm_reset();
	/* 开槽 w=0.5, t=0; 480min 后蒸发至 1/e≈0.184; 无交互>2h →
	 * 缺席缺口槽 +0.25 */
	TEST_ASSERT_TRUE(sm_question_open(0, 0.5f, 0));
	sm_tick(480u * 60);
	snap();
	TEST_ASSERT_TRUE(feq(S.kc, 0.5f / 2.71828f + 0.25f));
	/* 缺席缺口: >2h 无交互 → +0.25(sm_tick 内), 合计≈0.434 */
	sm_reset();
	sm_question_open(1, 0.30f, 0);
	sm_tick(8000);
	snap();
	/* 蒸发 133min: 0.30×exp(−133/480)≈0.2277 + 缺席 0.25 */
	TEST_ASSERT_TRUE(feq(S.kc, 0.30f * expf(-8000.0f / 60 / 480)
				  + 0.25f));
	/* kc 触发需 s_kl 门: 有 kc 无惊奇 → 通道 1 不触发 */
	sm_reset();
	sm_question_open(0, 0.9f, 0);
	sm_event(SM_EVT_TOUCH, 100);
	sm_event(SM_EVT_TOUCH, 5000);
	sm_tick(5000);
	struct sm_gate_out g;
	(void)gate(5000, 300, 0.5f, 0, 0, false, false, true, false,
		   false, false, false, &g);
	TEST_ASSERT_TRUE(g.reason_mask & (1 << 1));
}

/* ku: 余辉充能+τ 衰减+被忽清仓 */
static void golden_ku(void)
{
	sm_reset();
	sm_event(SM_EVT_BARGE, 1000);      /* after=0.8 */
	sm_tick(1000);
	snap();
	TEST_ASSERT_TRUE(feq(S.ku, 0.8f));
	sm_tick(1000 + 600);               /* τ 衰减至 1/e */
	snap();
	TEST_ASSERT_TRUE(feq(S.ku, 0.8f / 2.71828f));
	/* 遗留槽: arm s=0.5 → 60min 老化 ×1.02^60≈3.28×0.5=1.64→帽 1.5 */
	sm_reset();
	sm_intent_arm(0.5f, 0);
	sm_tick(3600);
	snap();
	TEST_ASSERT_TRUE(S.ku >= 1.49f);   /* 帽 */
	/* IGNORED 清仓(R2): 槽清零+h++(再武装老化除子减半) */
	sm_settle(SM_SETTLE_IGNORED, 3600);
	sm_tick(3700);
	snap();
	TEST_ASSERT_TRUE(S.ku < 0.2f);     /* 槽清零, 仅余辉 */
	sm_intent_arm(0.5f, 4000);
	sm_tick(4000 + 3600);
	snap();
	/* h=1: 0.5×3.28/2≈0.82(无 h 时 1.64 顶帽) */
	TEST_ASSERT_TRUE(S.ku < 1.0f);
	TEST_ASSERT_TRUE(S.ku > 0.7f);
}

/* kf: 非对称 EMA(docs §4.1: 事件值 ±1/+0.5, α_负0.30/α_正0.10)+挫败线 */
static void golden_kf(void)
{
	sm_reset();
	snap();
	TEST_ASSERT_TRUE(feq(S.kf, 0.5f));
	sm_event(SM_EVT_ROUND_FAIL, 100);  /* 0.5+0.3×(−0.5−0.5)=0.2 */
	snap();
	TEST_ASSERT_TRUE(feq(S.kf, 0.2f));
	sm_event(SM_EVT_ROUND_OK, 200);    /* 0.2+0.1×(1−0.2)=0.28 */
	snap();
	TEST_ASSERT_TRUE(feq(S.kf, 0.28f));
	sm_settle(SM_SETTLE_IGNORED, 300); /* 0.28+0.3×(−1.28)<0 → 0 */
	snap();
	TEST_ASSERT_TRUE(feq(S.kf, 0.0f));
	TEST_ASSERT_TRUE(feq(S.e_frustrate, 0.55f));   /* 0.55−0 → 满挫败 */
}

/* kb: 五态(DND/AWAY/BUSY/ATTENDED/FREE) */
static void golden_kb(void)
{
	sm_reset();
	sm_tick(1000);
	snap();
	TEST_ASSERT_TRUE(S.kb == SM_KB_AWAY);          /* 无用户=离开 */
	/* 勿扰最高优先(sm_ctx 驱动五态) */
	sm_ctx(true, false, false);
	sm_tick(2000);
	snap();
	TEST_ASSERT_TRUE(S.kb == SM_KB_DND);
	TEST_ASSERT_TRUE(feq(S.avail, 0.0f));
	/* 勿扰同时是 gate 硬门 */
	struct sm_gate_in in = { .now_s = 2000, .now_min = 300,
				 .arousal = 0.5f, .net_up = true,
				 .quiet_hours = true };
	struct sm_gate_out g;
	sm_gate(&in, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & 1);
	/* 有交互→在场(<45min) */
	sm_ctx(false, false, false);
	sm_event(SM_EVT_WAKE, 3000);
	sm_tick(3100);
	snap();
	TEST_ASSERT_TRUE(S.kb == SM_KB_ATTENDED);
	/* 46min 无交互→FREE */
	sm_tick(3000 + 46 * 60);
	snap();
	TEST_ASSERT_TRUE(S.kb == SM_KB_FREE);
}

/* 仲裁: 唤醒 S=1×M×H 触发+扣令牌; 习惯化压低; 透支; 滴灌 */
static void golden_arbiter_tokens(void)
{
	sm_reset();
	struct sm_gate_out g;
	/* 唤醒(U=1.0, 无偏置 M=1, H=1) → S=1.0>0.5 触发 */
	sm_event(SM_EVT_WAKE, 100);
	TEST_ASSERT_TRUE(gate(100, 10, 0.5f, 0, 0, false, false, true,
			      false, false, false, false, &g));
	TEST_ASSERT_TRUE(g.S > 0.9f);
	TEST_ASSERT_TRUE(g.reason_mask & 1);
	snap();
	TEST_ASSERT_EQUAL_INT(2, S.tokens);            /* 3−1 */
	/* 同 key 去重(5min, key=通道): kd 通道 12min 首发成立, 14min 内
	 * 同通道再发被去重拦(位 4) */
	sm_event(SM_EVT_NONE, 830);
	TEST_ASSERT_TRUE(gate(830, 12, 0.5f, 0, 0, false, false, false,
			      false, true, false, false, &g));
	TEST_ASSERT_TRUE(g.reason_mask & (1 << 3));
	sm_event(SM_EVT_NONE, 900);
	(void)gate(900, 14, 0.5f, 0, 0, false, false, false, false,
		   true, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 4));
	/* 习惯化爆发(多通道 1 分钟内连触发 n=3 → H=1/4 压低 S 通道):
	 * mem 通道 13min 触发后, 14min wake+在场偏置(M=1.4)×H(0.25)
	 * = S 0.35 <0.5 → 全低(位 6) */
	sm_event(SM_EVT_NONE, 840);
	TEST_ASSERT_TRUE(gate(840, 13, 0.5f, 0, 0, false, false, false,
			      false, false, true, false, &g));
	/* 习惯化爆发(独立局): kd+mem 同分钟触发 n=2 → H=1/3 → S key
	 * 空窗的 wake+在场偏置 S=1×1.4×0.33=0.47<0.5 → 全低(位 6) */
	sm_reset();
	sm_event(SM_EVT_NONE, 0);
	TEST_ASSERT_TRUE(gate(60, 1, 0.5f, 0, 0, false, false, false,
			      false, true, false, false, &g));
	TEST_ASSERT_TRUE(gate(60, 1, 0.5f, 0, 0, false, false, false,
			      false, false, true, false, &g));
	sm_event(SM_EVT_WAKE, 120);
	(void)gate(120, 1, 0.5f, 0.8f, 0, true, false, false, false,
		   false, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 6));
	/* 大事件连打(间隔 ≥ 去重窗; 习惯化按分钟恢复 ρ=0.7/min →
	 * 6.7min 后 n 清零, H=1, S=1.0 恒触发; 滴灌 400min≈3.3 枚) */
	sm_reset();
	int fired = 0;
	for (int i = 0; i < 5; i++) {
		sm_event(SM_EVT_WAKE, 100 + (uint32_t)i * 400);
		if (gate(100 + (uint32_t)i * 400, 10 + (uint32_t)i * 400,
			 0.5f, 0, 0, false, false, false, false,
			 false, false, false, &g))
			fired++;
	}
	TEST_ASSERT_TRUE(fired == 5);
	snap();
	/* 透支+滴灌: 30min 间隔(消耗 1 > 滴灌 0.25)把桶压干; S≥0.8 的
	 * wake 可透支到 −1(快照钳显 0); kd 通道(S<0.8)被无令牌拦 */
	sm_reset();
	for (int i = 0; i < 4; i++) {
		sm_event(SM_EVT_WAKE, 100 + (uint32_t)i * 30);
		TEST_ASSERT_TRUE(gate(100 + (uint32_t)i * 30,
				      10 + (uint32_t)i * 30, 0.9f, 0, 0,
				      false, false, false, false, false,
				      false, false, &g));
	}
	sm_event(SM_EVT_WAKE, 130 + 100);
	TEST_ASSERT_TRUE(gate(230, 140, 0.9f, 0, 0, false, false, false,
			      false, false, false, false, &g));   /* 透支 */
	sm_event(SM_EVT_NONE, 400);
	(void)gate(400, 170, 0.5f, 0, 0, false, false, false, false,
		   true, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 1));   /* 无令牌+非透支分 */
	snap();
	TEST_ASSERT_EQUAL_INT(0, S.tokens);            /* 负债钳显 0 */
}

/* 五通道修复: 孤立 idle 不触发/低唤醒降采样/quiet/单飞/忙 */
static void golden_gate_channels(void)
{
	sm_reset();
	struct sm_gate_out g;
	/* 孤立 idle(无事件/无 kc/ku): 全低 */
	(void)gate(1000, 100, 0.5f, 0, 0, false, false, true, false,
		   false, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 6));
	/* 勿扰硬门 */
	(void)gate(2000, 200, 0.5f, 0, 0, false, false, true, true,
		   false, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & 1);
	/* 深思单飞 */
	(void)gate(3000, 300, 0.5f, 0, 0, false, false, true, false,
		   false, false, true, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 5));
	/* kd 跃迁沿通道(mem/kd 直通) */
	sm_event(SM_EVT_NONE, 5000);
	TEST_ASSERT_TRUE(gate(5000, 500, 0.5f, 0, 0, false, false, false,
			      false, true, false, false, &g));
	TEST_ASSERT_TRUE(g.reason_mask & (1 << 3));
	/* 低唤醒降采样: kd 跃迁+arousal<0.1 第二次 5min 内拦 */
	sm_event(SM_EVT_NONE, 6000);
	(void)gate(6000, 600, 0.05f, 0, 0, false, false, false, false,
		   true, false, false, &g);
	sm_event(SM_EVT_NONE, 6600);
	(void)gate(6600, 601, 0.05f, 0, 0, false, false, false, false,
		   true, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 3));
	/* mem_hook 直通 */
	sm_event(SM_EVT_NONE, 20000);
	TEST_ASSERT_TRUE(gate(20000, 2000, 0.5f, 0, 0, false, false, true,
			      false, false, true, false, &g));
	TEST_ASSERT_TRUE(g.reason_mask & (1 << 4));
}

/* salient 成功率门: 挫败 kf=0 → p=0<0.35 拒(说话置信面在出口层) */
static void golden_p_gate(void)
{
	sm_reset();
	sm_event(SM_EVT_ROUND_FAIL, 100);
	sm_event(SM_EVT_ROUND_FAIL, 200);
	sm_event(SM_EVT_ROUND_FAIL, 300);       /* kf=0.5−0.9→0 */
	sm_event(SM_EVT_WAKE, 5000);            /* U=1 → S 高 */
	struct sm_gate_out g;
	(void)gate(5000, 500, 0.5f, 0, 0, false, false, true, false,
		   false, false, false, &g);
	TEST_ASSERT_TRUE(g.blocked_mask & (1 << 2));   /* p=kf×avail 拒 */
	TEST_ASSERT_EQUAL_INT(0, g.reason_mask);
}

/* B-6a: kb net 硬门(说话中/勿扰拒, 其余模型自决; net 仅遥测) */
static void golden_speak_net(void)
{
	sm_reset();
	/* 说话中(ai_active)=∞ 硬门 */
	sm_ctx(false, true, false);
	TEST_ASSERT_FALSE(sm_speak_allow());
	/* 勿扰态拒 */
	sm_ctx(true, false, false);
	sm_tick(60);
	TEST_ASSERT_FALSE(sm_speak_allow());
	/* 平静空闲: 模型自决(ku 低也放行, §55.29 自由度) */
	sm_ctx(false, false, false);
	sm_tick(120);
	TEST_ASSERT_TRUE(sm_speak_allow());
	/* net 遥测值域 sanity: 无用户态 cost=0 → net=ku ∈ [0,1.5] */
	float net = sm_speak_net();
	TEST_ASSERT_TRUE(net >= -1.0f && net <= 1.5f);
}

void motiv_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_kc);
	RUN_TEST(golden_ku);
	RUN_TEST(golden_kf);
	RUN_TEST(golden_kb);
	RUN_TEST(golden_arbiter_tokens);
	RUN_TEST(golden_gate_channels);
	RUN_TEST(golden_p_gate);
	RUN_TEST(golden_speak_net);
	UNITY_END();
}
