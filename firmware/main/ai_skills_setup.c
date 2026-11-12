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
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cubecore.h"
#include "app_ncm.h"
#include "app_player.h"
#include "app_assist.h"
#include "audio_mixer.h"

static const char *TAG = "ai_skills";

/* ================= speculative 搜歌预取(2026-11-12) =================
 * LLM 流式吐 tool_calls 参数期间(delta 未完), 关键词已可辨识 ——
 * 抢跑 NCM 搜索+取址(独立任务, 与 LLM 流并行), 技能正式执行时直接
 * 命中缓存。点歌场景首响预算 -1.5~2s(LiveKit/Pipecat 并行工具模式)。
 * 触发链: sp_llm args_cb → CUBE_EVT_AI_TOOL_HINT(总线) → 此处。 */
static char s_pf_kw[128];                      /* 抢跑关键词 */
static char s_pf_url[256];                     /* 预取的播放地址 */
static volatile int8_t s_pf_state;             /* 0闲 1跑 2成 3败 */
static ncm_search_result_t s_pf_res;           /* 预取结果集(静态, 任务写) */

static void sk_prefetch_task(void *arg)
{
    (void)arg;
    esp_err_t err = app_ncm_search(s_pf_kw, NCM_MAX_RESULTS, &s_pf_res);
    int pick = -1;
    if (err == ESP_OK) {
        for (int i = 0; i < s_pf_res.count; i++) {
            if (s_pf_res.songs[i].fee == 0 ||
                s_pf_res.songs[i].fee == 8) { pick = i; break; }
        }
    }
    if (pick >= 0 &&
        app_ncm_get_url(s_pf_res.songs[pick].id, s_pf_url,
                        sizeof(s_pf_url), NULL) == ESP_OK) {
        s_pf_state = 2;
        ESP_LOGI(TAG, "prefetch ready: 《%s》- %s",
                 s_pf_res.songs[pick].name, s_pf_res.songs[pick].artist);
    } else {
        s_pf_state = 3;
        ESP_LOGW(TAG, "prefetch failed (err=%d pick=%d)", err, pick);
    }
    vTaskDelete(NULL);
}

/* 从流式累计 args 里贪心提取 "keyword":"..."(引号闭合才算成) */
static bool hint_extract_kw(const char *args, char *out, size_t cap)
{
    const char *k = strstr(args, "\"keyword\"");
    if (k == NULL) return false;
    k = strchr(k + 9, ':');
    if (k == NULL) return false;
    k = strchr(k + 1, '"');
    if (k == NULL) return false;
    k++;
    const char *e = strchr(k, '"');             /* 未闭合 = 关键词还在吐 */
    if (e == NULL) return false;
    size_t n = (size_t)(e - k);
    if (n == 0 || n >= cap) return false;
    memcpy(out, k, n);
    out[n] = '\0';
    return true;
}

/* 总线回调(同步派发, SSE 读循环上下文): 只做判定+起任务, 不阻塞 */
static void sk_hint_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                           void *ctx)
{
    (void)len; (void)ctx;
    if (id != CUBE_EVT_AI_TOOL_HINT || data == NULL) return;
    const cube_ai_tool_hint_t *h = data;
    if (strcmp(h->name, "music.search_play") != 0) return;
    if (s_pf_state == 1 || s_pf_state == 2) return;    /* 已抢跑/已有货 */
    char kw[128];
    if (!hint_extract_kw(h->args, kw, sizeof(kw))) return;
    strlcpy(s_pf_kw, kw, sizeof(s_pf_kw));
    s_pf_state = 1;
    ESP_LOGI(TAG, "prefetch start: %s", kw);
    xTaskCreate(sk_prefetch_task, "skpf", 8192, NULL, 5, NULL);
}

