/*
 * soul_face.c — L1 底色表情选择实现(Russell 扇区三件套, docs/24 §2.3)
 *
 * 移植依据(二轮调研, docs/24 §2.3 证据行): Russell 1980 环形(圆心=中性)
 * / ROBIN(Paplu 2022)扇区=相邻词角中点、16 人评估 67% 真实感 /
 * EmoACT 30s 同素材节流 / Sims 需求底色(无事件回落) / 宏表情自然时长
 * 0.5-4s(旧 180s 一刀切驻留为极端反例)。
 * 确定性: 零 RNG、零分配; 状态=文件级 static, reset 可全清。
 */
#include "soul_face.h"

#include <math.h>
#include <string.h>

/* ---- 参数(docs/24 §2.3 表; S2 实测修订) ---- */
#define SF_R_NEUTRAL   0.20f   /* 中性死区半径(圆心=保持当前)        */
#define SF_T_HI        8u      /* 高唤醒素材驻留下限(秒)             */
#define SF_T_LO        30u     /* 低唤醒素材驻留下限(秒)             */
/* 切换裕度(Schmitt): 新素材须比当前素材"决定性更近"(角距差>10°)。
 * 实测教训: sigh/sad/cry 在极角上挤成 ~4° 密集簇, 逐点-当前素材角距
 * 比较会让跨簇切换永不发生(月测 100% 单态根因)——改为候选 vs 当前
 * 的相对角距差, 且长驻留(3×T_MIN)强制放行防密集簇冻结。 */
#define SF_A_MARGIN    0.175f  /* ≈10°                              */
#define SF_REPUB_S     30u     /* 同素材重播节流(EmoACT)             */
#define SF_IDLE_FALL_S 45u     /* 无事件回落 mood 底色脸             */
#define SF_MOOD_VW     0.30f   /* 底色偏置: vd = v + 0.30·mood       */
#define SF_MOOD_AW     0.03f   /* 底色偏置: 低 mood 轻压唤醒         */

const soul_face_pt_t soul_face_map[SOUL_FACE_N] = {
    { "smile",        0.6f, 0.50f },
    { "smile_static", 0.5f, 0.30f },
    { "laugh",        0.8f, 0.80f },
    { "snigger",      0.6f, 0.60f },
    { "yummy",        0.7f, 0.60f },
    { "confident",    0.5f, 0.55f },
    { "leisure",      0.3f, 0.30f },
    { "music",        0.5f, 0.60f },
    { "shy",          0.2f, 0.45f },
    { "mock",        -0.1f, 0.55f },
    { "sad",         -0.6f, 0.25f },
    { "cry",         -0.7f, 0.45f },
    { "sigh",        -0.4f, 0.20f },
    { "angry",       -0.7f, 0.80f },
    { "panic",       -0.5f, 0.90f },
    { "shocked",      0.0f, 0.90f },
    { "question",     0.0f, 0.60f },
    { "ponder",       0.0f, 0.45f },
    { "investigate",  0.1f, 0.55f },
    { "yawn",         0.0f, 0.10f },
    { "asleep",       0.1f, 0.05f },
    { "mute",         0.0f, 0.30f },
    { "badminton",    0.6f, 0.70f },
};

/* ---- 状态 ---- */
static int s_cur;              /* 当前素材索引                       */
static uint32_t s_hold;        /* 候选连续驻留秒                     */
static uint32_t s_last_pub[SOUL_FACE_N]; /* 各素材最近发布时刻(节流) */

#define SF_PI 3.14159265358979f

static float clamp1(float x)
{
    return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}

static int idx_of(const char *clip)
{
    for (int i = 0; i < SOUL_FACE_N; i++)
        if (strcmp(soul_face_map[i].clip, clip) == 0)
            return i;
    return -1;
}

/* 循环角差 [0,π] */
static float ang_dist(float a, float b)
{
    float d = fabsf(a - b);
    if (d > SF_PI)
        d = 2.0f * SF_PI - d;
    return d;
}

/* 极角最近的素材(扇区 owning θ = 角距最小者, 与角平分线边界等价) */
static int sector_owner(float th)
{
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < SOUL_FACE_N; i++) {
        float t = atan2f(soul_face_map[i].a, soul_face_map[i].v);
        float d = ang_dist(th, t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* 无事件/死区回落: mood 底色脸(Sims 需求底色; 按 mood 深度分四层) */
static int mood_face(float mood, float a)
{
    int i;
    if (mood >= 0.15f)
        i = idx_of("smile_static");
    else if (mood >= -0.15f)
        i = idx_of("leisure");
    else if (mood >= -0.5f)
        i = idx_of(a >= 0.30f ? "sad" : "sigh");
    else
        i = idx_of(a >= 0.40f ? "cry" : "sad");
    if (i < 0)
        i = idx_of("leisure");
    return i < 0 ? 0 : i;
}

void soul_face_bg_reset(void)
{
    int i = idx_of("leisure");
    s_cur = i < 0 ? 0 : i;
    s_hold = 0;
    memset(s_last_pub, 0, sizeof(s_last_pub));
}

const char *soul_face_bg(float v, float a, float mood,
                         uint32_t now_s, uint32_t idle_s, bool latched)
{
    float vd = clamp1(v + SF_MOOD_VW * mood);
    float ad = clamp1(a + (mood < 0.0f ? -SF_MOOD_AW : 0.0f));
    float r = sqrtf(vd * vd + ad * ad);
    int cand = -1;

    /* 伦理闩锁(安全模式): cry 深切安抚候选(S2: 与冷落场景的区分度
     * 来源之一; 解锁后自动回落扇区/mood 底色) */
    if (latched)
        cand = idx_of("cry");
    /* 死区(含长空闲) → mood 底色脸作为候选(走同一驻留/裕度规则,
     * 修复"事件脸在死区冻结"); 死区外 → 扇区裁决 */
    else if (idle_s >= SF_IDLE_FALL_S || r < SF_R_NEUTRAL)
        cand = mood_face(mood, a);
    else
        cand = sector_owner(atan2f(ad, vd));

    if (cand < 0 || cand == s_cur) {
        s_hold = 0;
        return soul_face_map[s_cur].clip;
    }

    /* 节流: 同素材 30s 内不重播 */
    if (s_last_pub[cand] != 0 &&
        now_s - s_last_pub[cand] < SF_REPUB_S)
        return soul_face_map[s_cur].clip;

    /* 分档驻留 + 决定性更近裕度(Schmitt); 长驻留 3×T_MIN 强制放行
     * (密集簇防冻结, 见参数注) */
    s_hold++;
    uint32_t t_min = soul_face_map[cand].a >= 0.55f ? SF_T_HI : SF_T_LO;
    float th = atan2f(ad, vd);
    float th_c = atan2f(soul_face_map[cand].a, soul_face_map[cand].v);
    float th_cur = atan2f(soul_face_map[s_cur].a, soul_face_map[s_cur].v);
    bool decisively_closer =
        ang_dist(th, th_cur) > ang_dist(th, th_c) + SF_A_MARGIN;
    if (s_hold >= t_min && (decisively_closer || s_hold >= 3u * t_min)) {
        s_cur = cand;
        s_hold = 0;
        s_last_pub[s_cur] = now_s ? now_s : 1;
    }
    return soul_face_map[s_cur].clip;
}
