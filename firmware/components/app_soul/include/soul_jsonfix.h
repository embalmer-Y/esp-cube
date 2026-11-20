/*
 * soul_jsonfix.h — 深思回复的机械 JSON 恢复(D2 容错面, S6; docs/24 §5.2)
 *
 * LLM 回复常被散文包裹/尾部截断/UTF-8 多字节腰斩; ki_intent_parse 是
 * 严格解析(拒非降级), 本件在其前做**机械恢复**(不猜测语义):
 *   ①首尾定界剥散文(首个 '{' 起, 字符串感知的配对 '}' 止);
 *   ②括号栈补齐截断(缺右括号按栈序补齐——解析器拿完整对象);
 *   ③UTF-8 尾部多字节截断剔除(不完整序列丢弃);
 *   ④失败一律 false → 调用方执行 silent 兜底(docs/24 §5.2 铁律)。
 * 运行时模式探测状态机(json_schema→fc→json_object)为 B-6 挂点——
 * 当前单供应商先固定容错模式。
 * 纯 C 零平台依赖(宿主可测); 确定性; 无分配(out 由调用方供给)。
 */
#ifndef SOUL_JSONFIX_H
#define SOUL_JSONFIX_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 恢复 JSON 到 out(≤cap)。返回 true=out 有效(已 NUL 结尾, *out_len=
 * 长度); false=无可恢复 JSON(调用方 silent)。in==out 允许(原位)。 */
bool soul_json_fix(const char *in, size_t len,
                   char *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_JSONFIX_H */
