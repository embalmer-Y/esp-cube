/*
 * unity.h —— Unity 宿主最小垫片(docs/24 S1: ke/kd 黄金测试入 CI)
 *
 * 仅实现黄金测试用到的宏子集: RUN_TEST/UNITY_BEGIN/UNITY_END +
 * TEST_ASSERT_{TRUE,FALSE,EQUAL,EQUAL_INT,EQUAL_STRING}。
 * 语义对齐真 Unity: 断言失败打点并 longjmp 中止当前测试(setUp/tearDown
 * 环绕), 非失败继续。runner(test_soul_ke_kd_host.c) 读 unity_fail 计数。
 * 放在 host_stubs/ 仅供宿主编译路径 -I 命中; 板端走 IDF 真 Unity, 不受影响。
 */
#ifndef UNITY_HOST_SHIM_H
#define UNITY_HOST_SHIM_H

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

extern int  unity_fail;          /* 累计失败断言数 */
extern int  unity_abort;         /* 当前测试已中止 */
extern jmp_buf unity_jmp;

void setUp(void);
void tearDown(void);

#define UNITY_BEGIN() do { unity_fail = 0; } while (0)
#define UNITY_END()   do { } while (0)

#define UNITY_FAIL_AT(line, msg) do { \
    printf("FAIL %s:%d %s\n", __FILE__, (line), (msg)); \
    unity_fail++; \
    unity_abort = 1; \
    longjmp(unity_jmp, 1); \
} while (0)

#define RUN_TEST(t) do { \
    unity_abort = 0; \
    if (setjmp(unity_jmp) == 0) { \
        setUp(); \
        t(); \
        tearDown(); \
    } \
} while (0)

#define TEST_ASSERT_TRUE(c)  do { if (!(c))  UNITY_FAIL_AT(__LINE__, #c); } while (0)
#define TEST_ASSERT_FALSE(c) do { if ((c))   UNITY_FAIL_AT(__LINE__, "!" #c); } while (0)
#define TEST_ASSERT_EQUAL(a, b) do { \
    if ((long long)(a) != (long long)(b)) UNITY_FAIL_AT(__LINE__, #a "==" #b); \
} while (0)
#define TEST_ASSERT_EQUAL_INT(a, b) TEST_ASSERT_EQUAL(a, b)
#define TEST_ASSERT_EQUAL_STRING(a, b) do { \
    const char *_x = (a), *_y = (b); \
    if (_x == NULL || _y == NULL || strcmp(_x, _y) != 0) \
        UNITY_FAIL_AT(__LINE__, #a "==" #b); \
} while (0)

#endif /* UNITY_HOST_SHIM_H */
