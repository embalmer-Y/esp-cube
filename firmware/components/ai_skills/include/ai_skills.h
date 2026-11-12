/*
 * ai_skills.h —— AI 技能注册表(L2: AI 工具调用的设备端半边)
 *
 * 设计定位(docs/06 §10): 让云端 LLM 经 OpenAI 兼容 function calling
 * 自主决定调用设备能力(调研结论 2026-11: xiaozhi=MCP 云网关模式,
 * Muse=Markdown 技能模式, 我们=MCU 直连 tools 模式, 见会话记录)。
 *   - ai_skills 只做注册表+JSON 生成+路由, 不认识任何业务组件;
 *   - handler(执行体)由 L5(main) 注册注入 —— 依赖倒置保分层宪法
 *     (ai_session L3 不直接依赖 app_player L3);
 *   - schema 为 OpenAI tools 的 parameters JSON Schema 字符串
 *     (C 字符串常量), 参数校验依赖 LLM 侧约束 + handler 防御解析。
 *
 * 线程契约: ai_skill_exec 在 ai_session 会话任务上下文执行,
 * handler 可阻塞(网络/播放启动), 但应 <5s(LLM 轮回超时预算)。
 */
#pragma once

#include <stddef.h>
#include <esp_err.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_SKILL_NAME_LEN   32
#define AI_SKILL_DESC_LEN   192      /* 字节; 中文描述实测 96B 会截断出
                                      * 非法 UTF-8 → LLM 请求体 400 */
#define AI_SKILL_MAX        12       /* 注册表容量(满即 ESP_ERR_NO_MEM);
                                      * M2 后 8 个在册: 音乐/电台/音量/
                                      * 停止 + 琐事/提醒×4, 留主动交互余量 */

/* 执行体: args=LLM 生成的参数对象(防防御解析, 可能为 NULL);
 * reply 写执行结果文本(回传给 LLM 作 role:"tool" 内容, 也是给
 * 模型的唯一反馈通道, 应包含成败与关键信息); 返回值仅日志用 */
typedef esp_err_t (*ai_skill_fn)(const cJSON *args, char *reply,
                                 size_t reply_cap);

/* 注册(幂等: 同名重复注册覆盖); schema_json 可 NULL(无参技能) */
esp_err_t ai_skill_register(const char *name, const char *description,
                            const char *schema_json, ai_skill_fn handler);

/* 生成 OpenAI 兼容 tools 数组:[{type:"function",function:{name,
 * description, parameters}}](堆分配, 调用方 cJSON_Delete);
 * 无技能注册时返回 NULL(调用方跳过 tools) */
cJSON *ai_skills_tools_json(void);

int ai_skills_count(void);

/* 路由执行: 未注册/无 handler → reply 写错误说明(仍返回给 LLM,
 * 让模型能向用户解释失败), 返回 ESP_ERR_NOT_FOUND */
esp_err_t ai_skill_exec(const char *name, const cJSON *args,
                        char *reply, size_t reply_cap);

#ifdef __cplusplus
}
#endif
