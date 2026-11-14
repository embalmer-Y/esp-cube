/*
 * mock_app.c —— ui_model 后端模拟(模拟器侧)
 *
 * 职责:
 *   1. 维护 ui_model_t 快照(电台/BT/歌词全状态);
 *   2. 实现 ui_ctl_* 控制接口(行为对齐设备语义:
 *      BT 接管挂起电台、断开恢复;CONNECTING→PLAYING;±切换=电台换台/BT 切曲);
 *   3. ui_audio_take_pcm 合成"类音乐"PCM, 驱动直线频谱动画;
 *   4. BT 歌词时间线(LRC 式: 按曲目内时刻推进 cur/next 两行);
 *   5. --demo 自动化时间线(驱动动作+截图), 供无屏自检。
 */
#include "sim_host.h"
#include "ui_model.h"
#include "ui_pages.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- 电台列表 ---- */
static const char *s_stations[] = {
    "央广卫星经典广播",
    "轻音乐电台",
    "SomaFM Groove Salad",
    "Hit FM 联播网",
};
#define ST_NUM ((int)(sizeof(s_stations) / sizeof(s_stations[0])))

/* ---- BT 曲库(歌词 = LRC 时间线; has_lyrics=false 为纯音乐) ---- */
typedef struct {
    uint32_t t_ms;
    const char *line;
} lyric_line_t;

typedef struct {
    const char *artist, *title, *album;
    bool has_lyrics;
    const lyric_line_t *lyr;
    int lyr_num;
    uint32_t loop_ms;            /* 歌词循环周期(演示用) */
} bt_track_t;

static const lyric_line_t s_lyr_qingtian[] = {
    {    0, "故事的小黄花" },
    { 4200, "从出生那年就飘着" },
    { 8400, "童年的荡秋千" },
    {12600, "随记忆一直晃到现在" },
    {16800, "Re So So Si Do Si La" },
    {21000, "So La Si Si Si Si La Si La So" },
};
static const bt_track_t s_bt_tracks[] = {
    { "周杰伦",   "晴天",         "叶惠美",         true,
      s_lyr_qingtian, 6, 25200 },
    { "久石让",   "Summer",       "菊次郎的夏天",   false, NULL, 0, 0 },
    { "新裤子",   "你要跳舞吗",   "生命因你而火热", false, NULL, 0, 0 },
};
#define BT_NUM ((int)(sizeof(s_bt_tracks) / sizeof(s_bt_tracks[0])))

/* 电台 ICY StreamTitle 轮换 */
static const char *s_icy_titles[] = {
    "早安音乐之旅 · 第 12 期",
    "经典正午 · Imagine",
    "晚间爵士 · Take Five",
    "深夜电台 · 城市夜归人",
};
#define ICY_NUM ((int)(sizeof(s_icy_titles) / sizeof(s_icy_titles[0])))

static ui_model_t s_model;
static uint32_t s_now;
static bool s_demo;
static bool s_radio_was_playing;             /* BT 接管前的电台状态 */
static int  s_bt_track_idx, s_icy_idx;
static uint32_t s_t_icy_next, s_t_bt_next, s_t_conn, s_t_track0;
static float s_phase_bass, s_phase_mel, s_noise, s_synth_t;

static void bt_meta_apply(void)
{
    const bt_track_t *tr = &s_bt_tracks[s_bt_track_idx % BT_NUM];
    snprintf(s_model.title,  sizeof(s_model.title),  "%s", tr->title);
    snprintf(s_model.artist, sizeof(s_model.artist), "%s", tr->artist);
    snprintf(s_model.album,  sizeof(s_model.album),  "%s", tr->album);
    s_model.has_lyrics = tr->has_lyrics;
    s_model.lyric_cur[0] = 0;
    s_model.lyric_next[0] = 0;
    s_t_track0 = s_now;
    s_t_bt_next = s_now + 20000;
}

static void bt_next(int dir)
{
    s_bt_track_idx = (s_bt_track_idx + dir + BT_NUM) % BT_NUM;
    bt_meta_apply();
}

