/*
 * soul_think.c — 灵魂 2.0 深思轮(W3 意图协议 × EspCube 多行动出口)
 *
 * 移植自 Kaleido tools/hil/convo-stage/main/soul_think.c, 适配 EspCube:
 *   - LLM 通道: brain 回调注入(ai_session_deliberate, 不依赖
 *     speech_provider —— app_soul 组件零语音依赖, 分层纪律);
 *   - 行动扩展: ki_act 的 ACT 词表位启用 —— content "face:段名" 换表情 /
 *     "music:next|random" 音乐; SPEAK 走注入回调(announce 带外);
 *   - 行动广播: face/music 经 CUBE_EVT_SOUL_ACT 广播(执行器订阅,
 *     全局广播空间语义); speak 保留回调(同进程最短路径);
 *   - 结算: speak 播后 20s 观察窗, 用户输入侧记(note_voice)=RESPONDED,
 *     静默=IGNORED → ki_settle2 冷静期翻倍; 非说话行动直接 RESPONDED。
 *
 * 安全边界(Kaleido 29 号 §9 逐条):
 *   - LLM 产出只经 ki_intent_parse 进固件, 无字段可写 ke/ki 内部态;
 *   - 拒非降级: parse 非 OK/非 LONG → 不行动 + 码落痕, 绝不猜;
 *   - 去重窗: content 与上次实播同且 <5min → 拒;
 *   - content 含数字且无 mem 证据 → 拒(UncitedClaim, 防幻觉统计)。
 *
 * 任务纪律: 常驻 think 任务 + 静态内部栈 16KB(TLS 深度, 禁落 PSRAM);
 * notify 门控(soul tick 触发, 不在现场起任务); 单飞(忙则丢弃不排队)。
 */
#include "soul.h"
#include "cube_act.h"
#include "soul_ignition.h"
#include "soul_budget.h"
#include "soul_motiv.h"
#include "soul_jsonfix.h"
#include "soul_bb.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cubecore.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CUBE_ENABLE_SOUL && CONFIG_ESP_CUBE_SOUL_PROACTIVE

#define THINK_STACK        (16 * 1024)
#define THINK_PRIO         4
#define THINK_REPLY_MAX    512
#define THINK_SETTLE_S     20           /* speak 播后用户反应观察窗 */
#define THINK_DEDUP_MIN    5            /* content 去重窗(分钟) */

static const char *TAG = "soulthink";

/* 深思 brain 回调(注入: ai_session_deliberate) */
static int (*s_brain)(const char *system_prompt, char *reply, size_t cap);
/* speak 出口(注入: ai_session_announce) */
static esp_err_t (*s_speak)(const char *text);

static volatile bool s_busy;
static volatile bool s_voice_seen;
static uint8_t s_q_opened;      /* B-6a: 深思问句已开槽位图(kc 台账) */
static volatile bool s_reflect_req;  /* B-6b: 反思轮请求 */
static uint8_t  s_reason;
static bool     s_forced;
static float    s_trig_score;

static uint32_t s_last_think_min;
static uint8_t  s_thought_today;
static bool     s_last_silent;
static float    s_last_silent_score;
static uint32_t s_last_fire_min;
static uint8_t  s_fired_today;
static char     s_last_content[KI_CONTENT_MAX + 1];
static uint32_t s_last_content_min;
static TaskHandle_t s_handle;

/* RTCRAM 定置(docs/22 §3B v6.1): 深思任务为纯 LLM 网络类(非实时/非 DMA/
 * 非 PIE), 栈入 LP SRAM 释放 HP DRAM——v6 全栈入 DRAM 致 dma largest 崩到
 * 8704、taskLVGL 创建失败, 板上 boot loop 实证 */
static StackType_t s_thk_stack[THINK_STACK / sizeof(StackType_t)]
    __attribute__((section(".rtc.bss")));
static StaticTask_t s_thk_tcb;

static uint32_t uptime_min(void)
{
    return (uint32_t)(esp_timer_get_time() / 60000000LL) % 1440u;
}

/* ---- 接线(main.c; brain 必接, 否则深思不触发) ---- */
void soul_think_set_brain(int (*fn)(const char *, char *, size_t))
{
    s_brain = fn;
}

void soul_think_set_speak(esp_err_t (*fn)(const char *))
{
    s_speak = fn;
}

void soul_think_note_voice(void)
{
    s_voice_seen = true;              /* 结算窗证据(用户输入侧记) */
}

bool soul_think_busy(void)
{
    return s_busy;
}

/* 触发闸上下文供给(soul tick 每拍调)。
 * 2026-11-19 修复(模拟器快进一周发现, §55.14 续): thought/fired 两配额
 * 此前只 ++ 无跨天重置——"日帽 12"实为"每次开机终身 12 次", 耗尽后深思
 * 永久沉默(重启才恢复)。在供给点(每拍必经)做日翻转检测: now_min 显著
 * 回绕(过午夜/SNTP 跳变)即清零两配额。 */
