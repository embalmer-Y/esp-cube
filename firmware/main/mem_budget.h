/*
 * mem_budget.h —— 内存预算审计入口(docs/22 内存预算表的板上执法点)
 *
 * 预算口径单点在 docs/22: 固定开销(栈/静态)链接期占位, 动态域设
 * 水位下限。本模块只在启动完成与 CLI `mem` 时对账并打日志,
 * FAIL 一律告警不 abort —— 内存硬断言=开机循环(§52 修复 B 教训)。
 */
#pragma once

#include <stdint.h>

/* 打一行分域水位 + 对照预算下限的逐条 PASS/FAIL(仅告警) */
void mem_budget_audit(const char *when);

/* 审计 + 全堆区域明细(heap_caps_print_heap_info 口径), CLI `mem` 用 */
void mem_budget_dump(void);
