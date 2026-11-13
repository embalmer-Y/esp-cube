/*
 * soul_bb.c —— L1 因子黑板实现(docs/14 §4)
 *
 * 纯 C 无平台依赖(宿主单测可编); 唯一平台件 = 覆写槽的临界区。
 * 宿主测试路径下用平凡桩(单线程测试无竞争, 见 SOUL_BB_HOST_TEST)。
 */
#include "soul_bb.h"

#include <stdio.h>
#include <string.h>

#if defined(SOUL_BB_HOST_TEST)
/* 宿主单测桩: 无 FreeRTOS */
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
static inline void portENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *const s_names[SOUL_BB_CNT] = {
    "valence", "arousal", "urgency", "bond", "ethics",
    "m_social", "m_stim", "m_expr", "m_curios", "m_comp",
    "idle", "quiet", "ai_active", "playing", "presence",
    "net", "salience", "mem_hook",
    "batt", "temp", "charging", "touch",
};

/* 因子真值(单写者=soul tick; 对齐 float 写原子) */
static float s_val[SOUL_BB_CNT];

/* 调试覆写槽(CLI 任务写, tick/soul 读 —— 短临界区) */
static struct {
    float    val;
    uint32_t until;         /* 生效截止(单调秒); 0=空槽 */
} s_ovr[SOUL_BB_CNT];
static portMUX_TYPE s_ovr_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_now;              /* 最近一拍的单调秒 */
static bool     s_inited;

/* 出厂默认: 电量满/温度零/其余 0(valence 0=平静) */
static void defaults(void)
{
    memset(s_val, 0, sizeof(s_val));
    memset(s_ovr, 0, sizeof(s_ovr));
    s_val[SOUL_BB_BATT] = 1.0f;
}

static void ensure_init(void)
{
    if (!s_inited) {
        defaults();
        s_inited = true;
    }
}

const char *soul_bb_name(soul_bb_key_t key)
{
    if ((unsigned)key >= SOUL_BB_CNT) return "?";
    return s_names[key];
}

float soul_bb_get(soul_bb_key_t key)
{
    if ((unsigned)key >= SOUL_BB_CNT) return 0;
    ensure_init();
    /* 覆写槽优先(生效期台调值即真值) */
    portENTER_CRITICAL(&s_ovr_lock);
    float o = (s_ovr[key].until != 0 && s_ovr[key].until > s_now)
              ? s_ovr[key].val : s_val[key];
    portEXIT_CRITICAL(&s_ovr_lock);
    return o;
}

void soul_bb_set(soul_bb_key_t key, float v)
{
    if ((unsigned)key >= SOUL_BB_CNT) return;
    ensure_init();
    s_val[key] = v;
}

void soul_bb_salience(float v)
{
    ensure_init();
    if (v > s_val[SOUL_BB_SALIENCE]) {
        s_val[SOUL_BB_SALIENCE] = v;
    }
}

void soul_bb_tick(uint32_t now_s)
{
    ensure_init();
    /* 显著性/触摸衰减: τ≈60s 半衰期 → 每拍 ×0.988 */
    const float decay = 0.988f;
    s_val[SOUL_BB_SALIENCE] *= decay;
    if (s_val[SOUL_BB_SALIENCE] < 0.01f) s_val[SOUL_BB_SALIENCE] = 0;
    s_val[SOUL_BB_TOUCH] *= decay;
    if (s_val[SOUL_BB_TOUCH] < 0.01f) s_val[SOUL_BB_TOUCH] = 0;
    s_now = now_s;
    /* 覆写槽到期回收 */
    portENTER_CRITICAL(&s_ovr_lock);
    for (int i = 0; i < SOUL_BB_CNT; i++) {
        if (s_ovr[i].until != 0 && s_ovr[i].until <= s_now) {
            s_ovr[i].until = 0;
        }
    }
    portEXIT_CRITICAL(&s_ovr_lock);
}

void soul_bb_override(soul_bb_key_t key, float val, uint32_t ttl_s)
{
    if ((unsigned)key >= SOUL_BB_CNT) return;
    ensure_init();
    portENTER_CRITICAL(&s_ovr_lock);
    if (ttl_s == 0) {
        s_ovr[key].until = 0;
    } else {
        s_ovr[key].val = val;
        s_ovr[key].until = s_now + ttl_s;
    }
    portEXIT_CRITICAL(&s_ovr_lock);
}

size_t soul_bb_dump(char *buf, size_t cap)
{
    ensure_init();
    size_t off = 0;
    for (int i = 0; i < SOUL_BB_CNT && off + 1 < cap; i++) {
        int n = snprintf(buf + off, cap - off, "%s%s=%.2f",
                         i ? " " : "", s_names[i], soul_bb_get(i));
        if (n < 0) break;
        off += (size_t)n;
    }
    if (off >= cap) off = cap - 1;
    buf[off] = '\0';
    return off;
}

void soul_bb_reset(void)
{
    ensure_init();
    portENTER_CRITICAL(&s_ovr_lock);
    defaults();
    portEXIT_CRITICAL(&s_ovr_lock);
    s_now = 0;
}