static void set_icy_title(int idx)
{
    snprintf(s_model.title, sizeof(s_model.title), "正在播放: %s",
             s_icy_titles[idx % ICY_NUM]);
}

static void radio_start(void)
{
    s_model.source = UI_SRC_RADIO;
    s_model.player = UI_PLAYER_CONNECTING;
    s_model.buffer_pct = 0;
    s_model.title[0] = 0;
    s_model.has_lyrics = false;
    s_t_conn = s_now;
}

/* ================= 快照 ================= */
const ui_model_t *ui_model(void) { return &s_model; }

/* ================= 控制接口 ================= */
void ui_ctl_toggle_play(void)
{
    if (s_model.source == UI_SRC_BT) return;        /* BT 播放由手机控制 */
    if (s_model.player == UI_PLAYER_PLAYING ||
        s_model.player == UI_PLAYER_CONNECTING) {
        s_model.player = UI_PLAYER_IDLE;
        s_model.source = UI_SRC_IDLE;
        s_model.title[0] = 0;
        s_model.buffer_pct = -1;
    } else {
        radio_start();
    }
}

void ui_ctl_switch_station(int dir)
{
    if (s_model.source == UI_SRC_BT) {
        bt_next(dir);                               /* BT: 切曲目 */
        printf("[mock] BT track -> %s - %s%s\n",
               s_model.artist, s_model.title,
               s_model.has_lyrics ? " (lyrics)" : " (instrumental)");
        return;
    }
    s_model.station_idx = (s_model.station_idx + dir + ST_NUM) % ST_NUM;
    if (s_model.player == UI_PLAYER_PLAYING ||
        s_model.player == UI_PLAYER_CONNECTING) {
        radio_start();
    }
}

void ui_ctl_select_station(int idx)
{
    if (idx < 0 || idx >= ST_NUM) return;
    s_model.station_idx = idx;
    radio_start();
}

void ui_ctl_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == s_model.volume) return;
    s_model.volume = v;
    printf("[mock] volume -> %d\n", v);
}

void ui_ctl_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_model.brightness = pct;
    printf("[mock] brightness -> %d\n", pct);
}

/* ================= 音频合成 ================= */
static int synth_one(float t)
{
    const float beat = 96.0f / 60.0f;               /* 96 BPM */
    float bp = fmodf(t, beat);
    /* 丰富频谱: 谐波列旋律(连续泛音)+和弦+鼓+噪声底, 否则正弦=单根
     * 谱线, 频谱可视化只剩稀疏刺(无法检验真实音乐观感) */
    float kick = expf(-bp * 18.0f) * sinf(2 * (float)M_PI * 55.0f * bp) * 20000;
    float sp = fmodf(t + beat / 2, beat);
    float snare = expf(-sp * 30.0f) * ((float)rand() / RAND_MAX - 0.5f) * 9000;
    float bass_f = 110.0f + 8.0f * sinf(t * 0.7f);
    s_phase_bass += bass_f / 48000.0f;
    float bass = sinf(2 * (float)M_PI * s_phase_bass) *
                 (0.4f + 0.6f * expf(-bp * 6.0f)) * 9000;
    /* 旋律: 三音和弦轮换 + 4 阶谐波(泛音列 → 连续中高频谱) */
    const float chord[3] = { 261.6f, 329.6f, 392.0f };   /* C4 E4 G4 */
    int ni = (int)(t / 2.1f) % 3;                   /* 每 2.1s 换和弦 */
    float f0 = chord[ni] + 4.0f * sinf(t * 0.5f);
    float mel = 0, ph;
    static float ph_h[4];
    for (int h = 1; h <= 4; h++) {
        ph_h[h - 1] += f0 * h / 48000.0f;
        if (ph_h[h - 1] > 1) ph_h[h - 1] -= 1;
        float amp_h = 3200 / (h * h);
        mel += sinf(2 * (float)M_PI * ph_h[h - 1]) * amp_h;
    }
    mel *= 0.5f + 0.5f * sinf(t * 0.11f);
    s_noise = s_noise * 0.88f + ((float)rand() / RAND_MAX - 0.5f) * 0.24f;
    float hat = s_noise * 2600 * (fmodf(t * 2, beat) < 0.03f ? 1.0f : 0.35f);
    float air = s_noise * 700;                      /* 全带宽空气噪声 */
    int16_t l = (int16_t)(kick + bass + mel + hat + snare * 0.8f + air);
    int16_t r = (int16_t)(kick + bass * 0.9f + mel * 1.1f + hat + snare + air);
    if (l > 30000) l = 30000;
    if (l < -30000) l = -30000;
    if (r > 30000) r = 30000;
    if (r < -30000) r = -30000;
    return ((int)r << 16) | (uint16_t)l;
}