/* [S5 退役] ki_think_gate 固定节律触发面由 soul_motiv sm_gate 取代
 * (docs/24 §4.2); 行动配额(fired_today)日翻转改由 soul tick 每拍调
 * soul_think_day_flip(触发间隔>24h 时任务内自检会漏翻)。 */

/* 配额日翻转(now_min=uptime 分; soul tick 每拍调) */
void soul_think_day_flip(uint32_t now_min)
{
    static uint32_t s_flip_min;
    if (s_flip_min != 0 && now_min < s_flip_min &&
        (s_flip_min - now_min) > 12 * 60) {
        s_thought_today = 0;
        s_fired_today = 0;
    }
    s_flip_min = now_min;
}

/* ---- 行动执行 ----
 * T13: 深思行动改经 cube_act 统一门(origin=AUTONOMOUS_LLM)。
 * 旧直发链路因 KI_ACT.content 未填充实为空动作(docs/15); 迁移后
 * 空/非法内容被能力 validate 拒绝并留回执, 断链显性化(T31 收口)。 */
static void act_broadcast(uint8_t kind, const char *content)
{
    cact_req_t r;
    memset(&r, 0, sizeof(r));
    r.origin = CACT_ORIGIN_AUTONOMOUS_LLM;
    if (kind == 3) {
        strlcpy(r.capability, "radio.play", sizeof(r.capability));
    } else {
        strlcpy(r.capability, "face.show", sizeof(r.capability));
        size_t n = strlcpy((char *)r.params, content ? content : "",
                           sizeof(r.params)) + 1;
        r.params_len = n;
    }
    uint32_t id = 0;
    cact_res_t res = cact_submit(cact_default(), &r, &id);
    if (res == CACT_RES_OK) {
        cact_pump(cact_default());
        cact_receipt_t rc;
        if (cact_receipt_last(cact_default(), id, &rc) == CACT_RES_OK) {
            ESP_LOGI(TAG, "think act %s -> #%u %s", r.capability, id,
                     cact_status_name(rc.status));
        }
    } else {
        ESP_LOGW(TAG, "think act %s rejected: %d", r.capability, res);
    }
}

/* ---- 能力注册表(§55.29 B-6 prompt 审阅修订, 用户字段规格:
 * ① 命令 id ② 命令描述(给 LLM 参考工具用途) ③ 参数个数 ④ 参数表) ----
 * 行动词表单一真源: think_prompt 的 act 枚举句/参数规则句、
 * act_normalize 归一、act_execute 分发、分账映射全部表驱动,
 * 新增能力=加一行。输出契约(用户裁决): 单参数命令 content=参数值
 * (无前缀); 归一层把命令 id 折进 content("<id>:<值>")走 kaleido
 * KI_ACT 通道(parse 侧零改动); 多参数时代再升级 params 对象。 */
struct cap_param {
    const char *name;         /* 参数名(params 对象时代启用)        */
    const char *desc;         /* 取值说明(含一个 %s 时由 vocab 拼)  */
    const char *const *vocab; /* 取值词表(NULL=desc 即全文)         */
};

struct cap_cmd {
    const char *id;           /* ① 命令 id(JSON act 枚举名)         */
    const char *desc;         /* ② 命令描述(LLM 参考工具用途)       */
    uint8_t    n_params;      /* ③ 参数个数                         */
    const struct cap_param *params; /* ④ 参数表(NULL=无参数)        */
    uint8_t    kind;          /* 执行: SOUL_ACT 广播 kind(0=kaleido) */
    uint8_t    bcat;          /* 执行: 分账类别 enum sb_cat         */
};

static const char *const s_face_vocab[] = {
    "smile", "laugh", "angry", "sad", "cry", "shy", "shocked", "panic",
    "sigh", "yawn", "asleep", "ponder", "question", "music", "leisure",
    "mock", "snigger", "confident", "investigate", "yummy", "mute",
    "badminton",
    NULL,                    /* 哨兵(板验 P0: 缺席=越界读 rodata 垃圾
                              * 进 vfprintf %s → Load access fault) */
};

static const struct cap_param s_p_speak[] = {
    { "text", "那句话", NULL },
};
static const struct cap_param s_p_face[] = {
    { "segment", "段名(%s)", s_face_vocab },
};
static const struct cap_param s_p_music[] = {
    { "verb", "next(切台)或 random(随便放)", NULL },
};

