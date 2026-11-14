/* sessstate.c —— 会话生命周期状态机实现(见 sessstate.h; T28) */
#include <stdlib.h>

#include "sessstate.h"

struct sessstate {
    ss_state_t st;
    uint32_t turn_max;
    uint32_t turns;                /* 已完成轮数 */
    uint64_t last_active;          /* 会话静默起算点 */
    uint64_t idle_timeout;
    bool capture;                  /* 采集权在位(双占麦防护) */
};

int ss_init(sessstate_t **out, uint32_t turn_max, uint64_t idle_timeout_ms)
{
    if (out == NULL || turn_max == 0 || idle_timeout_ms == 0) return -1;
    sessstate_t *ss = calloc(1, sizeof(*ss));
    if (ss == NULL) return -1;
    ss->st = SS_IDLE;
    ss->turn_max = turn_max;
    ss->idle_timeout = idle_timeout_ms;
    *out = ss;
    return 0;
}

void ss_free(sessstate_t *ss) { free(ss); }

ss_state_t ss_state(const sessstate_t *ss) { return ss ? ss->st : SS_IDLE; }
uint32_t ss_turns(const sessstate_t *ss) { return ss ? ss->turns : 0; }

bool ss_on_wake(sessstate_t *ss)
{
    if (ss == NULL) return false;
    if (ss->st == SS_IDLE) {
        ss->st = SS_WAKE;
        return true;
    }
    /* 会话中重复唤醒(回声) → 不重入(既有 EVT_WAKE 忽略语义) */
    return false;
}

bool ss_on_speech_start(sessstate_t *ss)
{
    if (ss == NULL || ss->st != SS_WAKE) return false;
    ss->st = SS_LISTENING;
    ss->turns = 0;
    ss->capture = true;
    return true;
}

bool ss_on_speech_end(sessstate_t *ss)
{
    if (ss == NULL || ss->st != SS_LISTENING || !ss->capture) return false;
    ss->st = SS_THINKING;
    return true;
}

bool ss_on_reply_done(sessstate_t *ss)
{
    if (ss == NULL) return false;
    if (ss->st != SS_THINKING && ss->st != SS_SPEAKING) return false;
    ss->turns++;
    if (ss->turns >= ss->turn_max) {
        ss->st = SS_IDLE;          /* 轮满: 停采集, 恢复唤醒 */
        ss->capture = false;
        return true;
    }
    ss->st = SS_LISTENING;         /* 连续多轮: 无需重新唤醒 */
    ss->capture = true;
    return true;
}

uint32_t ss_on_timeout(sessstate_t *ss, uint64_t now_ms)
{
    if (ss == NULL || ss->st == SS_IDLE) return 0;
    if (now_ms - ss->last_active < ss->idle_timeout) return 0;
    uint32_t mask = SS_CLEAN_CAPTURE;
    if (ss->st == SS_SPEAKING) mask |= SS_CLEAN_TTS;
    mask |= SS_CLEAN_GUARD;        /* 旧上下文/请求一并失效 */
    ss->st = SS_IDLE;
    ss->capture = false;
    return mask;
}

void ss_touch(sessstate_t *ss, uint64_t now_ms)
{
    if (ss != NULL) ss->last_active = now_ms;
}

bool ss_capture_request(sessstate_t *ss)
{
    if (ss == NULL || ss->st != SS_LISTENING) return false;
    if (ss->capture) return false;                 /* 双占麦防护 */
    ss->capture = true;
    return true;
}

bool ss_capture_active(const sessstate_t *ss)
{
    return ss != NULL && ss->capture;
}

uint32_t ss_cancel(sessstate_t *ss)
{
    if (ss == NULL || ss->st == SS_IDLE) return 0;
    uint32_t mask = SS_CLEAN_CAPTURE;
    if (ss->st == SS_SPEAKING) mask |= SS_CLEAN_TTS;
    mask |= SS_CLEAN_GUARD;
    ss->st = SS_IDLE;
    ss->capture = false;
    return mask;
}
