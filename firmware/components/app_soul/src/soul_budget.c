/*
 * soul_budget.c — 深思行动预算分账实现(S3, docs/24 §4.5/§6; 见 soul_budget.h)
 *
 * 结构: 每类 {今日计数, 上次执行秒} + 音频时窗环(8 槽时间戳) + 账日。
 * 确定性: 零 RNG/零分配; 日翻转在每次 allow/charge/recency 入口检测
 * (now_s/86400 变化 → 清计数+时窗, E3 跨天恢复语义)。
 */
#include "soul_budget.h"

#include <stdio.h>
#include <string.h>

#define SB_AUDIO_RING  8            /* 时窗环容量(≥audio_per_hour 上限) */

static const struct sb_cfg SB_CFG_DEFAULT = {
    .daily_cap = { 4, 6, 6 },       /* speak/music/face(§4.5 分账初值) */
    .audio_per_hour = 3,            /* docs/17 §4.3 全局音频≤3/h      */
    .cat_cooldown_s = 1800,         /* 同类冷却 30min(§4.3)           */
    .recency_tau_s = 1800,          /* E3 新鲜度恢复 τ=30min          */
};

static struct sb_cfg s_cfg;
static uint8_t  s_today[SB_CNT];    /* 各类今日已执行                 */
static uint32_t s_last_fire[SB_CNT];
static bool     s_ever[SB_CNT];     /* 冷却判据(0s 时刻未触发≠已触发) */
static uint32_t s_audio_ring[SB_AUDIO_RING];
static int      s_audio_head;       /* 下写位(=最旧)                  */
static int      s_audio_n;
static uint32_t s_day;              /* 当前账日(now_s/86400)          */
static bool     s_day_seen;

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* 日翻转检测(入口统一; 换日清分账与时窗, 冷却/新鲜度按时间自然衰减) */
static void day_roll(uint32_t now_s)
{
    uint32_t d = now_s / 86400u;
    if (!s_day_seen) {
        s_day = d;
        s_day_seen = true;
        return;
    }
    if (d != s_day) {
        s_day = d;
        memset(s_today, 0, sizeof(s_today));
        memset(s_audio_ring, 0, sizeof(s_audio_ring));
        s_audio_head = 0;
        s_audio_n = 0;
    }
}

void sb_reset(void)
{
    s_cfg = SB_CFG_DEFAULT;
    memset(s_today, 0, sizeof(s_today));
    memset(s_last_fire, 0, sizeof(s_last_fire));
    memset(s_ever, 0, sizeof(s_ever));
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    s_audio_head = 0;
    s_audio_n = 0;
    s_day = 0;
    s_day_seen = false;
}

void sb_configure(const struct sb_cfg *cfg)
{
    if (cfg == NULL) {
        s_cfg = SB_CFG_DEFAULT;
        return;
    }
    s_cfg = *cfg;
}

static bool is_audio(int cat)
{
    return cat == SB_SPEAK || cat == SB_MUSIC;
}

bool sb_allow(int cat, uint32_t now_s)
{
    if (cat < 0 || cat >= SB_CNT)
        return false;
    day_roll(now_s);
    if (s_today[cat] >= s_cfg.daily_cap[cat])
        return false;
    if (is_audio(cat)) {
        /* 拖尾 1h 音频计数(环内 now-3600s 内的戳数) */
        int recent = 0;
        for (int i = 0; i < s_audio_n; i++) {
            uint32_t t = s_audio_ring[i];
            if (now_s >= t && now_s - t < 3600u)
                recent++;
        }
        if (recent >= s_cfg.audio_per_hour)
            return false;
    }
    if (s_ever[cat] &&
        now_s - s_last_fire[cat] < s_cfg.cat_cooldown_s)
        return false;
    return true;
}

void sb_charge(int cat, uint32_t now_s)
{
    if (cat < 0 || cat >= SB_CNT)
        return;
    day_roll(now_s);
    s_today[cat]++;
    s_last_fire[cat] = now_s;
    s_ever[cat] = true;
    if (is_audio(cat)) {
        s_audio_ring[s_audio_head] = now_s;
        s_audio_head = (s_audio_head + 1) % SB_AUDIO_RING;
        if (s_audio_n < SB_AUDIO_RING)
            s_audio_n++;
    }
}

float sb_recency(int cat, uint32_t now_s)
{
    if (cat < 0 || cat >= SB_CNT)
        return 0.0f;
    if (!s_ever[cat])
        return 1.0f;
    if (s_cfg.recency_tau_s == 0)
        return 1.0f;
    return clampf((float)(now_s - s_last_fire[cat]) /
                      (float)s_cfg.recency_tau_s, 0.0f, 1.0f);
}

void sb_snapshot(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0)
        return;
    buf[0] = '\0';
    /* 典型形态: "今日说话 1/4 切歌 0/6 表情 2/6" — 由调用方补时窗/
     * 冷却动态(snapshot 无 now 入参, 保持纯查询; 时窗在 allow 处执法) */
    snprintf(buf, cap, "今日说话 %u/%u 切歌 %u/%u 表情 %u/%u",
             s_today[SB_SPEAK], s_cfg.daily_cap[SB_SPEAK],
             s_today[SB_MUSIC], s_cfg.daily_cap[SB_MUSIC],
             s_today[SB_FACE], s_cfg.daily_cap[SB_FACE]);
}

uint8_t sb_fired_today(int cat)
{
    if (cat < 0 || cat >= SB_CNT)
        return 255;
    return s_today[cat];
}