static const struct cap_cmd s_caps[] = {
    { "speak",       "说一句话",     1, s_p_speak, 0, SB_SPEAK },
    { "stay_silent", "什么都不做",   0, NULL,      0, 0 },
    { "face",        "切换表情动画", 1, s_p_face,  1, SB_FACE },
    { "music",       "放歌或切台",   1, s_p_music, 3, SB_MUSIC },
};
#define CAP_N (sizeof(s_caps) / sizeof(s_caps[0]))

/* act 枚举句: "speak(说一句话)/stay_silent(什么都不做)/…" */
/* 板验 P0 教训(§55.30): 词表数组必须 NULL 哨兵收尾; 组装器的
 * snprintf 返回值(截断时=全文长度)可推 o 越过 n → n-o 下溢→越界
 * 写。两处防线: 每次写入前 clamp 到 n-1, 写后 o 超 n-1 即止。 */
static size_t sn_cat(char *out, size_t n, size_t o, const char *fmt,
                     const char *a1)
{
    if (o >= n - 1 || n == 0)
        return o;
    int w = snprintf(out + o, n - o, fmt, a1);
    if (w < 0)
        return o;
    return o + (size_t)w;
}

static void caps_act_line(char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < CAP_N && o + 1 < n; i++) {
        o = sn_cat(out, n, o, "%s", i ? "/" : "");
        o = sn_cat(out, n, o, "%s", s_caps[i].id);
        o = sn_cat(out, n, o, "(%s)", s_caps[i].desc);
    }
}

/* 参数规则句: "speak=那句话; face=段名(smile|…); music=next(切台)…" */
static void caps_rule_line(char *out, size_t n)
{
    char voc[192];
    size_t o = 0;
    bool first = true;
    for (size_t i = 0; i < CAP_N && o + 1 < n; i++) {
        const struct cap_cmd *c = &s_caps[i];
        if (c->n_params == 0 || c->params == NULL)
            continue;                  /* stay_silent 无参数规则 */
        o = sn_cat(out, n, o, first ? "%s=" : "; %s=", c->id);
        first = false;
        for (uint8_t k = 0; k < c->n_params && o + 1 < n; k++) {
            const struct cap_param *p = &c->params[k];
            if (p->vocab != NULL) {
                size_t v = 0;
                for (size_t w = 0; p->vocab[w] != NULL && v + 1 < sizeof(voc); w++)
                    v = sn_cat(voc, sizeof(voc), v, w ? "|%s" : "%s",
                               p->vocab[w]);
                o = sn_cat(out, n, o, p->desc, voc);  /* desc 含 %s 吃 voc */
            } else {
                o = sn_cat(out, n, o, "%s", p->desc);
            }
        }
    }
}

/* content 前缀 "<命令id>:" → 注册表行(kind!=0); 未知=NULL */
static const struct cap_cmd *cap_of_content(const char *content)
{
    for (size_t i = 0; i < CAP_N; i++) {
        if (s_caps[i].kind == 0)
            continue;
        size_t pl = strlen(s_caps[i].id);
        if (strncmp(content, s_caps[i].id, pl) == 0 && content[pl] == ':')
            return &s_caps[i];
    }
    return NULL;
}

/* 返回 true=行动真实出口执行(S3 审计 P1: 拒绝动词不得烧预算配额) */
static bool act_execute(const struct ki_intent *it)
{
    if (it->act == KI_SPEAK) {
        if (s_speak != NULL) {
            s_speak(it->content);
            return true;
        }
        return false;
    }
    if (it->act == KI_ACT) {
        /* content 已由 act_normalize 折成 "<命令id>:<参数值>" */
        const struct cap_cmd *c = cap_of_content(it->content);
        if (c != NULL && it->content[strlen(c->id) + 1] != '\0') {
            act_broadcast(c->kind, it->content + strlen(c->id) + 1);
            return true;
        }
        ESP_LOGW(TAG, "act verb 未知: %s", it->content);
        return false;
    }
    return false;   /* KI_STAY_SILENT: 无行动(一等输出) */
}

/* ---- 深思 prompt v3.3(S6/D1+§55.29 两轮审阅修订, docs/24 §5.1/§5.4):
 * 背景块六维=人格一句/时空+时段/环境/情绪三段式/记忆闪回(用户选型),
 * 预算叙事移除(多说少说模型自决, sb 记账不执法); 行动词表/参数规则
 * 由能力注册表生成; content=纯参数值(命令 id 由归一层折入分发)。
 * JSON schema 契约与 v2 一致(容错恢复面=soul_jsonfix)。 */
