/*
 * soul_motiv.h — 六维动机扩展面: kc/ku/kf/kb 四维 + 显著性仲裁器 +
 * 令牌桶(S5/B-3, docs/24 §4.1/§4.2/§4.5)
 *
 * 与 kd(soul_drive 三水库)的关系: kd 是内稳态水库(喂 ke), 本件是
 * "何时触发深思"的动机-仲裁层(喂 think gate); kb 礼貌闸的可用度
 * avail 同时供行动出口的 TTS 门执法面(ki_intent_fire)。
 * 行为来源大原则(R6): 本件只决定"是否触发深思", 不生成行动;
 * 令牌桶=思考预算(日 12), 行动分账(speak/music/face)在 soul_budget(S3)。
 * 触发侧替换 soul_ignition 的 ki_think_gate(固定 10min 节律退役,
 * docs/24 §4.2); ki_intent_parse/fire/settle2(行动面)不动。
 *
 * 事件源纪律(L7 五通道修复, docs/24 §4.2): new_event 只收仲裁打分
 * S>0.5 的事件; 孤立 idle 不触发(须 ku>0.8+静默窗); low_arousal 检查
 * 降采样 1/5min; bin 通道=kd sev 跃迁沿(调用方边沿检测); mem_hook
 * 仅用户锚定时刻(B-6 前恒 false)。
 * 纯 C 零平台依赖(宿主可测); 确定性: 零 RNG/零分配/零墙钟(全部由
 * 调用方 now_s/now_min 单调供给)。
 */
#ifndef SOUL_MOTIV_H
#define SOUL_MOTIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- kc: 待解问题台账(8 槽, docs/24 §4.1) ---- */
#define SM_QSLOTS 8

/* 动机事件源(1Hz tick 侧注入; 与 kd_event 平行) */
enum sm_evt {
    SM_EVT_NONE = 0,
    SM_EVT_WAKE,        /* 唤醒命中(U=1.0; ku 余辉 +0.6; kc KL 记账) */
    SM_EVT_BARGE,       /* 打断(U=0.8; ku 余辉 +0.8)                  */
    SM_EVT_TOUCH,       /* 触摸(U=0.35; ku 余辉 +0.2)                 */
    SM_EVT_PLAY_CHG,    /* 播放切换(U=0.5)                            */
    SM_EVT_NET,         /* 网络边沿(U=0.35)                           */
    SM_EVT_ROUND_OK,    /* 轮成功(U=—; kf +1)                         */
    SM_EVT_ROUND_FAIL,  /* 轮失败(U=0.6; kf −0.5)                     */
    SM_EVT_SILENCE_WIN, /* 10s 静默窗满(on_pause; ku 结算挂点)        */
    SM_EVT_LONG_SILENCE, /* 长静默(每满 2h 一次, U=0.30; 调用方标记)  */
    SM_EVT_CNT
};

/* 轮结算(kf 非对称 EMA; 与 ki_settle2 平行的动机面记账) */
enum sm_settle {
    SM_SETTLE_RESPONDED,    /* +1 */
    SM_SETTLE_SILENT,       /* +0.5 */
    SM_SETTLE_IGNORED,      /* −1 */
};

struct sm_cfg {
    /* kc */
    float q_tau_min;        /* 槽强蒸发 τ(480min)            */
    float absence_cap;      /* 缺席缺口槽权重封顶(0.25)      */
    float kc_gate;          /* kc 触发深思门(0.45)           */
    float skl_gate;         /* s_kl 惊奇事件门(0.3)          */
    float skl_gain;         /* 惊奇事件 kc 增益系数(0.4)     */
    /* ku */
    float ku_grow;          /* 槽老化底数(ln 增益: ×1.02^Δmin) */
    float ku_thresh;        /* 触发阈(0.8)                   */
    float ku_cap;           /* 上限(1.5)                     */
    float ku_after_tau_s;   /* 事件余辉 τ(600s)              */
    float ku_wake;          /* 唤醒余辉(0.6)                 */
    float ku_barge;         /* 打断余辉(0.8)                 */
    float ku_touch;         /* 触摸余辉(0.2)                 */
    uint32_t ku_silence_s;  /* 触发需静默窗(10s)             */
    /* kf */
    float kf_an, kf_ap;     /* 非对称 EMA(α负0.30/α正0.10)   */
    float kf_frustrate;     /* 挫败线(0.55): e_f=max(0,·−kf) */
    /* kb */
    uint32_t kb_away_min;   /* 离开态阈(120min)              */
    uint32_t kb_attend_min; /* 在场态阈(45min)               */
    float kb_cost_free;     /* 空闲成本(1.0)                 */
    float kb_cost_attended; /* 在场成本(0.6)                 */
    float kb_p_gate;        /* salient 深思成功率门(0.35)    */
    /* 仲裁器+令牌桶 */
    float s_gate;           /* S 触发门(0.5)                 */
    float s_overdraft;      /* 透支门(0.8)                   */
    uint8_t tb_capacity;    /* 令牌桶容量(3)                 */
    uint8_t tb_daily;       /* 日补令牌(12)                  */
    uint32_t dedup_min;     /* 同 key 去重窗(5min)           */
    float habit_recover;    /* 习惯化自发恢复 ρ(/min, 0.7)   */
};