int ui_audio_take_pcm(int16_t *stereo, int frames)
{
    bool on = s_model.player == UI_PLAYER_PLAYING ||
              (s_model.source == UI_SRC_BT && s_model.bt_connected);
    if (!on) return 0;
    for (int i = 0; i < frames; i++) {
        int v = synth_one(s_synth_t);
        stereo[i * 2] = (int16_t)(v & 0xFFFF);
        stereo[i * 2 + 1] = (int16_t)((v >> 16) & 0xFFFF);
        s_synth_t += 1.0f / 48000.0f;
    }
    return frames;
}

/* ================= 键盘动作 ================= */
static uint32_t s_ai_t0;                     /* AI 演示脚本起点 */
static bool s_ai_active;

void mock_app_action(sim_action_t a)
{
    switch (a) {
        case SIM_ACT_WIFI_TOGGLE:        /* T18: 断/连(遮罩沿触发由 UI 渲染) */
        s_model.wifi_up = !s_model.wifi_up;
        break;
case SIM_ACT_TOGGLE_PLAY:   ui_ctl_toggle_play(); break;
    case SIM_ACT_NEXT_STATION:  ui_ctl_switch_station(1); break;
    case SIM_ACT_PREV_STATION:  ui_ctl_switch_station(-1); break;
    case SIM_ACT_BT_TOGGLE:
        s_model.bt_connected = !s_model.bt_connected;
        printf("[mock] BT %s\n", s_model.bt_connected ? "connect" : "disconnect");
        if (s_model.bt_connected) {
            s_radio_was_playing =
                s_model.player == UI_PLAYER_PLAYING ||
                s_model.player == UI_PLAYER_CONNECTING;
            s_model.player = UI_PLAYER_IDLE;         /* 电台让路(设备同语义) */
            s_model.source = UI_SRC_BT;
            s_model.has_lyrics = false;              /* bt_meta_apply 重设 */
            s_bt_track_idx = 0;
            bt_meta_apply();
        } else {
            if (s_radio_was_playing) radio_start();
            else { s_model.source = UI_SRC_IDLE; s_model.title[0] = 0; }
        }
        break;
    case SIM_ACT_BT_META:       bt_next(1); break;
    case SIM_ACT_ICY_TITLE:
        s_icy_idx++;
        set_icy_title(s_icy_idx);
        break;
    case SIM_ACT_VOL_UP:        ui_ctl_set_volume(s_model.volume + 5); break;
    case SIM_ACT_VOL_DOWN:      ui_ctl_set_volume(s_model.volume - 5); break;
    case SIM_ACT_AI_ASK:
        s_ai_active = true;                 /* 脚本一轮: 聆听→思考→回答→IDLE */
        s_ai_t0 = s_now;
        printf("[mock] AI session demo\n");
        break;
    case SIM_ACT_SWIPE_DOWN:
        sim_swipe_playback(200);
        break;
    case SIM_ACT_SWIPE_UP:
        sim_swipe_playback(-200);
        break;
    case SIM_ACT_TAP:
        sim_tap_playback();
        break;
    }
}