static void think_prompt(char *buf, size_t n)
{
    soul_state_t st;
    if (!soul_state_get(&st)) {
        buf[0] = '\0';
        return;
    }
    /* 感受三段式: 强度三档(0.33/0.66 分位)+唤醒分档的桌面化感受词 */
    static const char *lv_pos[3] = { "有点舒心", "挺开心", "非常雀跃" };
    static const char *lv_neg[3] = { "有点低落", "挺低落", "非常低落" };
    static const char *lv_mid[3] = { "平静", "平静", "有点坐立不安" };
    const char *feel;
    if (st.valence > 0.15f)
        feel = lv_pos[st.valence > 0.66f ? 2 : (st.valence > 0.33f ? 1 : 0)];
    else if (st.valence < -0.15f)
        feel = lv_neg[st.valence < -0.66f ? 2 : (st.valence < -0.33f ? 1 : 0)];
    else
        feel = lv_mid[st.arousal > 0.55f ? 2 : 0];
    /* 变化五档(与上次深思比; 无历史=说"没什么变化") */
    static float s_prev_val;
    static bool s_prev_seen;
    bool first = !s_prev_seen;          /* 先取基线位再更新(P2-2 时序) */
    float dv = st.valence - (s_prev_seen ? s_prev_val : 0.0f);
    s_prev_val = st.valence;
    s_prev_seen = true;
    const char *chg = first || fabsf(dv) < 0.03f ? "心情没什么变化"
                      : dv > 0.15f ? "心情明显好了起来"
                      : dv > 0.03f ? "心情略有好转"
                      : dv < -0.15f ? "心情明显沉了一些"
                                     : "心情略微沉了些";
    /* 背景块六维(§55.29 用户选型: 人格/时空+时段/环境/情绪/记忆闪回;
     * 预算叙事已移除——多说少说由模型自决)。act 枚举句/参数规则句由
     * 能力注册表生成(不写死于模板) */
    char acts[128], rules[336];
    caps_act_line(acts, sizeof(acts));
    caps_rule_line(rules, sizeof(rules));
    /* ① 人格一句(空=桌面伙伴兜底) */
    const char *who = st.persona_desc[0] ? st.persona_desc : "桌面伙伴";
    /* ② 时段(SNTP 未同步=省略; 不给数值只给词) */
    char tod[24] = "";
    time_t wall = time(NULL);
    if (wall > 1700000000LL) {        /* 已过 2023-11: 视为已校时 */
        struct tm tmv;
        localtime_r(&wall, &tmv);
        const char *w =
            tmv.tm_hour < 5 ? "深夜" : tmv.tm_hour < 8 ? "清晨" :
            tmv.tm_hour < 11 ? "上午" : tmv.tm_hour < 13 ? "中午" :
            tmv.tm_hour < 17 ? "下午" : tmv.tm_hour < 19 ? "傍晚" :
            tmv.tm_hour < 23 ? "晚上" : "深夜";
        snprintf(tod, sizeof(tod), ", 现在是%s", w);
    }
    /* ③ 环境(黑板 playing 键) */
    const char *env = soul_bb_get(SOUL_BB_PLAYING) > 0.5f
                          ? "房间里正在放音乐" : "房间里很安静";
    /* ⑥ 记忆块 top-3(§55.29 调研定案: 多条加权召回, 非单条;
     * 空库=省略整块) */
    char mem[160];
    const char *mem_line =
        soul_mem_recall(mem, sizeof(mem), st.valence) > 0 ? mem : NULL;
    snprintf(buf, n,
        "[主动行为决策·背景块] 这是背景信息, 不是台词, 不要朗读或解释。\n"
        "你是%s。\n"
        "此刻没有人在和你说话, 已安静%d秒%s。\n"
        "%s。\n"
        "内在状态: %s, 你现在%s。\n"
        "%s"
        "%s%s"
        "stay_silent 是默认动作: 只有当下状态给出充分理由时才行动。"
        "只输出一行 JSON, 格式: {\"act\":\"...\",\"content\":\"...\","
        "\"confidence\":0.8,\"evidence\":[\"affect:mood\"],"
        "\"reason\":\"一句话理由\"}。act 只能是: %s。"
        "content 规则: %s。content "
        "不要出现数字。evidence 至少一条, 前缀只能是 drive|affect|mem|dialog。",
        who,
        st.idle_sec, tod,
        env,
        chg, feel,
        mem_line ? mem_line : "",
        st.audio_user_stopped
            ? "用户最近手动停止过音乐, 不要放歌/切台。"
            : "",
        st.ethics_latched ? "现在只适合温柔安抚的语气。" : "",
        acts, rules);
}

/* act 名归一(§55.29 注册表驱动): kaleido ki_intent_parse 只认
 * speak/stay_silent(其余 act 是词表位); 注册表能力(id kind!=0)归一为
 * "act", 同时把命令 id 折进 content 值前缀("laugh"→"face:laugh")作为
 * 分发标记——输出契约保持纯参数值, parse 侧零改动。
 * S3 审计 P0 教训沿用: 替换串短 1B 手术 + 显式长度, 不 memcpy NUL。 */
