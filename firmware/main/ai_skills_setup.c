/*
 * ai_skills_setup.c —— L5 技能注册(依赖注入接线)
 *
 * ai_skills(L2)只做注册表/路由; 执行体在此接线业务组件 —— 依赖倒置
 * 保分层(ai_session L3 不直接依赖 app_player L3, docs/06 §10)。
 * 首批技能(点歌场景): music.search_play / radio.play /
 * player.set_volume / player.stop。
 */
#include "ai_skills.h"

#include <stdio.h>
#include "esp_err.h"
#include "app_ncm.h"
#include "app_player.h"
#include "audio_mixer.h"

/* ---- 点歌: NCM 游客搜索 → 挑免费曲 → 单曲直播 ---- */
static esp_err_t sk_music_search_play(const cJSON *args, char *reply,
                                      size_t cap)
{
    const cJSON *kw = cJSON_GetObjectItem(args, "keyword");
    if (!cJSON_IsString(kw) || kw->valuestring[0] == 0) {
        snprintf(reply, cap, "错误: 缺少 keyword 参数");
        return ESP_ERR_INVALID_ARG;
    }
    /* 会话任务单线程消费: 静态结果集免压栈(~1.1KB) */
    static ncm_search_result_t res;
    esp_err_t err = app_ncm_search(kw->valuestring, NCM_MAX_RESULTS, &res);
    if (err != ESP_OK) {
        snprintf(reply, cap, "搜索\"%s\"失败(%s), 请稍后再试", kw->valuestring,
                 esp_err_to_name(err));
        return err;
    }
    int pick = -1;
    for (int i = 0; i < res.count; i++) {
        if (res.songs[i].fee == 0 || res.songs[i].fee == 8) { pick = i; break; }
    }
    if (pick < 0) {
        snprintf(reply, cap, "搜到 %d 首, 但全部需要 VIP 或购买, "
                 "游客模式无法播放, 请换首歌曲", res.count);
        return ESP_ERR_NOT_FOUND;
    }
    const ncm_song_t *s = &res.songs[pick];
    char url[256];
    err = app_ncm_get_url(s->id, url, sizeof(url), NULL);
    if (err != ESP_OK) {
        snprintf(reply, cap, "《%s》获取播放地址失败, 请换一首", s->name);
        return err;
    }
    err = app_player_start(url);
    if (err != ESP_OK) {
        snprintf(reply, cap, "《%s》播放启动失败", s->name);
        return err;
    }
    snprintf(reply, cap, "正在播放:《%s》- %s(免费音质)", s->name, s->artist);
    return ESP_OK;
}

/* ---- 电台: 切台/回电台轮播 ---- */
static esp_err_t sk_radio_play(const cJSON *args, char *reply, size_t cap)
{
    const cJSON *st = cJSON_GetObjectItem(args, "station");
    esp_err_t err;
    if (cJSON_IsNumber(st)) {
        int n = app_player_station_num();
        int idx = st->valueint - 1;             /* LLM 用 1 起编号 */
        if (idx < 0) idx = 0;
        if (idx >= n) idx = idx % n;
        err = app_player_play_index(idx);
        snprintf(reply, cap, "已切换到电台:%s",
                 err == ESP_OK ? app_player_station_name(idx) : "失败");
    } else {
        err = app_player_start(NULL);
        snprintf(reply, cap, "已回到电台轮播");
    }
    return err;
}

/* ---- 音量 ---- */
static esp_err_t sk_set_volume(const cJSON *args, char *reply, size_t cap)
{
    const cJSON *lv = cJSON_GetObjectItem(args, "level");
    if (!cJSON_IsNumber(lv)) {
        snprintf(reply, cap, "错误: 缺少 level 参数(0-100)");
        return ESP_ERR_INVALID_ARG;
    }
    int level = lv->valueint;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    audio_mixer_set_master_volume(level);       /* 内部发音量事件(UI 同步) */
    snprintf(reply, cap, "音量已设为 %d%%", level);
    return ESP_OK;
}

/* ---- 停止 ---- */
static esp_err_t sk_player_stop(const cJSON *args, char *reply, size_t cap)
{
    (void)args;
    app_player_stop();
    snprintf(reply, cap, "已停止播放");
    return ESP_OK;
}

void register_ai_skills(void)
{
    ai_skill_register("music.search_play",
                      "在网易云音乐搜索并立即播放歌曲(游客模式仅免费曲, "
                      "VIP/购买曲会失败并说明)",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"keyword\":{\"type\":\"string\","
                      "\"description\":\"歌名/歌手等搜索关键词\"}},"
                      "\"required\":[\"keyword\"]}",
                      sk_music_search_play);
    ai_skill_register("radio.play",
                      "切换到网络电台播放(不传 station 则恢复电台轮播)",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"station\":{\"type\":\"integer\","
                      "\"description\":\"电台编号, 从 1 开始\","
                      "\"minimum\":1,\"maximum\":4}}}",
                      sk_radio_play);
    ai_skill_register("player.set_volume", "设置音箱音量",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"level\":{\"type\":\"integer\","
                      "\"description\":\"音量百分比 0-100\","
                      "\"minimum\":0,\"maximum\":100}},"
                      "\"required\":[\"level\"]}",
                      sk_set_volume);
    ai_skill_register("player.stop", "停止当前所有播放", NULL, sk_player_stop);
}