/* ================= 助理服务演示拍(时间/待办/情绪, M4) ================= */
static void assist_demo_tick(void)
{
    /* 时钟: 每整分跳; 日期固定(演示) */
    s_model.time_synced = true;
    static uint32_t base = 9 * 60 + 41;          /* 09:41 起 */
    uint32_t m = base + s_now / 1000;
    s_model.hour = m / 60;
    s_model.min = m % 60;
    s_model.mon = 11; s_model.day = 12; s_model.wday = 4;
    /* 待办: 固定两条演示 */
    s_model.memo_cnt = 2;
    s_model.remind_cnt = 1;
    snprintf(s_model.next_remind, UI_STR_LEN, "%s", "取快递");
    int32_t left = 25 - (int32_t)(s_now / 1000);
    s_model.next_remind_in_min = left;
    /* 情绪: 每 6s 轮播一档(HOME 光环演示) */
    static const char *tags[] = { "relaxed", "happy", "sleepy", "sad" };
    snprintf(s_model.mood_tag, sizeof(s_model.mood_tag), "%s",
             tags[(s_now / 6000) % 4]);
}

/* ================= AI 会话演示脚本(状态时间线) ================= */
static void ai_demo_tick(void)
{
    if (!s_ai_active) return;
    uint32_t el = s_now - s_ai_t0;
    ui_ai_state_t st = el < 1200  ? UI_AI_LISTENING
                     : el < 2200  ? UI_AI_THINKING
                     : el < 4600  ? UI_AI_SPEAKING
                     : UI_AI_IDLE;
    if (st != s_model.ai_state) {
        s_model.ai_state = st;
        if (st == UI_AI_SPEAKING) {
            snprintf(s_model.ai_text, UI_SUB_LEN, "%s",
                     "今天天气不错，适合听点音乐。");
        }
        if (st == UI_AI_IDLE) s_ai_active = false;
    }
}

/* ================= 歌词时间线 ================= */
static void lyrics_tick(void)
{
    if (!(s_model.source == UI_SRC_BT && s_model.bt_connected &&
          s_model.has_lyrics)) {
        return;
    }
    const bt_track_t *tr = &s_bt_tracks[s_bt_track_idx % BT_NUM];
    uint32_t t = (s_now - s_t_track0) % tr->loop_ms;

    int cur = -1;
    for (int i = 0; i < tr->lyr_num; i++) {
        if (t >= tr->lyr[i].t_ms) cur = i;
        else break;
    }
    if (cur >= 0) {
        snprintf(s_model.lyric_cur, UI_SUB_LEN, "%s", tr->lyr[cur].line);
        int nx = cur + 1;
        snprintf(s_model.lyric_next, UI_SUB_LEN, "%s",
                 nx < tr->lyr_num ? tr->lyr[nx].line
                                  : tr->lyr[0].line);    /* 循环预示首行 */
    }
}

/* ================= demo 时间线 ================= */
enum { DEMO_NONE = -1, DEMO_END = -3 };
typedef struct {
    uint32_t at;
    int act;                                 /* sim_action_t / DEMO_* */
    const char *shot;
} demo_step_t;
/* 连拍时间线: 覆盖各动画中间过程(review 用)
 *   歌词切句发生在 14401ms(track0=10201 + 第二句 4200ms),
 *   fly_in 480ms / fly_out 420ms → 14450..14930 密集连拍 */