static void act_normalize(char *reply, size_t cap)
{
    for (size_t i = 0; i < CAP_N; i++) {
        if (s_caps[i].kind == 0)
            continue;                  /* speak/stay_silent kaleido 原生 */
        char pat[24];
        int pn = snprintf(pat, sizeof(pat), "\"act\":\"%s\"", s_caps[i].id);
        if (pn <= 0 || (size_t)pn >= sizeof(pat))
            continue;                  /* id 过长: 该能力归一禁用(护栏) */
        char *p = strstr(reply, pat);
        if (p == NULL)
            continue;
        /* ① act 值 → "act"(等长差收缝; 独立复现验证: 5 字符 id 如
         * music 用旧式 memcpy(oldn-1) 会连 NUL 复制截断 JSON——
         * S3 P0 同病灶, 手术必须按替换串真实长度) */
        size_t oldn = (size_t)pn;
        static const char rep[] = "\"act\":\"act\"";
        const size_t rl = sizeof(rep) - 1;
        if (oldn < rl)
            continue;      /* id 短于 4 字符: 手术不适用, 该能力拒收 */
        memmove(p + rl, p + oldn, strlen(p + oldn) + 1);
        memcpy(p, rep, rl);
        /* ② content 参数值前折进 "<id>:"(单对象输出, 一次即止) */
        char *q = strstr(reply, "\"content\":\"");
        if (q == NULL)
            break;
        char *val = q + strlen("\"content\":\"");
        size_t idl = strlen(s_caps[i].id);
        if (strncmp(val, s_caps[i].id, idl) == 0 && val[idl] == ':')
            break;                     /* 模型已带前缀(旧习): 不重复折 */
        if (strlen(reply) + idl + 1 >= cap)
            break;                     /* 缓冲余量不足: 放弃折入(
                                          分发失败→拒→silent, 安全侧) */
        memmove(val + idl + 1, val, strlen(val) + 1);
        memcpy(val, s_caps[i].id, idl);
        val[idl] = ':';
        break;
    }
}

/* B-6a: 用户轮答=深思遗留问句闭合(kc 台账清源, soul.c ROUND_OK 拍调) */
void soul_think_round_answered(void)
{
    for (uint8_t q = 0; q < 8; q++) {
        if (s_q_opened & (1u << q)) {
            sm_question_close(q);
            s_q_opened &= (uint8_t)~(1u << q);
        }
    }
}

static void think_reflect_run(void);   /* B-6b: 定义在 fire 区 */

