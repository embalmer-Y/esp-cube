/* llm_profile.h —— LLM 服务商 profile(T30 首段; docs/18 T30)
 *
 * 职责: 服务商配置的解析/校验/选择/回退 —— "至少两种模拟 profile
 * 兼容配置切换、回退与权限测试"(H+S0)。
 *
 * profile 来源: 管理 v1 通道(T17)写入的 settings blob(JSON, 经
 * credstore 加密落盘); 本模块只做纯解析/选择(无 I/O, 宿主可测)。
 *
 * schema:
 *   {"active":"p1", "profiles":[
 *     {"id":"p1", "llm_url":"https://...", "llm_model":"qwen...",
 *      "tts_model":"cosyvoice...", "tts_voice":"...", "tools":true},
 *     ...]}
 * 校验: id 非空≤15; llm_url http(s) 前缀; llm_model 非空 —— 不满足的
 * 条目整条剔除(有效数=剔除后); tts_* 可空(空=沿用内置)。
 * 选择(pick): active 命中→用之; active 缺失/无效→首个有效条目; 全无→
 * -1(调用方走内置 Kconfig 兜底并记日志)。
 * 安全降级: tools=false 的 profile 会话期禁用工具(cfg.tools=NULL,
 * 纯文本模式) —— 缺工具能力不静默误调用。
 */
#ifndef LLM_PROFILE_H
#define LLM_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP_ID_MAX      16
#define LP_URL_MAX     96
#define LP_MODEL_MAX   48
#define LP_VOICE_MAX   32
#define LP_PROFILE_MAX 8

typedef struct {
    char id[LP_ID_MAX];
    char llm_url[LP_URL_MAX];
    char llm_model[LP_MODEL_MAX];
    char tts_model[LP_MODEL_MAX];
    char tts_voice[LP_VOICE_MAX];
    bool tools;                    /* false=纯文本(工具安全降级) */
    int8_t no_think;               /* B-6a: -1=未设(回退 Kconfig 全局默认)
                                    * 0=开思考 1=关思考(sp_llm no_think 字段;
                                    * MiniMax M3.1 关思考=空输出, 板上实测
                                    * §55.30 —— 思考型模型必须显式开) */
} llm_profile_t;

/* 解析 settings blob; 返回有效 profile 数(0=无有效, 负=JSON 根错误);
 * active_id 回传 "active"(无则空串) */
int llm_profiles_parse(const char *json, llm_profile_t out[], int max,
                       char active_id[LP_ID_MAX]);
/* 选择(见头注); 返回下标/-1 */
int llm_profiles_pick(const llm_profile_t *list, int n,
                      const char *active_id);
/* 单条校验(内部逻辑导出供测试): 0=有效, 负=剔除原因 */
int llm_profile_validate(const llm_profile_t *p);

#ifdef __cplusplus
}
#endif
#endif /* LLM_PROFILE_H */
