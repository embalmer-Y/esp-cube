/*
 * test_soul_jsonfix.c — 深思回复机械 JSON 恢复黄金回归(S6 D2, docs/24 §5.2)
 */
#include <string.h>

#include "soul_jsonfix.h"
#include "unity.h"

static bool fix(const char *in, char *out, size_t cap)
{
	return soul_json_fix(in, strlen(in), out, cap, NULL);
}

/* 干净 JSON 原样通过 */
static void golden_clean(void)
{
	const char *src =
		"{\"act\":\"speak\",\"content\":\"你好\",\"confidence\":0.8}";
	char out[128];
	TEST_ASSERT_TRUE(fix(src, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING(src, out);
}

/* 散文包裹剥离 */
static void golden_prose_wrapped(void)
{
	char out[128];
	TEST_ASSERT_TRUE(fix(
		"好的, 我的决定如下: {\"act\":\"stay_silent\","
		"\"content\":\"\",\"confidence\":0.9} 以上。",
		out, sizeof(out)));
	TEST_ASSERT_TRUE(strstr(out, "{\"act\":\"stay_silent\"") == out);
	TEST_ASSERT_TRUE(out[strlen(out) - 1] == '}');
}

/* 尾部截断补齐(缺右括号) */
static void golden_truncated_pad(void)
{
	char out[128];
	TEST_ASSERT_TRUE(fix(
		"{\"act\":\"face\",\"content\":\"face:laugh\",\"conf",
		out, sizeof(out)));
	TEST_ASSERT_TRUE(strstr(out, "\"act\":\"face\"") != NULL);
	size_t n = strlen(out);
	TEST_ASSERT_TRUE(out[n - 1] == '}');
	/* 括号平衡粗检 */
	int depth = 0;
	for (size_t i = 0; i < n; i++) {
		if (out[i] == '{') depth++;
		if (out[i] == '}') depth--;
	}
	TEST_ASSERT_TRUE(depth == 0);
}

/* UTF-8 多字节腰斩剔除 */
static void golden_utf8_cut(void)
{
	/* "沉"=E6 B2 89: 截去最后 1 字节 → 恢复时剔除不完整序列 */
	char src[64];
	int n = snprintf(src, sizeof(src),
			 "{\"act\":\"speak\",\"content\":\"沉");
	TEST_ASSERT_TRUE(n > 0);
	size_t cut = strlen(src) - 1;
	src[cut] = '\0';
	/* 手动补齐成截断态: 去掉闭括号再截一字节 */
	char broken[64];
	snprintf(broken, sizeof(broken), "{\"act\":\"speak\",\"content\":\"沉");
	size_t bl = strlen(broken);
	broken[bl - 1] = '\0';             /* 掐掉"89"续行字节 */
	char out[64];
	TEST_ASSERT_TRUE(fix(broken, out, sizeof(out)));
	TEST_ASSERT_TRUE(strstr(out, "speak") != NULL);
	/* 全输出无孤儿首字节(P1-1: 裸 E6/F0 不得存活) */
	for (const char *q = out; *q; q++) {
		if (((unsigned char)*q & 0xE0) == 0xC0) {   /* 2 字节首 */
			TEST_ASSERT_TRUE(((unsigned char)q[1] & 0xC0) == 0x80);
		} else if (((unsigned char)*q & 0xF0) == 0xE0) {  /* 3 字节首 */
			TEST_ASSERT_TRUE(((unsigned char)q[1] & 0xC0) == 0x80);
			TEST_ASSERT_TRUE(((unsigned char)q[2] & 0xC0) == 0x80);
		}
	}
}

/* 字符串内的括号/引号不欺骗栈 */
static void golden_braces_in_string(void)
{
	char out[128];
	TEST_ASSERT_TRUE(fix(
		"结论: {\"act\":\"speak\",\"content\":\"公式 a{b} 与 \\\\\"x\\\\\"\""
		",\"confidence\":0.7}",
		out, sizeof(out)));
	TEST_ASSERT_TRUE(strstr(out, "a{b}") != NULL);
}

/* 纯散文/乱码 → false(调用方 silent 兜底) */
static void golden_garbage_rejected(void)
{
	char out[64];
	TEST_ASSERT_FALSE(fix("今天天气不错, 我想唱歌。", out, sizeof(out)));
	TEST_ASSERT_FALSE(fix("} { 反的 ]", out, sizeof(out)));
	TEST_ASSERT_FALSE(fix("", out, sizeof(out)));
}

void jsonfix_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(golden_clean);
	RUN_TEST(golden_prose_wrapped);
	RUN_TEST(golden_truncated_pad);
	RUN_TEST(golden_utf8_cut);
	RUN_TEST(golden_braces_in_string);
	RUN_TEST(golden_garbage_rejected);
	UNITY_END();
}