/* ---- 深思任务(常驻, notify 门控, 单飞) ---- */
static void think_task(void *arg)
{
    static char prompt[1600];
    static char reply[THINK_REPLY_MAX];

    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_reflect_req) {           /* B-6b: 反思轮优先(独立流程) */
            s_reflect_req = false;
            think_reflect_run();
            s_busy = false;
            continue;
        }
        struct ki_intent intent;
        struct ki_decision dec;
        enum ki_parse_err pe;
        uint32_t now = uptime_min();

        reply[0] = 0;
        if (s_brain == NULL) {
            /* 无 brain: 不盲动(接线纪律) */
            goto out;
        }
        think_prompt(prompt, sizeof(prompt));
        ESP_LOGI(TAG, "THINK reason=%u forced=%d score=%.2f",
                 s_reason, s_forced, s_trig_score);
        if (s_brain(prompt, reply, sizeof(reply)) != 0 || !reply[0]) {
            ESP_LOGW(TAG, "LLMFAIL deliberation");
            ki_settle2(KI_SETTLE_SILENT);
            sm_settle(SM_SETTLE_SILENT, (uint32_t)(esp_timer_get_time()
                                                       / 1000000LL));
            s_last_silent = true;
            goto out;
        }
        ESP_LOGI(TAG, "raw: %.100s", reply);
        /* D2 容错恢复(散文剥离/截断补齐/UTF-8 修边; 恢复失败=silent
         * 兜底, docs/24 §5.2), 成功后在恢复缓冲上做词表规约+解析 */
        static char fixed[THINK_REPLY_MAX];
        size_t fixed_len = 0;
        if (!soul_json_fix(reply, strlen(reply), fixed, sizeof(fixed),
                           &fixed_len)) {
            ESP_LOGW(TAG, "JSONFIX fail → silent");
            ki_settle2(KI_SETTLE_SILENT);
            sm_settle(SM_SETTLE_SILENT, (uint32_t)(esp_timer_get_time()
                                                       / 1000000LL));
            s_last_silent = true;
            goto out;
        }
        act_normalize(fixed, sizeof(fixed));
        fixed_len = strlen(fixed);   /* normalize 改写, 刷新(P2-6) */

        pe = ki_intent_parse(fixed, fixed_len, &intent);
        if (pe != KI_PARSE_OK && pe != KI_PARSE_CONTENT_LONG) {
            ESP_LOGW(TAG, "REJECT parse=%d", (int)pe);   /* 拒非降级 */
            ki_settle2(KI_SETTLE_SILENT);
            sm_settle(SM_SETTLE_SILENT, (uint32_t)(esp_timer_get_time()
                                                       / 1000000LL));
            s_last_silent = true;
            goto out;
        }
        if (intent.act == KI_STAY_SILENT ||
            intent.confidence < KI_CONF_FLOOR) {
            ESP_LOGI(TAG, "SILENT conf=%.2f reason=%.32s",
                     intent.confidence, intent.reason);
            ki_settle2(KI_SETTLE_SILENT);
            sm_settle(SM_SETTLE_SILENT, (uint32_t)(esp_timer_get_time()
                                                       / 1000000LL));
            s_last_silent = true;
            s_last_silent_score = s_trig_score;
            goto out;
        }
        /* 去重窗 */
        if (strncmp(intent.content, s_last_content,
                    sizeof(intent.content)) == 0 &&
            now - s_last_content_min < THINK_DEDUP_MIN) {
            ESP_LOGW(TAG, "REJECT duplicate");
            ki_settle2(KI_SETTLE_SILENT);
            s_last_silent = true;
            goto out;
        }

        /* 出口硬门(只门动作不门思维; forced 落痕越否决) */
        {
            soul_state_t st = { 0 };
            if (!soul_state_get(&st)) {
                /* 快照锁超时: 保守默认(会话空闲=可被打扰门拦/无闩锁) */
                ESP_LOGW(TAG, "snapshot timeout, gate degraded");
            }
            struct ki_fire_ctx fc = {
                .user_speaking = !st.ai_idle,
                .ethics_latched = st.ethics_latched,
                .tts_ready = s_speak != NULL,
                .now_min = now,
                .last_fire_min = s_last_fire_min,
                .fired_today = s_fired_today,
            };
            ki_intent_fire(&intent, &fc, ki_config_default(), &dec);
        }
        /* 分账遥测(S3→§55.29 用户裁决: 记账不执法——是否多说少说由
         * 模型自决, 产品不面向大众, sb 仅作观察数据, 不再拦截)。
         * user-stop 抑制窗内仍拒 music: 尊重用户显式动作, 非频率限制。 */
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        int bcat = intent.act == KI_SPEAK ? SB_SPEAK : SB_FACE;
        if (intent.act == KI_ACT) {
            const struct cap_cmd *c = cap_of_content(intent.content);
            if (c != NULL)
                bcat = c->bcat;      /* 注册表分账映射(§55.29) */
        }
        if (bcat == SB_MUSIC) {
            soul_state_t st2 = { 0 };
            (void)soul_state_get(&st2);
            if (st2.audio_user_stopped && !s_forced) {
                ESP_LOGW(TAG, "music: user-stop window");
                ki_settle2(KI_SETTLE_GATE_BLOCKED);
                s_last_silent = true;
                goto out;
            }
        }
        if (dec.gates.gate_mask && !s_forced) {
            ESP_LOGW(TAG, "GATE fire_mask=%u", dec.gates.gate_mask);
            ki_settle2(KI_SETTLE_GATE_BLOCKED);
            s_last_silent = true;      /* 同 P1: 门拒=本轮回沉默 */
            goto out;
        }

        /* 执行 */
        ESP_LOGI(TAG, "EXEC act=%d %s (%.2f) %s",
                 (int)intent.act, intent.content, intent.confidence,
                 intent.reason);
        /* kb net 硬门(B-6a): 说话中/勿扰态拒 speak(其余模型自决,
         * net 遥测留痕); 被拒=本轮回沉默(非降级) */
        if (intent.act == KI_SPEAK && !sm_speak_allow()) {
            ESP_LOGW(TAG, "speak: kb net gate(说话中/勿扰), net=%.2f",
                     sm_speak_net());
            ki_settle2(KI_SETTLE_GATE_BLOCKED);
            s_last_silent = true;
            goto out;
        }
        bool executed = act_execute(&intent);
        if (executed)
            sb_charge(bcat, now_s);    /* 记账只对真实出口(P1: 拒动词不烧配额) */
        if (intent.act != KI_SPEAK) {
            /* 非说话行动无"被回应"语义, 直接结算 */
            ki_settle2(KI_SETTLE_RESPONDED);
            strlcpy(s_last_content, intent.content, sizeof(s_last_content));
            s_last_content_min = now;
            goto settled;
        }
        /* kc 台账喂入(B-6a): speak 以问句收尾=遗留问题开槽(蒸发 τ8h,
         * 用户下一轮对话即闭合; docs/24 §4.1 待解问题台账) */
        size_t clen = strlen(intent.content);
        if (clen > 0 && (intent.content[clen - 1] == '?' ||
                         strstr(intent.content, "？") != NULL)) {
            for (uint8_t q = 0; q < 8; q++) {
                if (sm_question_open(q, 0.2f, now_s)) {
                    s_q_opened |= 1u << q;
                    ESP_LOGI(TAG, "kc question slot %u open", q);
                    break;
                }
            }
        }
        /* speak: 播后观察窗 */
        s_voice_seen = false;
        for (int i = 0; i < THINK_SETTLE_S * 5; i++) {
            if (s_voice_seen) break;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (s_voice_seen) {
            ki_settle2(KI_SETTLE_RESPONDED);
            sm_settle(SM_SETTLE_RESPONDED, (uint32_t)(esp_timer_get_time()
                                                          / 1000000LL));
            ESP_LOGI(TAG, "SETTLE responded");
        } else {
            ki_settle2(KI_SETTLE_IGNORED);
            sm_settle(SM_SETTLE_IGNORED, (uint32_t)(esp_timer_get_time()
                                                        / 1000000LL));
            /* ku 遗留意图武装(B-6a): 说了没人应=意图挂账, 老化增势
             * (×1.02^Δmin)至下次冲动窗; 用户再回应即清(R2) */
            sm_intent_arm(0.5f, (uint32_t)(esp_timer_get_time()
                                              / 1000000LL));
            ESP_LOGW(TAG, "SETTLE ignored, cooldown=%umin",
                     ki_cooldown_current());
        }
        strlcpy(s_last_content, intent.content, sizeof(s_last_content));
        s_last_content_min = now;

settled:
        s_fired_today++;
        s_last_fire_min = now;
        s_last_silent = false;

out:
        s_thought_today++;
        s_last_think_min = now;
        s_voice_seen = false;
        s_busy = false;
    }   /* 回 notify 门控待命 */
}

