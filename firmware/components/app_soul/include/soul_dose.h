/* soul_dose.h —— 深思 appraisal 快通道(docs/24 §11.6 触发层; B-6c)
 *
 * 分层纪律(§11.7 实验回填后的保守形态): 引擎规则表=情绪剂量主通道,
 * LLM 评价=dose_hint——无量纲评价经本模块固定映射变换后, 走引擎
 * 官方注入口(kd_inject_social/ke_ethics_dose/ke_reappraisal)进生理
 * 层, 绝不直写任何内部数值。映射公式=soul_arms 双臂实验已采纳版
 * (docs/24 §11.6 生理通道剂量):
 *   social = +0.15·max(0,gc) − 0.10·max(0,−gc)   (滋养/伤害水库)
 *   ethics =  0.3·imp·max(0,−gc)                 (伤害计伦理负荷)
 *   val    →  快层重评价 ±0.2                    (即时情绪)
 * D3 分位校准映射(引擎目标分布)待设备原始流累积后接入
 * (tools/soul_calib.py fit → 板载映射; 当前=未加载恒等, 与
 * soul_arms --calib-map 缺省口径一致)。
 *
 * 纯函数零平台依赖(宿主可测); 提取侧容错: appraisal 可缺席(不影
 * 响行动管线), 三键全为数值才收, 越界钳位。
 */
#ifndef SOUL_DOSE_H
#define SOUL_DOSE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scherer 口径三元组(soul_arms 同名同域; 简化首版, §55.22 采纳) */
struct soul_appraisal {
    float gc;    /* goal_congruence 需求滋养(+1)~伤害(-1), 0=无关 */
    float imp;   /* importance 事件重要性 [0,1]                   */
    float val;   /* 即时情绪变化 [-0.2,0.2]                       */
};

/* 从深思回复 JSON 提取可选 "appraisal" 对象(整体单对象 JSON, 已过
 * soul_json_fix; cJSON 解析, 调用方任务上下文短驻)。
 * 返回 false=缺席/非对象/任一键缺失或非数值(调用方跳过剂量)。 */
bool soul_appraisal_extract(const char *json, struct soul_appraisal *out);

/* 剂量映射(dose_hint 进引擎前 transform; 见头注)。
 * out 可逐一为 NULL(只算关心的通道)。 */
void soul_dose_compute(const struct soul_appraisal *a, float *social,
                       float *ethics, float *val);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_DOSE_H */
