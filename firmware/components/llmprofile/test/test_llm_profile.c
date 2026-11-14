/*
 * test_llm_profile.c —— LLM 服务商 profile 宿主验收(docs/18 T30 H+S0)
 *
 * 编译运行(tools/host_tests.sh 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/llmprofile/include \
 *       -I firmware/managed_components/espressif__cjson/cJSON \
 *       -o /tmp/test_llm_profile \
 *       firmware/components/llmprofile/test/test_llm_profile.c \
 *       firmware/components/llmprofile/src/llm_profile.c \
 *       firmware/managed_components/espressif__cjson/cJSON/cJSON.c \
 *       -lm && /tmp/test_llm_profile
 *
 * 验收(T30: 至少两种模拟 profile 切换/回退/权限):
 * - 双 profile 解析 + active 切换
 * - active 缺失/无效 → 回退首个有效
 * - 无效条目剔除(url 非法/id 缺失/模型缺失)
 * - tools 缺省=true, 显式 false → 降级标志
 * - 坏 JSON/空 profiles → 0/-1(调用方走内置兜底)
 */
#include <stdio.h>
#include <string.h>

#include "llm_profile.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

static void scenario_two_profiles(void)
{
    /* T30 验收主体: 两种模拟 profile + 切换 */
    const char *json =
        "{\"active\":\"mock2\",\"profiles\":["
        "{\"id\":\"dashscope\",\"llm_url\":\"https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions\","
        "\"llm_model\":\"qwen3.8-flash\",\"tts_model\":\"cosyvoice-v3-flash\",\"tts_voice\":\"longxiaochun\",\"tools\":true},"
        "{\"id\":\"mock2\",\"llm_url\":\"https://mock.local/v1/chat/completions\","
        "\"llm_model\":\"mock-7b\",\"tools\":false}]}";
    llm_profile_t ps[LP_PROFILE_MAX];
    char active[LP_ID_MAX];
    int n = llm_profiles_parse(json, ps, LP_PROFILE_MAX, active);
    CHECK(n == 2, "two profiles parsed");
    CHECK(strcmp(active, "mock2") == 0, "active id");
    CHECK(strcmp(ps[0].id, "dashscope") == 0 && ps[0].tools,
          "profile0 fields");
    CHECK(strcmp(ps[1].id, "mock2") == 0 && !ps[1].tools,
          "profile1 tools=false");
    CHECK(ps[1].tts_model[0] == '\0', "optional tts omitted");

    /* 切换: active=mock2 → 命中 #1 */
    CHECK(llm_profiles_pick(ps, n, active) == 1, "pick active #1");
    /* 切回: active=dashscope → #0 */
    CHECK(llm_profiles_pick(ps, n, "dashscope") == 0, "pick switch #0");
    /* active 无效 → 回退首个有效 */
    CHECK(llm_profiles_pick(ps, n, "ghost") == 0, "invalid active fallback");
    CHECK(llm_profiles_pick(ps, n, "") == 0, "empty active fallback");
}

static void scenario_invalid_items(void)
{
    /* 无效条目剔除: url 非法 / id 缺失 / 模型缺失 */
    const char *json =
        "{\"active\":\"bad\",\"profiles\":["
        "{\"id\":\"bad1\",\"llm_url\":\"ftp://x\",\"llm_model\":\"m\"},"
        "{\"id\":\"\",\"llm_url\":\"https://x\",\"llm_model\":\"m\"},"
        "{\"id\":\"good1\",\"llm_url\":\"https://x\",\"llm_model\":\"m\"},"
        "{\"id\":\"bad2\",\"llm_url\":\"https://x\"}]}";
    llm_profile_t ps[LP_PROFILE_MAX];
    char active[LP_ID_MAX];
    int n = llm_profiles_parse(json, ps, LP_PROFILE_MAX, active);
    CHECK(n == 1, "only valid survives");
    CHECK(strcmp(ps[0].id, "good1") == 0, "survivor is good1");
    /* active 指向被剔除条目 → 回退 */
    CHECK(llm_profiles_pick(ps, n, active) == 0, "pruned-active fallback");
    CHECK(llm_profile_validate(NULL) < 0, "validate NULL");
}

static void scenario_degenerate(void)
{
    /* 坏 JSON / 空 profiles / 超容截断 */
    llm_profile_t ps[LP_PROFILE_MAX];
    char active[LP_ID_MAX];
    CHECK(llm_profiles_parse("not json", ps, LP_PROFILE_MAX, active) == -1,
          "bad json");
    CHECK(llm_profiles_parse("{\"profiles\":[]}", ps, LP_PROFILE_MAX,
                             active) == 0, "empty profiles");
    CHECK(llm_profiles_parse("{}", ps, LP_PROFILE_MAX, active) == 0,
          "no profiles key");
    CHECK(llm_profiles_parse(NULL, ps, LP_PROFILE_MAX, active) == -1,
          "NULL json");
    /* 超容: 10 条只收 8(数组上限) */
    char big[2048];
    strcpy(big, "{\"profiles\":[");
    for (int i = 0; i < 10; i++) {
        char one[160];
        snprintf(one, sizeof(one),
                 "%s{\"id\":\"p%02d\",\"llm_url\":\"https://x\",\"llm_model\":\"m\"}",
                 i > 0 ? "," : "", i);
        strcat(big, one);
    }
    strcat(big, "]}");
    int n = llm_profiles_parse(big, ps, LP_PROFILE_MAX, active);
    CHECK(n == LP_PROFILE_MAX, "cap at 8");
    CHECK(llm_profiles_pick(ps, n, "p07") == 7, "cap pick ok");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "two_profiles", scenario_two_profiles },
        { "invalid_items", scenario_invalid_items },
        { "degenerate",   scenario_degenerate },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = s_fail;
        cases[i].fn();
        printf("%s %s\n", s_fail > before ? "FAIL" : "PASS", cases[i].name);
    }
    if (s_fail == 0) {
        printf("test_llm_profile: ALL PASS\n");
        return 0;
    }
    printf("test_llm_profile: %d FAILURE(S)\n", s_fail);
    return 1;
}