/* kb 礼貌闸五态(全由真实事件推出, docs/24 §4.1) */
enum sm_kb {
    SM_KB_DND = 0,          /* 勿扰                             */
    SM_KB_AWAY,             /* 离开>120min                      */
    SM_KB_BUSY,             /* 忙(会话/播放中)                  */
    SM_KB_ATTENDED,         /* 在场<45min                       */
    SM_KB_FREE,             /* 空闲                             */
};

struct sm_state {
    float kc;               /* [0,1] 好奇                       */
    float ku;               /* [0,1.5] 冲动                     */
    float kf;               /* [0,1] 胜任                       */
    enum sm_kb kb;
    float avail;            /* kb 可用度[0,1](DND=0)            */
    float e_frustrate;      /* max(0, 0.55−kf)                  */
    float s_kl;             /* 最近一次交互间隔 KL 惊奇[0,1]    */
    uint8_t tokens;         /* 令牌桶余量                       */
};

/* 复位(中性: 桶满/台账空/kf=0.5) */
void sm_reset(void);

/* 事件注入(1Hz tick 侧; 至多一个, 多事件分拍) */
void sm_event(enum sm_evt e, uint32_t now_s);

/* 轮结算(kf 非对称 EMA; IGNORED 同时清 ku 槽=R2 被忽即清仓) */
void sm_settle(enum sm_settle s, uint32_t now_s);

/* kc 台账挂点(B-6 深思遗留问题; 缺席缺口自动开槽=权重封顶)。
 * w=开槽强(蒸发基准); open_t0_s=开槽单调秒(蒸发时基) */
bool sm_question_open(uint8_t slot, float w, uint32_t open_t0_s);
void sm_question_close(uint8_t slot);

/* ku 遗留意图槽武装(B-6: 深思产出的未执行意图; s=意图强[0,1])。
 * IGNORED 结算即清仓(R2 被忽即清仓, docs/24 §4.1) */
void sm_intent_arm(float s, uint32_t now_s);

/* 本拍环境供给(1Hz, 先于 sm_tick; quiet/会话/播放=礼貌闸输入源) */
void sm_ctx(bool quiet_hours, bool ai_active, bool playing);

/* 周期推进(1Hz; 槽蒸发/余辉衰减/KL 冻结由日翻转触发) */
void sm_tick(uint32_t now_s);

/* 状态快照(只读) */
void sm_snapshot(struct sm_state *out);

/* ---- 显著性仲裁器+深思触发(替换 ki_think_gate, docs/24 §4.2) ----
 * S = U(事件突发度表) × M(动机偏置) × H(习惯化 1/(1+n));
 * 触发 = (S 门 ∧ 令牌桶>0 ∧ kb 允许) ∨ (kc 门 ∧ s_kl 门) ∨
 *        (ku 门 ∧ 静默窗满) ∨ kd 跃迁 ∨ mem_hook;
 * 扣令牌(触发即扣; S≥透支门可透支 1)。
 * 五通道修复内建: 孤立 idle 需 ku; low_arousal 降采样 1/5min;
 * new_event 只收 U≥0.5 事件; mem_hook 原样透传(调用方锚定)。 */
struct sm_gate_in {
    uint32_t now_s;
    uint32_t now_min;       /* 单调分(跨天检测/习惯化恢复) */
    float arousal;          /* ke arousal(低唤醒降采样)    */
    float e_social;         /* kd error social(M 偏置)     */
    float e_stim;           /* kd error stim(M 偏置)       */
    bool ai_active;         /* 用户域在场                   */
    bool playing;           /* 播放域在场                   */
    bool net_up;            /* 网络域                       */
    bool quiet_hours;       /* 勿扰(硬门)                   */
    bool kd_sev_jump;       /* kd sev 跃迁沿(调用方边沿)    */
    bool mem_hook;          /* 用户锚定时刻(B-6 前恒 false) */
    bool thinking;          /* 深思在途(单飞, 调用方供给)   */
};
struct sm_gate_out {
    bool trigger;
    uint8_t reason_mask;    /* bit0=S 仲裁 1=kc 2=ku 3=kd跃迁 4=mem */
    uint8_t blocked_mask;   /* bit0=quiet 1=无令牌 2=kb 拒 3=降采样
                             * 4=去重 5=单飞 6=S/kc/ku 全低    */
    float S;                /* 本拍仲裁分(遥测)             */
};
void sm_gate(const struct sm_gate_in *in, struct sm_gate_out *out);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_MOTIV_H */
