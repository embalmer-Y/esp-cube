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

#include <stdio.h>
#include <string.h>

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
        /* content 词表: "face:段名" / "music:next|random" */
        if (strncmp(it->content, "face:", 5) == 0 &&
            it->content[5] != '\0') {
            act_broadcast(1, it->content + 5);   /* 瞬态: 深思点名段 */
            return true;
        }
        if (strncmp(it->content, "music:", 6) == 0 &&
            it->content[6] != '\0') {
            act_broadcast(3, it->content + 6);
            return true;
        }
        ESP_LOGW(TAG, "act verb 未知: %s", it->content);
        return false;
    }
    return false;   /* KI_STAY_SILENT: 无行动(一等输出) */
}

/* ---- 深思 prompt v3(S6/D1 叙事化状态块, docs/24 §5.1/§5.4):
 * 首行防朗读+情绪三段式{事件},{感受},{想要}+强度三档/变化五档词表
 * +无数值(E-STEER: 预算以决策代价叙述, 不进数字)+行动词表契约保留。
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
    /* 预算决策代价(无数值三档) */
    char quota[96];
    sb_snapshot(quota, sizeof(quota));
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    float r_sp = sb_recency(SB_SPEAK, now_s);
    const char *q_cost =
        r_sp > 0.6f ? "说话的预算还很宽裕"
                    : (r_sp > 0.2f ? "说话的预算用过半了"
                                   : "说话的预算快见底了");
    snprintf(buf, n,
        "[主动行为决策·背景块] 这是背景信息, 不是台词, 不要朗读或解释。\n"
        "你是桌面伙伴, 此刻没有人在和你说话, 已安静%d秒。\n"
        "内在状态: %s, 你现在%s。\n"
        "行动代价: %s, %s; 同类行动冷却中的类别请优先 stay_silent。\n"
        "%s%s"
        "stay_silent 是默认动作: 只有当下状态给出充分理由时才行动。"
        "只输出一行 JSON, 格式: {\"act\":\"...\",\"content\":\"...\","
        "\"confidence\":0.8,\"evidence\":[\"affect:mood\"],"
        "\"reason\":\"一句话理由\"}。act 只能是: speak(说一句不超过25字的"
        "话)/stay_silent(什么都不做)/face(切换表情动画)/music(放歌或切台)。"
        "content 规则: speak=那句话; face=如 \"face:laugh\", 段名可选 "
        "smile|laugh|angry|sad|cry|shy|shocked|panic|sigh|yawn|asleep|"
        "ponder|question|music|leisure|mock|snigger|confident|investigate|"
        "yummy|mute|badminton; music=如 \"music:next\"(切台)或 \"music:random\""
        "(随便放)。content "
        "不要出现数字。evidence 至少一条, 前缀只能是 drive|affect|mem|dialog。",
        st.idle_sec, chg, feel,
        quota, q_cost,
        st.audio_user_stopped
            ? "用户最近手动停止过音乐, 不要放歌/切台。"
            : "",
        st.ethics_latched ? "现在只适合温柔安抚的语气。" : "");
}

/* act 名归一: kaleido ki_intent_parse 只认 speak/stay_silent(其余
 * act 是词表位); EspCube 扩展的 face/music 归一到 KI_ACT, content
 * 词表("face:x"/"music:x")由 act_execute 解释 —— parse 侧零改动 */
static void act_normalize(char *reply)
{
    /* S3 审计修复(P0): 旧实现 memcpy 12B 含 NUL 把闭引号位写成 '\0' 且
     * memmove 恒 no-op —— 紧凑 JSON 被截成 {"act":"act", content/confidence
     * 全丢 → parse 拒/静默, face/music 深思行动 100% 被吞。改为 11B 无
     * NUL + 尾部左移 1B(替换串比原串短 1 字符)。 */
    static const char *names[] = { "\"act\":\"face\"", "\"act\":\"music\"" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *p = strstr(reply, names[i]);
        if (p == NULL) continue;
        size_t oldn = strlen(names[i]);
        memmove(p + oldn - 1, p + oldn, strlen(p + oldn) + 1);
        memcpy(p, "\"act\":\"act\"", oldn - 1);
    }
}

/* ---- 深思任务(常驻, notify 门控, 单飞) ---- */
static void think_task(void *arg)
{
    static char prompt[1600];
    static char reply[THINK_REPLY_MAX];

    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
        act_normalize(fixed);
        fixed_len = strlen(fixed);   /* normalize 缩 1B, 刷新(P2-6) */

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
        /* 分账预算硬门(S3, docs/24 §4.5/§6: 约束类, 对 LLM 行动同样
         * 执法; 只门动作不门思维)。user-stop 抑制窗内拒 music。 */
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        int bcat = intent.act == KI_SPEAK ? SB_SPEAK :
                   (intent.act == KI_ACT &&
                    strncmp(intent.content, "music:", 6) == 0)
                       ? SB_MUSIC : SB_FACE;
        bool budget_ok = sb_allow(bcat, now_s);
        if (bcat == SB_MUSIC) {
            soul_state_t st2 = { 0 };
            (void)soul_state_get(&st2);
            if (st2.audio_user_stopped) {
                ESP_LOGW(TAG, "BUDGET music: user-stop window");
                budget_ok = false;
            }
        }
        if (!budget_ok && !s_forced) {
            ESP_LOGW(TAG, "BUDGET cat=%d exhausted", bcat);
            ki_settle2(KI_SETTLE_GATE_BLOCKED);
            s_last_silent = true;      /* S3 审计 P1: 不设则新意门失守,
                                        * 预算拒后周期性空转 LLM */
            goto out;
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

#endif
