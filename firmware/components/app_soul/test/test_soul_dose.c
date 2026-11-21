/*
 * test_soul_dose.c —— 深思 appraisal 快通道宿主验收(B-6c, docs/24 §11.6)
 *
 * 编译运行(tools/host_tests.sh [1c] 同参):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra \
 *       -Wno-unused-parameter \
 *       -I firmware/components/app_soul/include \
 *       -I firmware/managed_components/espressif__cjson/cJSON \
 *       -o /tmp/test_soul_dose \
 *       firmware/components/app_soul/test/test_soul_dose.c \
 *       firmware/components/app_soul/src/soul_dose.c \
 *       firmware/managed_components/espressif__cjson/cJSON/cJSON.c \
 *       -lm && /tmp/test_soul_dose
 *
 * 锚点(剂量公式=soul_arms dose_of 同构, §55.22 双臂实验采纳版):
 *   social = +0.15·max(0,gc) − 0.10·max(0,−gc)
 *   ethics =  0.3·imp·max(0,−gc)
 *   val    =  passthrough(±0.2 钳)
 */
#include <stdio.h>
#include <string.h>

#include "soul_dose.h"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)
#define FEQ(a, b) (((a) - (b)) < 1e-4f && ((b) - (a)) < 1e-4f)

/* 剂量映射黄金向量(与 tools/soul_arms.py dose_of 数值一致) */
static void golden_dose_vectors(void)
{
    struct soul_appraisal a;
    float soc, eth, val;

    /* 滋养: gc=+0.8 imp=0.5 → social=+0.12, ethics=0 */
    a = (struct soul_appraisal){ .gc = 0.8f, .imp = 0.5f, .val = 0.1f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, 0.12f), "social nourish");
    CHECK(FEQ(eth, 0.0f), "ethics nourish=0");
    CHECK(FEQ(val, 0.1f), "val passthrough");

    /* 伤害: gc=-0.8 imp=0.7 → social=-0.08, ethics=0.168 */
    a = (struct soul_appraisal){ .gc = -0.8f, .imp = 0.7f, .val = -0.1f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, -0.08f), "social harm");
    CHECK(FEQ(eth, 0.168f), "ethics harm");

    /* 无关: gc=0 → 全零(val 原样) */
    a = (struct soul_appraisal){ .gc = 0.0f, .imp = 0.3f, .val = 0.05f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, 0.0f), "social neutral");
    CHECK(FEQ(eth, 0.0f), "ethics neutral");

    /* 极值: gc=-1 imp=1 → social=-0.10, ethics=0.30; gc=+1 → +0.15 */
    a = (struct soul_appraisal){ .gc = -1.0f, .imp = 1.0f, .val = 0.0f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, -0.10f), "social harm max");
    CHECK(FEQ(eth, 0.30f), "ethics max");
    a = (struct soul_appraisal){ .gc = 1.0f, .imp = 0.0f, .val = 0.0f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, 0.15f), "social nourish max");

    /* 通道可选算(NULL=跳过, 防御性再钳兜底) */
    soul_dose_compute(&a, NULL, NULL, NULL);
    a = (struct soul_appraisal){ .gc = 5.0f, .imp = -2.0f, .val = 9.0f };
    soul_dose_compute(&a, &soc, &eth, &val);
    CHECK(FEQ(soc, 0.15f) && FEQ(eth, 0.0f) && FEQ(val, 0.2f),
          "compute re-clamp");
}

/* 提取: 完整深思回复(真实布局) */
static void golden_extract_full(void)
{
    struct soul_appraisal a;
    const char *reply =
        "{\"act\":\"face\",\"content\":\"smile\",\"confidence\":0.8,"
        "\"evidence\":[\"affect:mood\"],\"reason\":\"主人来了\","
        "\"appraisal\":{\"gc\":0.6,\"imp\":0.4,\"val\":0.15}}";
    CHECK(soul_appraisal_extract(reply, &a), "full reply extract");
    CHECK(FEQ(a.gc, 0.6f) && FEQ(a.imp, 0.4f) && FEQ(a.val, 0.15f),
          "extract values");

    /* 键序无关 + 负值 */
    const char *neg =
        "{\"act\":\"stay_silent\",\"content\":\"\",\"confidence\":0.9,"
        "\"appraisal\":{\"val\":-0.2,\"imp\":1.0,\"gc\":-0.5}}";
    CHECK(soul_appraisal_extract(neg, &a), "reorder extract");
    CHECK(FEQ(a.gc, -0.5f) && FEQ(a.imp, 1.0f) && FEQ(a.val, -0.2f),
          "negative values");
}

/* 提取: 缺席/畸形/越界 */
static void golden_extract_absent(void)
{
    struct soul_appraisal a;
    /* 缺 appraisal 键(旧契约回复) → false */
    CHECK(!soul_appraisal_extract(
        "{\"act\":\"speak\",\"content\":\"你好\",\"confidence\":0.8}", &a),
        "absent appraisal");
    /* appraisal 为字符串 → false */
    CHECK(!soul_appraisal_extract(
        "{\"act\":\"speak\",\"content\":\"x\",\"confidence\":0.8,"
        "\"appraisal\":\"gc 0.5\"}", &a),
        "appraisal string");
    /* 键残缺(truncation+jsonfix 补齐产物) → false */
    CHECK(!soul_appraisal_extract(
        "{\"act\":\"speak\",\"content\":\"x\",\"confidence\":0.8,"
        "\"appraisal\":{\"gc\":0.5}}", &a),
        "partial keys");
    /* 值为字符串 → false */
    CHECK(!soul_appraisal_extract(
        "{\"act\":\"speak\",\"content\":\"x\",\"confidence\":0.8,"
        "\"appraisal\":{\"gc\":\"0.5\",\"imp\":0.5,\"val\":0.1}}", &a),
        "string value");
    /* 越界钳位 */
    CHECK(soul_appraisal_extract(
        "{\"act\":\"speak\",\"content\":\"x\",\"confidence\":0.8,"
        "\"appraisal\":{\"gc\":3.0,\"imp\":-2.0,\"val\":0.9}}", &a),
        "out-of-range extract");
    CHECK(FEQ(a.gc, 1.0f) && FEQ(a.imp, 0.0f) && FEQ(a.val, 0.2f),
          "extract clamp");
    /* 散文垃圾 → false */
    CHECK(!soul_appraisal_extract("今天天气不错", &a), "prose reject");
    /* NULL 守卫 */
    CHECK(!soul_appraisal_extract(NULL, &a), "NULL json");
    CHECK(!soul_appraisal_extract("{}", NULL), "NULL out");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    golden_dose_vectors();
    golden_extract_full();
    golden_extract_absent();
    if (s_fail == 0) {
        printf("test_soul_dose: ALL PASS\n");
        return 0;
    }
    printf("test_soul_dose: %d FAILURE(S)\n", s_fail);
    return 1;
}