static const demo_step_t s_demo_steps[] = {
    { 1200,  DEMO_NONE,              "01_splash" },
    { 3600,  SIM_ACT_TOGGLE_PLAY,    NULL },         /* 开电台 */
    { 5000,  DEMO_NONE,              "02_face_music" },
    { 9200,  SIM_ACT_NEXT_STATION,   NULL },         /* 切台(表情常驻) */
    { 9600,  DEMO_NONE,              "03_face_station" },
    { 10200, SIM_ACT_VOL_DOWN,       NULL },
    { 10600, DEMO_NONE,              "04_face_vol" },
    /* 下滑状态栏(350ms 滑动 + 落定) */
    { 12000, SIM_ACT_SWIPE_DOWN,     NULL },
    { 12800, DEMO_NONE,              "20_status_panel" },
    /* T18: 断网遮罩演示(断→截→复连→截; 面板开状态下可见) */
    { 13100, SIM_ACT_WIFI_TOGGLE,    NULL },
    { 13600, DEMO_NONE,              "23_offline" },
    { 13900, SIM_ACT_WIFI_TOGGLE,    NULL },
    /* 单击收起 + 上滑 AI 状态(动作间隔>600ms: 手势冷却闸 350ms,
     * 贴太近的连续动作会被当丢帧重入吃掉 —— 与真机节奏一致) */
    { 14000, SIM_ACT_TAP,            NULL },
    { 14800, SIM_ACT_SWIPE_UP,       NULL },
    { 15600, DEMO_NONE,              "21_soul_panel" },
    { 16900, DEMO_NONE,              "22_soul_panel_late" },
    /* AI 会话演示: 聆听/思考/回答(表情帧速变化) */
    { 20300, SIM_ACT_AI_ASK,         NULL },
    { 20600, DEMO_NONE,              "13_ai_listening" },
    { 21900, DEMO_NONE,              "13_ai_thinking" },
    { 23000, DEMO_NONE,              "13_ai_speaking" },
    { 24900, DEMO_NONE,              "13_ai_idle" },
    { 25200, DEMO_END,               NULL },
};
static int s_demo_idx;

static void demo_tick(void)
{
    while (s_demo_idx < (int)(sizeof(s_demo_steps) / sizeof(s_demo_steps[0])) &&
           s_now >= s_demo_steps[s_demo_idx].at) {
        const demo_step_t *d = &s_demo_steps[s_demo_idx++];
        if (d->shot != NULL) {
            printf("[demo] shot %s\n", d->shot);
            sim_host_screenshot(d->shot);
        }
        if (d->act == DEMO_END) {
            printf("[demo] timeline finished\n");
        } else if (d->act != DEMO_NONE) {
            mock_app_action((sim_action_t)d->act);
        }
    }
}

int mock_app_demo_done(void)
{
    return s_demo_idx >= (int)(sizeof(s_demo_steps) / sizeof(s_demo_steps[0]));
}

/* ================= 初始化与周期 ================= */
void mock_app_init(bool demo)
{
    memset(&s_model, 0, sizeof(s_model));
    s_model.volume = 60;
    s_model.brightness = 80;
    s_model.buffer_pct = -1;
    s_model.station_num = ST_NUM;
    for (int i = 0; i < ST_NUM; i++) s_model.station_names[i] = s_stations[i];
    snprintf(s_model.wifi_ssid, sizeof(s_model.wifi_ssid), "MyWiFi");
    snprintf(s_model.wifi_ip, sizeof(s_model.wifi_ip), "192.168.2.42");
    s_model.wifi_up = true;
    s_model.sd_ok = true;      /* T18: sim 默认 SD 在位 */
    snprintf(s_model.fw_info, sizeof(s_model.fw_info), "v0.3.0-sim");
    s_demo = demo;
    srand(20261111);
}

void mock_app_periodic(uint32_t now_ms_val)
{
    s_now = now_ms_val;

    /* CONNECTING → PLAYING(缓冲爬升 ~1.6s) */
    if (s_model.player == UI_PLAYER_CONNECTING) {
        int p = (int)((s_now - s_t_conn) * 100 / 1600);
        s_model.buffer_pct = p > 100 ? 100 : p;
        if (p >= 100) {
            s_model.player = UI_PLAYER_PLAYING;
            s_model.buffer_pct = -1;
            s_icy_idx = 0;
            set_icy_title(s_icy_idx);
            s_t_icy_next = s_now + 9000;
        }
    }

    /* 电台标题轮换(9s) */
    if (s_model.player == UI_PLAYER_PLAYING &&
        s_model.source == UI_SRC_RADIO && s_now >= s_t_icy_next) {
        s_icy_idx++;
        set_icy_title(s_icy_idx);
        s_t_icy_next = s_now + 9000;
    }

    lyrics_tick();
    ai_demo_tick();
    assist_demo_tick();

    if (s_demo) demo_tick();
}