/* 技能执行时等待/命中预取(≤3s); 返回 true=命中(url 已填) */
static bool sk_prefetch_take(const char *kw, char *url, size_t cap)
{
    if (strcmp(kw, s_pf_kw) != 0) return false;
    for (int i = 0; i < 60 && s_pf_state == 1; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_pf_state != 2) return false;
    strlcpy(url, s_pf_url, cap);
    return true;
}

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
    /* speculative 命中: 抢跑结果直接用(省搜索+取址 ~2s) */
    char url[256];
    if (sk_prefetch_take(kw->valuestring, url, sizeof(url))) {
        ESP_LOGI(TAG, "prefetch HIT");
        const ncm_song_t *s = NULL;
        for (int i = 0; i < s_pf_res.count; i++) {
            if (s_pf_res.songs[i].fee == 0 || s_pf_res.songs[i].fee == 8) {
                s = &s_pf_res.songs[i];
                break;
            }
        }
        if (app_player_start(url) == ESP_OK && s != NULL) {
            snprintf(reply, cap, "正在播放:《%s》- %s(免费音质)",
                     s->name, s->artist);
            return ESP_OK;
        }
        /* 落到常规路径重试 */
    }
    const ncm_song_t *s = &res.songs[pick];
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

/* ================= M2 助理技能: 琐事/提醒 =================
 * 存储与调度在 app_assist(NVS + 1s tick); 到点播报走 L5 接线的
 * announcer → ai_session_announce。回执要口语化(fast_ack 直播)。 */

