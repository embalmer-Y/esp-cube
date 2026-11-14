/* act_gate.h —— cube_act 装配入口(L5 内部; docs/18 T13) */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cube_act.h"
#include "persona.h"

void act_gate_init(void);
persona_svc_t *act_gate_persona(void);
void act_gate_persona_clear(void);   /* 清除全部角色资料(不可逆; 台调) */

/* 共享提交 helper(main 内 CLI/触摸/技能各入口用; T13 续) */
cact_res_t act_gate_submit(const char *cap, const uint8_t *params,
                           size_t params_len, cact_origin_t origin,
                           uint32_t *out_id);
void act_gate_pump(void);
bool act_gate_started(uint32_t id);          /* 最近回执 == started */
void act_gate_user_stop_audio(const char *reason);  /* 停止锁存组1+取消在途 */
void act_gate_resume_audio(void);            /* 用户显式播放解除锁存 */
