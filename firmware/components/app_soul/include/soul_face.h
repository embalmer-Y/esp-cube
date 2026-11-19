/*
 * soul_face.h — L1 底色表情选择: Russell 扇区三件套(docs/24 §2.3, S2)
 *
 * 纯 C 零平台依赖: soul.c(固件)与 soul_sim(宿主)同源编译, 单一素材表
 * 单一算法(消灭旧 soul.c/soul_sim 双份漂移)。
 * 算法: 极角扇区(相邻素材角平分线为界, ROBIN/Paplu 口径) + 中性死区
 * (r<R_N 换脸冻结, Russell 圆心=中性) + 分档驻留(高唤醒 8s/低唤醒 30s,
 * 替换旧 180s 一刀切) + 角度裕度(防边界抖动) + 同素材 30s 节流 +
 * 无事件 45s 回 mood 底色脸(Sims 需求底色)。
 * 强度分级(r>0.7 播强烈版)为挂点: 现素材无强/弱双版本, 暂不启用
 * (docs/24 §2.3 注), 素材扩充后接入。
 */
#ifndef SOUL_FACE_H
#define SOUL_FACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOUL_FACE_N 23

typedef struct {
    const char *clip;   /* 素材名(设备 face/ 目录实有段) */
    float v, a;         /* valence/arousal 平面坐标 [-1,1] */
} soul_face_pt_t;

/* 唯一素材表(与 app_ui/ui/assets/face 的 23 段 .efc 一一对应) */
extern const soul_face_pt_t soul_face_map[SOUL_FACE_N];

/*
 * 每拍调用(1Hz)。v/a=ke 发布显示效价/唤醒(内部再叠 mood 底色偏移
 * docs/24 §2.3: vd=v+0.30·mood, ad=a−0.03·(mood<0)); mood=ke 慢层;
 * now_s=单调秒(驻留/节流时基); idle_s=距上次用户活动的秒数;
 * latched=伦理闩锁(安全模式 → cry 深切安抚候选, 与虐待场景可区分)。
 * 返回当前底色 clip(驻留机芯内部维护); 未变化时返回同一指针。
 */
const char *soul_face_bg(float v, float a, float mood,
                         uint32_t now_s, uint32_t idle_s, bool latched);

/* 回开机底色(leisure)——soul_reset/测试用 */
void soul_face_bg_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SOUL_FACE_H */
