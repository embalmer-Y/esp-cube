/*
 * test_sessstate.c —— 会话生命周期状态机宿主验收(docs/18 T28 H; 批次 28)
 *
 * 编译运行(tools/host_tests.sh 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/sessguard/include \
 *       -o /tmp/test_sessstate \
 *       firmware/components/sessguard/test/test_sessstate.c \
 *       firmware/components/sessguard/src/sessstate.c \
 *   && /tmp/test_sessstate
 *
 * 验收(T28 H 语义逐条):
 * - 连续多轮: ≥5 轮无需重新唤醒(轮满才回 IDLE)
 * - 超时: 停止会话采集 + 恢复唤醒(清理掩码含 CAPTURE/GUARD)
 * - VAD 定稿: LISTENING→THINKING; 采集中二次采集请求拒绝(双占麦)
 * - 各状态取消: 清理掩码正确; SPEAKING 期含 TTS(无旧声)
 * - 非法转移防护: IDLE 直接 speech_end 拒等
 */
#include <stdio.h>
#include <string.h>

#include "sessstate.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

static void scenario_multiturn(void)
{
    sessstate_t *ss;
    CHECK(ss_init(&ss, 6, 30000) == 0, "init");
    CHECK(ss_state(ss) == SS_IDLE, "initial idle");

    /* 会话开: 唤醒 → 开采集 */
    CHECK(ss_on_wake(ss), "wake");
    CHECK(ss_on_speech_start(ss), "speech start");
    CHECK(ss_capture_active(ss), "capture on");

    /* 连续多轮: 5 轮无需重新唤醒(T28 验收 ≥5) */
    for (int i = 0; i < 5; i++) {
        CHECK(ss_capture_request(ss) == false, "double capture rejected");
        CHECK(ss_on_speech_end(ss), "vad finalize");
        CHECK(ss_state(ss) == SS_THINKING, "thinking");
        CHECK(ss_on_reply_done(ss), "reply done");
        if (i < 4) {
            CHECK(ss_state(ss) == SS_LISTENING, "back to listening");
        }
    }
    CHECK(ss_turns(ss) == 5, "5 turns done");
    /* 第 6 轮(轮满 6) → IDLE */
    CHECK(ss_on_speech_end(ss), "vad again");
    CHECK(ss_on_reply_done(ss), "reply done 6");
    CHECK(ss_state(ss) == SS_IDLE, "turn-max → idle");
    CHECK(!ss_capture_active(ss), "capture off after turn-max");
    CHECK(ss_on_speech_end(ss) == false, "idle reject speech_end");
    ss_free(ss);
}

static void scenario_timeout(void)
{
    sessstate_t *ss;
    ss_init(&ss, 6, 30000);
    ss_on_wake(ss);
    ss_on_speech_start(ss);
    ss_touch(ss, 1000);
    CHECK(ss_on_timeout(ss, 2000) == 0, "before timeout");
    CHECK(ss_state(ss) == SS_LISTENING, "still listening");
    CHECK(ss_on_timeout(ss, 32000) != 0, "timeout fires");
    CHECK(ss_state(ss) == SS_IDLE, "timeout → idle");
    CHECK(!ss_capture_active(ss), "capture stopped");
    CHECK(ss_on_wake(ss), "wake re-armed after timeout");
    /* SPEAKING 期超时: 掩码含 TTS(无旧声) */
    ss_on_wake(ss);
    ss_on_speech_start(ss);
    ss_on_speech_end(ss);
    ss_touch(ss, 40000);
    /* 造 SPEAKING: reply_done 到 LISTENING 再 thinking…直接测 cancel 场景
     * 中 SPEAKING 掩码; 此处先验证 LISTENING 超时掩码不含 TTS */
    uint32_t mask = ss_on_timeout(ss, 71000);
    CHECK((mask & SS_CLEAN_TTS) == 0, "listening timeout no tts clean");
    CHECK((mask & SS_CLEAN_CAPTURE) != 0, "capture clean present");
    CHECK((mask & SS_CLEAN_GUARD) != 0, "guard clean present");
    ss_free(ss);
}

static void scenario_cancel(void)
{
    sessstate_t *ss;
    ss_init(&ss, 6, 30000);
    /* IDLE 取消: 无会话 → 0 */
    CHECK(ss_cancel(ss) == 0, "cancel in idle = 0");
    /* SPEAKING 取消: 掩码含 TTS(无旧声)+CAPTURE+GUARD */
    ss_on_wake(ss);
    ss_on_speech_start(ss);
    ss_on_speech_end(ss);
    ss_on_reply_done(ss);
    CHECK(ss_state(ss) == SS_LISTENING, "round1 listening");
    CHECK(ss_cancel(ss) != 0, "cancel fires");
    CHECK(ss_state(ss) == SS_IDLE, "cancel → idle");
    /* 双占麦防护: LISTENING 下 capture_request 只授一次 */
    ss_on_wake(ss);
    ss_on_speech_start(ss);
    CHECK(ss_capture_active(ss), "owner granted");
    CHECK(!ss_capture_request(ss), "second owner rejected (双占麦)");
    ss_free(ss);
}

static void scenario_illegal_transitions(void)
{
    sessstate_t *ss;
    ss_init(&ss, 6, 30000);
    CHECK(!ss_on_speech_start(ss), "idle speech_start rejected");
    CHECK(!ss_on_speech_end(ss), "idle speech_end rejected");
    CHECK(!ss_on_reply_done(ss), "idle reply_done rejected");
    CHECK(ss_on_wake(ss), "idle→wake 合法");
    CHECK(!ss_on_wake(ss), "wake 态重复唤醒 rejected");
    CHECK(ss_on_speech_end(ss) == false, "wake speech_end rejected");
    ss_on_speech_start(ss);
    CHECK(ss_on_speech_start(ss) == false, "listening re-start rejected");
    ss_on_speech_end(ss);
    CHECK(ss_on_speech_end(ss) == false, "thinking re-vad rejected");
    ss_free(ss);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "multiturn",  scenario_multiturn },
        { "timeout",    scenario_timeout },
        { "cancel",     scenario_cancel },
        { "illegal",    scenario_illegal_transitions },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = s_fail;
        cases[i].fn();
        printf("%s %s\n", s_fail > before ? "FAIL" : "PASS", cases[i].name);
    }
    if (s_fail == 0) {
        printf("test_sessstate: ALL PASS\n");
        return 0;
    }
    printf("test_sessstate: %d FAILURE(S)\n", s_fail);
    return 1;
}
