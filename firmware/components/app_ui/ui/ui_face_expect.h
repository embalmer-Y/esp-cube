/*
 * ui_face_expect.h —— 内置表情片段的第 0 帧期望值(设备自检对照, 任务 31.2)
 *
 * 本表即内置片段集合: 与 firmware/components/app_ui/CMakeLists.txt 的 FACE_EMBED、
 * MANIFEST.json 中 embedded=true 的 4 段一致。数值照 MANIFEST 抄入整数:
 *   key_permille_x100 = frame0_key_permille × 100    alpha 为 0 的占比, 单位 0.01‰
 *   lit_pct_x100      = frame0_lit_share_pct × 100   lit 占比, 单位 0.01%
 * tools/test_face_expect.py(host_tests.sh [18])逐项核对本表、FACE_EMBED 与
 * MANIFEST.json; 素材重新生成后不一致即失败, 须同步修改本表。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 自检容差(任务 31.2 B2): alpha0 为 0.5 个百分点, lit 为 5 个百分点 */
#define UI_FACE_ALPHA0_TOL_X100  500u   /* 单位 0.01‰: 500 = 5‰ = 0.5 个百分点 */
#define UI_FACE_LIT_TOL_X100     500u   /* 单位 0.01%: 500 = 5 个百分点 */

typedef struct {
    const char *name;            /* 片段名, 与 EMBED 文件名主干一致 */
    uint32_t key_permille_x100;  /* frame0_key_permille × 100 */
    uint32_t lit_pct_x100;       /* frame0_lit_share_pct × 100 */
} ui_face_expect_t;

/* 按片段名查找; 名称不在表中(或为 NULL)返回 NULL。表中的名称即内置片段集合。 */
static inline const ui_face_expect_t *ui_face_expect_find(const char *name)
{
    static const ui_face_expect_t tbl[] = {
        { "smile_static", 83408u, 2069u },
        { "laugh",        83413u, 2094u },
        { "investigate",  83413u, 2069u },
        { "shocked",      83413u, 2069u },
    };
    if (name == NULL) return NULL;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcmp(name, tbl[i].name) == 0) return &tbl[i];
    }
    return NULL;
}