esp_err_t soul_think_start(void)
{
    if (s_handle != NULL) return ESP_OK;
    s_handle = xTaskCreateStatic(think_task, "think",
                                 THINK_STACK / sizeof(StackType_t),
                                 NULL, THINK_PRIO, s_thk_stack, &s_thk_tcb);
    if (s_handle == NULL) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "think task up (deliberation loop)");
    return ESP_OK;
}

/* ---- B-6b 反思轮(慢通道, docs/24 §3.5/§11.6 参数层) ----
 * 触发(soul tick: Σimp≥60 ∨ surprise≥40 + 24h 冷却) → 本轮:
 * ≤15 条记忆 → LLM 洞察(≤5)+五维微调 → trait_reflect_apply 五门
 * 执法 → CUBE_EVT_TRAIT_EVO 广播(act_gate 落 persona 卡)。
 * 解析成功即消费触发面(mark+reset; LLM 失败保留待冷却后重试)。 */
static void think_reflect_run(void)
{
    static char pbuf[2600];
    static char reply[THINK_REPLY_MAX];
    static char fixed[THINK_REPLY_MAX];

    soul_state_t st;
    const char *who = soul_state_get(&st) && st.persona_desc[0]
                          ? st.persona_desc : "桌面伙伴";
    char mems[1200];
    int n = soul_mem_recent(mems, sizeof(mems), 15);
    if (n <= 0) {
        ESP_LOGW(TAG, "REFLECT: no memories, skip");
        return;
    }
    snprintf(pbuf, sizeof(pbuf),
        "[性格反思·背景块] 这是背景信息, 不是台词, 不要朗读或解释。\n"
        "你是%s。\n"
        "最近你们一起经历了这些(按时间):\n"
        "%s"
        "请安静回顾这段相处, 只输出一行 JSON, 格式: "
        "{\"insights\":[\"一句话洞察\",最多5条],\"trait\":{\"C\":0.0,"
        "\"W\":0.0,\"E\":0.0,\"A\":0.0,\"R\":0.0},\"conf\":0.7,"
        "\"evidence_n\":2}。trait=你对自己性格的五维微调建议: C=好奇 "
        "W=温情 E=活力 A=主见 R=韧性; 每维 -0.05~0.05, 没有经历依据的"
        "维度必须给 0。insights=这段经历让你对用户或自己的新认识(一句话"
        "一条, 最多5条)。evidence_n=支撑洞察的记忆条数。conf=确信度 0~1。",
        who, mems);

    reply[0] = 0;
    if (s_brain == NULL ||
        s_brain(pbuf, reply, sizeof(reply)) != 0 || !reply[0]) {
        ESP_LOGW(TAG, "REFLECT: LLMFAIL (触发面保留)");
        return;                        /* 失败不消费, 冷却后重试 */
    }
    size_t flen = 0;
    if (!soul_json_fix(reply, strlen(reply), fixed, sizeof(fixed),
                       &flen)) {
        ESP_LOGW(TAG, "REFLECT: jsonfix fail (触发面保留)");
        return;
    }
    cJSON *root = cJSON_Parse(fixed);
    if (root == NULL) {
        ESP_LOGW(TAG, "REFLECT: parse fail (触发面保留)");
        return;
    }
    struct trait_reflect r;
    memset(&r, 0, sizeof(r));
    r.ref_depth = 1;
    static const char *const keys[TRAIT_DIM] = { "C", "W", "E", "A", "R" };
    const cJSON *tr = cJSON_GetObjectItem(root, "trait");
    for (int d = 0; d < TRAIT_DIM && tr != NULL; d++) {
        const cJSON *it = cJSON_GetObjectItem(tr, keys[d]);
        if (it != NULL && cJSON_IsNumber(it))
            r.delta[d] = (float)it->valuedouble;
    }
    const cJSON *cf = cJSON_GetObjectItem(root, "conf");
    if (cf != NULL && cJSON_IsNumber(cf))
        r.conf = (float)cf->valuedouble;
    r.conf = r.conf < 0.0f ? 0.0f : (r.conf > 1.0f ? 1.0f : r.conf);
    const cJSON *ev = cJSON_GetObjectItem(root, "evidence_n");
    if (ev != NULL && cJSON_IsNumber(ev) && ev->valuedouble > 0)
        r.evidence_n = (uint8_t)(ev->valuedouble > 15 ? 15
                                                      : ev->valuedouble);
    /* 洞察留痕(v1: 控制台; psn_evo_rev_t 正式落库挂透明性批) */
    char i0[56] = "";
    const cJSON *ins = cJSON_GetObjectItem(root, "insights");
    int ins_n = 0;
    if (cJSON_IsArray(ins)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, ins) {
            if (!cJSON_IsString(it) || it->valuestring == NULL) continue;
            if (ins_n == 0)
                strlcpy(i0, it->valuestring, sizeof(i0));
            ESP_LOGI(TAG, "REFLECT insight[%d]: %.90s", ins_n,
                     it->valuestring);
            if (++ins_n >= 5) break;
        }
    }
    cJSON_Delete(root);

    float before[TRAIT_DIM], after[TRAIT_DIM];
    trait_get(before);
    bool moved = trait_reflect_apply(&r);
    soul_mem_imp_mark();               /* 消费触发面(反思已发生) */
    sm_surprise_reset();
    if (!moved) {
        ESP_LOGI(TAG, "REFLECT no move (门拒: conf=%.2f evid=%u)",
                 r.conf, r.evidence_n);
        return;
    }
    trait_get(after);
    cube_trait_evo_t evo;
    for (int d = 0; d < TRAIT_DIM; d++) {
        evo.trait[d] = after[d];
        evo.applied[d] = after[d] - before[d];
    }
    snprintf(evo.reason, sizeof(evo.reason), "反思: %.40s", i0);
    cube_event_publish(CUBE_EVT_TRAIT_EVO, &evo, sizeof(evo));
    ESP_LOGI(TAG, "REFLECT applied C%+.3f W%+.3f E%+.3f A%+.3f R%+.3f",
             evo.applied[0], evo.applied[1], evo.applied[2],
             evo.applied[3], evo.applied[4]);
}

