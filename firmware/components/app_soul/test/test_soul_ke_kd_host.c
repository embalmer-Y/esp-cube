/*
 * test_soul_ke_kd_host.c —— ke/kd 黄金测试宿主 runner(docs/24 S1)
 *
 * 用法: ./test_soul_ke_kd_host emotion|drive
 * 退出码 = unity_fail(失败断言数; 0=全绿)。锚=合同纪律 docs/23 §6.0/R4:
 * 语义变更必须同批改锚。当前 emotion 存在 E1-E4 旧锚失守(9 失, 已知,
 * docs/24 S2 批随 mood 三层栈重构一并重锚), 由 host_tests.sh 按已知数
 * 放行并打印 KNOWN-FAIL; drive 全绿要求。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"   /* host_stubs 垫片 */

int unity_fail;
int unity_abort;
jmp_buf unity_jmp;

void setUp(void) {}
void tearDown(void) {}

void emotion_tests_main(void);
void drive_tests_main(void);
void trait_tests_main(void);
void budget_tests_main(void);
void motiv_tests_main(void);
void jsonfix_tests_main(void);
void moodgate_tests_main(void);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s emotion|drive|trait|budget|motiv|jsonfix|moodgate\n", argv[0]);
        return 99;
    }
    UNITY_BEGIN();
    if (strcmp(argv[1], "emotion") == 0)
        emotion_tests_main();
    else if (strcmp(argv[1], "drive") == 0)
        drive_tests_main();
    else if (strcmp(argv[1], "trait") == 0)
        trait_tests_main();
    else if (strcmp(argv[1], "budget") == 0)
        budget_tests_main();
    else if (strcmp(argv[1], "motiv") == 0)
        motiv_tests_main();
    else if (strcmp(argv[1], "jsonfix") == 0)
        jsonfix_tests_main();
    else if (strcmp(argv[1], "moodgate") == 0)
        moodgate_tests_main();
    else {
        fprintf(stderr, "unknown suite: %s\n", argv[1]);
        return 99;
    }
    if (unity_fail == 0) {
        printf("test_soul_%s: ALL PASS\n", argv[1]);
        return 0;
    }
    printf("test_soul_%s: %d FAIL\n", argv[1], unity_fail);
    return unity_fail;
}