static esp_err_t sk_memo_add(const cJSON *args, char *reply, size_t cap)
{
    const cJSON *t = cJSON_GetObjectItem(args, "text");
    if (!cJSON_IsString(t) || t->valuestring[0] == 0) {
        snprintf(reply, cap, "错误: 缺少 text 参数");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t id = 0;
    esp_err_t err = app_assist_add(t->valuestring, 0, &id);
    if (err == ESP_OK) {
        snprintf(reply, cap, "已记下:%s", t->valuestring);
    } else {
        snprintf(reply, cap, "记录失败(%s), 请稍后再试",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t sk_reminder_add(const cJSON *args, char *reply, size_t cap)
{
    const cJSON *t = cJSON_GetObjectItem(args, "text");
    if (!cJSON_IsString(t) || t->valuestring[0] == 0) {
        snprintf(reply, cap, "错误: 缺少 text 参数");
        return ESP_ERR_INVALID_ARG;
    }
    /* LLM 责任: 口语时间 → 绝对时间(time)或相对分钟(delay_minutes) */
    int64_t due = 0;
    const cJSON *dm = cJSON_GetObjectItem(args, "delay_minutes");
    const cJSON *tm = cJSON_GetObjectItem(args, "time");
    char when[48] = "";
    if (cJSON_IsNumber(dm) && dm->valueint > 0) {
        due = app_assist_now() + (int64_t)dm->valueint * 60;
        snprintf(when, sizeof(when), "%d分钟后", dm->valueint);
    } else if (cJSON_IsString(tm) && app_assist_parse_localtime(tm->valuestring, &due)) {
        app_assist_fmt_speak(due, when, sizeof(when));
    } else {
        snprintf(reply, cap,
                 "错误: 时间参数无效。time 必须是\"YYYY-MM-DD HH:MM\"格式"
                 "(如 2026-11-13 10:00), 或改用 delay_minutes 整数分钟; "
                 "两个参数只给一个");
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_assist_time_synced()) {
        snprintf(reply, cap, "错误: 设备时间还没校准, 请过一会儿再设");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t id = 0;
    esp_err_t err = app_assist_add(t->valuestring, due, &id);
    if (err == ESP_OK) {
        snprintf(reply, cap, "好的, 已定好提醒:%s, %s我会叫你",
                 t->valuestring, when);
    } else {
        snprintf(reply, cap, "提醒设置失败(%s)", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t sk_assist_list(const cJSON *args, char *reply, size_t cap)
{
    (void)args;
    static assist_item_t items[ASSIST_MAX_ITEMS];  /* 会话任务单线程消费 */
    int n = app_assist_list(items, ASSIST_MAX_ITEMS);
    if (n == 0) {
        snprintf(reply, cap, "目前没有记录任何琐事和提醒");
        return ESP_OK;
    }
    /* 口播友好: 每类最多报 3 条, 超出报总数(逐条念 24 条会念到天荒地老,
     * 回执同时给 LLM: 多了它会主动问用户要哪条) */
    size_t off = 0;
    int rem_shown = 0, memo_shown = 0, rem_total = 0, memo_total = 0;
    for (int i = 0; i < n; i++) {
        (items[i].due > 0) ? rem_total++ : memo_total++;
    }
    if (rem_total > 0) {
        off += snprintf(reply + off, cap - off, "提醒%d条: ", rem_total);
        for (int i = 0; i < n && rem_shown < 3 && off < cap - 96; i++) {
            if (items[i].due <= 0) continue;
            char when[32];
            app_assist_fmt_compact(items[i].due, when, sizeof(when));
            off += snprintf(reply + off, cap - off, "%s#%u %s %s",
                            rem_shown++ ? "; " : "", (unsigned)items[i].id,
                            when, items[i].text);
        }
        if (rem_total > rem_shown) {
            off += snprintf(reply + off, cap - off, "; 等共%d条", rem_total);
        }
    }
    if (memo_total > 0) {
        off += snprintf(reply + off, cap - off, "%s琐事%d条: ",
                        rem_total ? "。" : "", memo_total);
        for (int i = 0; i < n && memo_shown < 3 && off < cap - 96; i++) {
            if (items[i].due != 0) continue;
            off += snprintf(reply + off, cap - off, "%s#%u %s",
                            memo_shown++ ? "; " : "",
                            (unsigned)items[i].id, items[i].text);
        }
        if (memo_total > memo_shown) {
            off += snprintf(reply + off, cap - off, "; 等共%d条", memo_total);
        }
    }
    snprintf(reply + off, cap - off, "。");
    return ESP_OK;
}

static esp_err_t sk_assist_remove(const cJSON *args, char *reply, size_t cap)
{
    const cJSON *id = cJSON_GetObjectItem(args, "id");
    if (!cJSON_IsNumber(id)) {
        snprintf(reply, cap, "错误: 缺少 id 参数");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = app_assist_remove((uint32_t)id->valueint);
    if (err == ESP_OK) {
        snprintf(reply, cap, "已删除#%d", id->valueint);
    } else {
        snprintf(reply, cap, "没找到编号 %d 的记录", id->valueint);
    }
    return err;
}

void register_ai_skills(void)
{
    cube_event_subscribe(sk_hint_evt_cb, NULL);
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
    ai_skill_register("memo.add",
                      "帮用户记一条琐事/备忘(用户说\"记一下/帮我记住/"
                      "别忘了\"这类没有具体时间点的交代时用)",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"text\":{\"type\":\"string\","
                      "\"description\":\"要记住的内容, 保留用户原意\"}},"
                      "\"required\":[\"text\"]}",
                      sk_memo_add);
    ai_skill_register("reminder.add",
                      "设置定时提醒, 到点设备会主动语音提醒用户"
                      "(用户说\"XX分钟后/明天早上八点/几点提醒我\"时用)",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"text\":{\"type\":\"string\","
                      "\"description\":\"提醒事项内容\"},"
                      "\"time\":{\"type\":\"string\","
                      "\"description\":\"提醒的绝对时间, 必须把用户的口语"
                      "时间(明天早上八点/下周三下午三点半)换算成本地"
                      "时间, 格式 YYYY-MM-DD HH:MM(24小时制)\"},"
                      "\"delay_minutes\":{\"type\":\"integer\","
                      "\"description\":\"距现在的分钟数(用户说几分钟后时"
                      "优先用这个)\"}},"
                      "\"required\":[\"text\"]}",
                      sk_reminder_add);
    ai_skill_register("assist.list",
                      "列出已记录的所有琐事和提醒(带编号, 用户问\"我记了"
                      "什么/有什么提醒\"时用; 编号用于删除/取消)",
                      NULL, sk_assist_list);
    ai_skill_register("assist.remove",
                      "按编号删除一条琐事或取消一条提醒",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"id\":{\"type\":\"integer\","
                      "\"description\":\"assist.list 返回的编号\"}},"
                      "\"required\":[\"id\"]}",
                      sk_assist_remove);
}