/* soul tick 反思触发入口(B-6b; 忙则丢弃, tick 下拍重试) */
bool soul_think_reflect_fire(void)
{
    if (s_busy || s_handle == NULL) return false;
    s_reflect_req = true;
    s_busy = true;
    xTaskNotifyGive(s_handle);
    return true;
}

/* soul tick 触发入口(gate 过后调; 忙则丢弃 —— 思维不排队) */
bool soul_think_fire(uint8_t reason_mask, bool forced, float score)
{
    if (s_busy || s_handle == NULL) return false;
    s_reason = reason_mask;
    s_forced = forced;
    s_trig_score = score;
    s_busy = true;
    xTaskNotifyGive(s_handle);
    return true;
}

#else /* 未启用 proactive: 空实现(链接面不变) */

void soul_think_set_brain(int (*fn)(const char *, char *, size_t)) { (void)fn; }
void soul_think_set_speak(esp_err_t (*fn)(const char *)) { (void)fn; }
void soul_think_note_voice(void) {}
bool soul_think_busy(void) { return false; }
void soul_think_day_flip(uint32_t now_min) { (void)now_min; }

esp_err_t soul_think_start(void) { return ESP_OK; }
bool soul_think_fire(uint8_t r, bool f, float s)
{ (void)r; (void)f; (void)s; return false; }
bool soul_think_reflect_fire(void) { return false; }

#endif
