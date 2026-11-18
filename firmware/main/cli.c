/*
 * cli.c —— 串口命令行(L5 调试/演示入口)
 *
 * 背景: P4C5 无触摸/无按键, 电台切台与 NCM 点歌暂无物理输入 ——
 * 串口命令作为最小输入通道(后续由语音/UI 手势接管)。
 *
 * 输入通道实现: 只对 UART0 安装 RX 驱动(uart_read_bytes 阻塞读),
 * 控制台 TX 保持 esp_stdio 默认直写路径 —— 不调 uart_vfs_dev_use_driver
 * (实测该路径在 IDF6 控制台上会 vfprintf 嵌套递归爆栈, 2026-11-11)。
 *
 * 命令:
 *   ncm <关键词>    网易云游客搜索并播放首个可播曲目(打印候选列表)
 *   ncmn <序号>     播放上一次搜索结果的第 n 条(1 起)
 *   radio           回电台播放列表轮播
 *   st <n>          电台列表切台(1 起)
 *   stop            停止播放
 *   help            帮助
 */
#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/usb_serial_jtag.h"   /* 第二输入通道(docs/19 §55.7) */
#endif
#include "cube_tasks.h"
#include "app_ncm.h"
#include "app_player.h"
#include "audio_mixer.h"
#include "ui_model.h"
#include "cube_act.h"
#include "act_gate.h"
#include "soul.h"

/* soul_mem record/boot 声明已收进 soul.h(批次 44) */
#include "app_assist.h"
#include "app_storage.h"
#include "pair.h"
#include "pair_server.h"
#include "mem_budget.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/stats.h"
#include "ai_skills.h"
#include "sdkconfig.h"
#include "face_perf.h"
#if CONFIG_ESP_CUBE_ENABLE_PROV
#include "app_prov.h"
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
#include "ai_session.h"
#endif
#if CONFIG_ESP_CUBE_ENABLE_SOUL
#include "soul_bb.h"
#include "soul_behavior.h"
#endif

static const char *TAG = "cli";

#define CLI_UART        UART_NUM_0       /* 控制台串口(CH343P) */
#define CLI_RX_BUF      2048
#define CLI_LINE_MAX    128

static ncm_search_result_t s_last;
static bool s_has_last;

static const char *fee_str(int fee)
{
    switch (fee) {
    case 0: return "无版权";
    case 1: return "VIP";
    case 4: return "购买";
    case 8: return "免费";
    default: return "?";
    }
}

static void print_results(const ncm_search_result_t *r)
{
    printf("  #   歌名                         歌手              时长   版权\n");
    for (int i = 0; i < r->count; i++) {
        const ncm_song_t *s = &r->songs[i];
        printf("  %-3d %-28s %-16s %d:%02d   %s\n", i + 1, s->name, s->artist,
               s->duration_ms / 60000, (s->duration_ms / 1000) % 60,
               fee_str(s->fee));
    }
    fflush(stdout);
}

/* 播放搜索结果第 idx 条(0 起): 取 URL → app_player 单曲直播 */
static void play_index(int idx)
{
    if (!s_has_last || idx < 0 || idx >= s_last.count) {
        printf("  序号越界(先 ncm <关键词> 搜索)\n");
        return;
    }
    const ncm_song_t *s = &s_last.songs[idx];
    char url[256];
    int br = 0;
    printf("  取址: %s - %s ... ", s->artist, s->name);
    fflush(stdout);
    esp_err_t err = app_ncm_get_url(s->id, url, sizeof(url), &br);
    if (err != ESP_OK) {
        printf("失败(%s): 游客仅免费曲, VIP 曲需二期登录\n",
               esp_err_to_name(err));
        return;
    }
    printf("%dkbps\n  播放: %s\n", br / 1000, url);
    err = app_player_start(url);
    printf("  player: %s\n", err == ESP_OK ? "started" : esp_err_to_name(err));
    fflush(stdout);
}

/* radio.play 经统一门(DEBUG 来源; station>=0 切台, -1 轮播; T13 续) */
static void cmd_play_via_gate(int station)
{
    uint8_t param[16];
    uint32_t id = 0;
    cact_res_t r;
    if (station >= 0) {
        int n = app_player_station_num();
        if (n > 0) station %= n;
        memcpy(param, "station", 8);
        param[8] = (uint8_t)('0' + station);
        param[9] = 0;
        r = act_gate_submit("radio.play", param, 10,
                            CACT_ORIGIN_PHYSICAL_USER, &id);
    } else {
        r = act_gate_submit("radio.play", NULL, 0,
                            CACT_ORIGIN_PHYSICAL_USER, &id);
    }
    act_gate_pump();
    if (r == CACT_RES_OK && act_gate_started(id)) {
        act_gate_resume_audio();     /* 用户显式播放 → 解除停止锁存 */
        printf("  player: radio started (#%lu via gate)\n", (unsigned long)id);
    } else {
        printf("  player: rejected (%d)\n", r);
    }
}

static void cmd_ncm(char *keywords)
{
    if (keywords == NULL || keywords[0] == 0) {
        printf("  用法: ncm <关键词>\n");
        return;
    }
    printf("  搜索 \"%s\" ...\n", keywords);
    /* 直接搜进静态 s_last(避免大结构体局部变量压栈) */
    esp_err_t err = app_ncm_search(keywords, NCM_MAX_RESULTS, &s_last);
    if (err != ESP_OK) {
        printf("  搜索失败: %s\n", esp_err_to_name(err));
        return;
    }
    s_has_last = true;
    print_results(&s_last);
    /* 自动播放首个"游客可播"(fee 0/8); 全是 VIP/购买则试第一条并说明 */
    int pick = -1;
    for (int i = 0; i < s_last.count; i++) {
        if (s_last.songs[i].fee == 0 || s_last.songs[i].fee == 8) { pick = i; break; }
    }
    if (pick < 0 && s_last.count > 0) {
        pick = 0;
        printf("  (候选均为 VIP/购买曲, 游客态大概率不可播)\n");
    }
    if (pick >= 0) {
        play_index(pick);
    }
}

/* M2 助理: 琐事/提醒查看与测试注入(零 API 成本的本地通道) */
static void cmd_assist_list(void)
{
    static assist_item_t items[ASSIST_MAX_ITEMS];
    int n = app_assist_list(items, ASSIST_MAX_ITEMS);
    char ts[40];
    time_t t = (time_t)app_assist_now();
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    printf("  时间: %s(%s)\n", ts,
           app_assist_time_synced() ? "已同步" : "未同步");
    if (n == 0) {
        printf("  (空, note <文本> 记一条 / remind <秒> <文本> 定提醒)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char when[32];
        if (items[i].due > 0) {
            app_assist_fmt_compact(items[i].due, when, sizeof(when));
            printf("  #%-3u [提醒 %s] %s\n",
                   (unsigned)items[i].id, when, items[i].text);
        } else {
            printf("  #%-3u [琐事] %s\n",
                   (unsigned)items[i].id, items[i].text);
        }
    }
}

/* wget: 局域网 http 拉文件到 SD(素材/配置传输通道, ≤5MB PSRAM 缓冲) */
static void cmd_wget(const char *arg)
{
    if (arg == NULL || arg[0] == 0) {
        printf("  用法: wget <url> [rel](如 wget http://192.168.2.6:8000/cat.efc face/cat)\n");
        return;
    }
    char url[192], rel[80] = "";
    int n = sscanf(arg, "%191s %79s", url, rel);
    if (n < 1) { printf("  url 解析失败\n"); return; }
    char *slash = strrchr(url, '/');
    if (n < 2 && slash != NULL) {
        snprintf(rel, sizeof(rel), "face/%s", slash + 1);
    }
    esp_http_client_config_t hc = { .url = url, .timeout_ms = 15000 };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (c == NULL) { printf("  http init fail\n"); return; }
    esp_err_t e = esp_http_client_open(c, 0);
    int64_t total = (e == ESP_OK) ? esp_http_client_fetch_headers(c) : -1;
    if (e != ESP_OK || total <= 0 || total > 5 * 1024 * 1024) {
        printf("  fetch fail/过大: %s(%lldB)\n", esp_err_to_name(e), (long long)total);
        esp_http_client_cleanup(c);
        return;
    }
    char *buf = heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM);
    if (buf == NULL) { esp_http_client_cleanup(c); printf("  oom\n"); return; }
    int64_t got = 0;
    while (got < total) {
        int r = esp_http_client_read(c, buf + got, (size_t)(total - got));
        if (r <= 0) break;
        got += r;
    }
    esp_http_client_cleanup(c);
    e = (got == total) ? app_storage_write(rel, buf, (size_t)got) : ESP_FAIL;
    free(buf);
    printf("  wget %s: %d/%lldB -> %s\n", rel, (int)got,
           (long long)total, esp_err_to_name(e));
}

#if CONFIG_ESP_CUBE_FACE_PERF
/* faceperf start <秒> [片段] | report | stop (任务 31.4; 实现与语义见 face_perf.h)。
 * start 立即返回, 测量窗在 LVGL 任务里到时自动结束并恢复片段; 测量前后的 cputop 由操作者另行执行。 */
static void cmd_faceperf(char *arg)
{
    static const char usage[] =
        "  用法: faceperf start <1-300秒> [smile_static|laugh|investigate|shocked]"
        " | faceperf report | faceperf stop\n";
    char *save = NULL;
    char *sub = arg != NULL ? strtok_r(arg, " ", &save) : NULL;
    if (sub != NULL && strcmp(sub, "start") == 0) {
        char *s_sec = strtok_r(NULL, " ", &save);
        char *clip = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        char *end = NULL;
        unsigned long sec = s_sec != NULL ? strtoul(s_sec, &end, 10) : 0;
        if (s_sec == NULL || end == s_sec || *end != 0 || sec == 0 ||
            sec > FACE_PERF_WIN_MAX_S || extra != NULL) {
            printf("%s", usage);
            return;
        }
        esp_err_t e = face_perf_start_clip((uint32_t)sec, clip);
        if (e == ESP_OK) {
            printf("  faceperf: started %lus clip=%s(到时后 faceperf report)\n",
                   sec, clip != NULL ? clip : "current");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("%s", usage);
        } else {
            printf("  faceperf: start failed (%s)\n", esp_err_to_name(e));
        }
    } else if (sub != NULL && strcmp(sub, "report") == 0) {
        face_perf_report(printf);
    } else if (sub != NULL && strcmp(sub, "stop") == 0) {
        printf("  faceperf: stop %s\n", esp_err_to_name(face_perf_stop()));
    } else {
        printf("%s", usage);
    }
}
#endif

#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
/* uimood <tag>: 经 ui_port 单写者接口改快照 mood_tag(任务 31.4; 切换测试用) */
static void cmd_uimood(char *arg)
{
    extern esp_err_t ui_port_mood_req(const char *tag);   /* ui_port_esp.c */
    char *save = NULL;
    char *tag = arg != NULL ? strtok_r(arg, " ", &save) : NULL;
    esp_err_t e = tag != NULL ? ui_port_mood_req(tag) : ESP_ERR_INVALID_ARG;
    if (e == ESP_OK) {
        printf("  uimood -> %s\n", tag);
    } else if (e == ESP_ERR_INVALID_ARG) {
        printf("  用法: uimood <happy|relaxed|angry|sad|surprised|confused|sleepy|neutral>\n");
    } else {
        printf("  uimood: %s\n", esp_err_to_name(e));
    }
}
#endif

static void handle_line(char *line)
{
    char *sp = strchr(line, ' ');
    char *arg = NULL;
    if (sp != NULL) {
        *sp = 0;
        arg = sp + 1;
        while (*arg == ' ') arg++;
    }
    if (strcmp(line, "help") == 0) {
        printf("  ncm <关键词>  网易云搜索+播放 | ncmn <n> 播第n条\n"
               "  radio 回电台轮播 | st <n> 切台 | stop 停止\n"
               "  note <文本>   记琐事 | notes 看全部 | adel <n> 删一条\n"
               "  remind <秒> <文本> 定时提醒(到点语音播报) | time 校时状态\n"
               "  skill <名> <json> 直接执行技能(零 LLM 成本调试)\n"
               "  panel <0|1|2>  开关面板(0收起 1状态栏 2AI状态)\n"
               "  sdretry <0|1> SD 重挂载(0=MMC 1=SDSPI) | sdprobe 电平诊断\n"
               "  mem            内存预算审计+分域水位(docs/22)\n"
               "  memcheck       TLSF 堆完整性校验(§55.10 腐蚀诊断)\n"
               "  pairunbid <id> 撤销配对客户端(FR-02; 表满腾位)\n"
#if CONFIG_ESP_CUBE_ENABLE_PROV
               "  prov start [秒] P3a 配网窗口(SoftAP+Security2) | prov stop\n"
#endif
#if CONFIG_ESP_CUBE_ENABLE_AI
               "  ask <文本>    AI 会话(LLM 自主调用技能点歌/控台)\n"
#endif
#if CONFIG_ESP_CUBE_ENABLE_SOUL
               "  bb 黑板快照 | bh 行为分数 | bbset <键> <值> [ttl] 台调\n"
#endif
#if CONFIG_ESP_CUBE_FACE_PERF
               "  faceperf start <秒> [smile_static|laugh|investigate|shocked] 表情计时窗"
               " | faceperf report | faceperf stop\n"
#endif
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
               "  uimood <tag>  台调 mood(happy relaxed angry sad surprised confused"
               " sleepy neutral)\n"
#endif
        );
    } else if (strcmp(line, "ncm") == 0) {
        cmd_ncm(arg);
    } else if (strcmp(line, "ncmn") == 0) {
        cmd_play_via_gate((arg != NULL ? atoi(arg) : 0) - 1);
    } else if (strcmp(line, "psn") == 0) {
        /* T20 台调: 角色列表/激活状态/代次/授权 */
        persona_svc_t *ps = act_gate_persona();
        printf("  epoch=%lu switching=%d\n", (unsigned long)persona_epoch(ps),
               (int)persona_switching(ps));
        for (int i = 0; i < ps->n; i++) {
            printf("  [%d] #%08lx %s v%u%s%s mem=%d evo=%d "
                   "int=%.2f r=%d\n", i,
                   (unsigned long)ps->slot[i].persona_id,
                   ps->slot[i].display_name, ps->slot[i].version,
                   ps->slot[i].draft ? " (draft)" : "",
                   i == ps->active ? " [active]" : "",
                   persona_consent_memory(ps, ps->slot[i].persona_id),
                   persona_consent_evolution(ps, ps->slot[i].persona_id),
                   persona_evo_value(ps, ps->slot[i].persona_id,
                                     PSN_DIM_INTEREST),
                   persona_evo_revision_count(ps,
                                              ps->slot[i].persona_id));
        }
    } else if (strcmp(line, "psnsw") == 0) {
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: psnsw <id 十六进制>(见 psn 输出)\n");
        } else {
        uint32_t id = (uint32_t)strtoul(arg, NULL, 16);
        persona_svc_t *ps = act_gate_persona();
        if (persona_activate_begin(ps, id, 0) == PSN_RES_OK &&
            persona_activate_commit(ps) == PSN_RES_OK) {
            persona_save(ps);
            cact_set_persona_epoch(cact_default(), persona_epoch(ps));
            /* T21: 切角色 → 记忆作用域重绑(隔离) */
            char key[16];
            snprintf(key, sizeof(key), "%08lx",
                     (unsigned long)persona_active_id(ps));
#if CONFIG_ESP_CUBE_ENABLE_SOUL
            soul_mem_scope_bind(key,
                                persona_consent_memory(ps,
                                                       persona_active_id(ps)));
#endif
            printf("  psn: active #%08lx epoch=%lu\n",
                   (unsigned long)persona_active_id(ps),
                   (unsigned long)persona_epoch(ps));
        } else {
            printf("  psnsw: rejected (id=%s)\n", arg ? arg : "-");
        }
        }
    } else if (strcmp(line, "psnclear") == 0) {
        /* 用户裁决(2026-11-13): 测试阶段角色资料不保留 —— 不可逆清除 */
        act_gate_persona_clear();
        persona_svc_t *ps = act_gate_persona();
        printf("  psnclear: 已清除并重建默认角色\n");
        printf("  epoch=%lu switching=%d\n",
               (unsigned long)persona_epoch(ps),
               (int)persona_switching(ps));
        for (int i = 0; i < ps->n; i++) {
            printf("  [%d] #%08lx %s v%u%s\n", i,
                   (unsigned long)ps->slot[i].persona_id,
                   ps->slot[i].display_name, ps->slot[i].version,
                   i == ps->active ? " [active]" : "");
        }
    } else if (strcmp(line, "psnconsent") == 0) {
        /* 台调: psnconsent <id> <0|1> [0|1 演化] —— 授权变更 */
        char *tok = arg ? strtok(arg, " ") : NULL;
        char *tok2 = arg ? strtok(NULL, " ") : NULL;
        char *tok3 = arg ? strtok(NULL, " ") : NULL;
        if (tok == NULL || tok2 == NULL) {
            printf("  用法: psnconsent <id hex> <0|1 记忆> [0|1 演化]\n");
        } else {
            uint32_t id = (uint32_t)strtoul(tok, NULL, 16);
            bool on = tok2[0] == '1';
            persona_svc_t *ps = act_gate_persona();
            bool evo_on = tok3 != NULL ? tok3[0] == '1'
                                       : persona_consent_evolution(ps, id);
            if (persona_consent_set(ps, id, on, evo_on) == PSN_RES_OK) {
                persona_save(ps);
                uint32_t act = persona_active_id(ps);
                if (act == id) {
                    char key[16];
                    snprintf(key, sizeof(key), "%08lx",
                             (unsigned long)act);
#if CONFIG_ESP_CUBE_ENABLE_SOUL
                    soul_mem_scope_bind(key, on);   /* 重绑作用域 */
#endif
                }
                printf("  psnconsent: #%08lx memory=%d epoch=%lu\n",
                       (unsigned long)id, (int)on,
                       (unsigned long)persona_consent_epoch(ps, id));
            } else {
                printf("  psnconsent: rejected\n");
            }
        }
    } else if (strcmp(line, "memstat") == 0) {
        /* T21/T23 台调: 记忆作用域/授权/条数(数值经 soul 查询接口) */
        printf("  mem: scope+consent 见 mem scope 日志; 条数经 bh/面板\n");
        printf("  memtest <文本> 写入 | memdel <seq> 删除(台账防复活)\n");
    } else if (strcmp(line, "memtest") == 0) {
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: memtest <文本>(写入当前作用域; 需 consent=1)\n");
        } else {
#if CONFIG_ESP_CUBE_ENABLE_SOUL
            soul_mem_record(arg, "台调回复", 0.1f, 0.1f);
            printf("  memtest: 已提交(record; 是否持久见 consent/SD)\n");
#else
            printf("  memtest: SOUL 未启用\n");
#endif
        }
    } else if (strcmp(line, "memdel") == 0) {
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: memdel <seq>\n");
        } else {
#if CONFIG_ESP_CUBE_ENABLE_SOUL
            soul_mem_del_res_t r = soul_mem_delete(
                (uint32_t)strtoul(arg, NULL, 10));
            printf("  memdel #%s: %s\n", arg,
                   r == SOUL_MEM_DEL_OK ? "已删除(台账落盘)" :
                   r == SOUL_MEM_DEL_PENDING ? "pending(已隐藏,未持久)" :
                   r == SOUL_MEM_DEL_NOT_FOUND ? "不存在" : "拒绝");
#else
            printf("  memdel: SOUL 未启用\n");
#endif
        }
    } else if (strcmp(line, "radio") == 0) {
        cmd_play_via_gate(-1);
    } else if (strcmp(line, "st") == 0) {
        cmd_play_via_gate((arg != NULL ? atoi(arg) : 1) - 1);
    } else if (strcmp(line, "stop") == 0) {
        /* CLI 停止 = 用户停止: 锁存组1 + 经统一门(与语音停止同语义) */
        act_gate_user_stop_audio("cli_stop");
        uint32_t id = 0;
        act_gate_submit("music.stop", NULL, 0,
                            CACT_ORIGIN_PHYSICAL_USER, &id);
        act_gate_pump();
        printf("  stopped (%s)\n",
               act_gate_started(id) ? "via gate" : "gate busy");
    } else if (strcmp(line, "time") == 0 || strcmp(line, "notes") == 0) {
        cmd_assist_list();
    } else if (strcmp(line, "note") == 0) {
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: note <文本>\n");
        } else {
            uint32_t id = 0;
            esp_err_t err = app_assist_add(arg, 0, &id);
            printf("  note: %s (id=%u)\n", esp_err_to_name(err),
                   (unsigned)id);
        }
    } else if (strcmp(line, "remind") == 0) {
        /* remind <秒> <文本>: 秒级相对时间(测试友好, 语音走分钟) */
        char *sp = arg ? strchr(arg, ' ') : NULL;
        int secs = arg ? atoi(arg) : 0;
        if (sp == NULL || secs <= 0) {
            printf("  用法: remind <秒> <文本>\n");
        } else if (!app_assist_time_synced()) {
            printf("  时间未同步, 稍等 SNTP\n");
        } else {
            uint32_t id = 0;
            esp_err_t err = app_assist_add(sp + 1,
                                           app_assist_now() + secs, &id);
            printf("  remind: %s (id=%u, %ds 后)\n", esp_err_to_name(err),
                   (unsigned)id, secs);
        }
    } else if (strcmp(line, "adel") == 0) {
        if (arg == NULL) {
            printf("  用法: adel <编号>\n");
        } else {
            printf("  adel: %s\n",
                   esp_err_to_name(app_assist_remove((uint32_t)atoi(arg))));
        }
    } else if (strcmp(line, "skill") == 0) {
        /* skill <名> <json>: 直接路由到技能执行体(不经 LLM) */
        char *sp = arg ? strchr(arg, ' ') : NULL;
        if (arg == NULL || sp == NULL) {
            printf("  用法: skill <名> <json>(如: skill memo.add "
                   "{\"text\":\"买菜\"})\n");
        } else {
            *sp = 0;
            cJSON *args = cJSON_Parse(sp + 1);
            char reply[256];
            esp_err_t err = ai_skill_exec(arg, args, reply, sizeof(reply));
            cJSON_Delete(args);
            printf("  skill %s: %s\n  reply: %s\n", arg,
                   err == ESP_OK ? "ok" : esp_err_to_name(err), reply);
        }
    } else if (strcmp(line, "panel") == 0) {
        /* 面板开关(无触摸期 CLI 入口): panel 1 状态栏 / 2 AI状态 / 0 收起 */
        extern void ui_port_panel_req(int which);
        int n = arg != NULL ? atoi(arg) : 0;
        ui_port_panel_req(n == 0 ? 3 : n);
        printf("  panel -> %d\n", n);
    } else if (strcmp(line, "assistclear") == 0) {
        printf("  clear: %s\n", esp_err_to_name(app_assist_clear()));
#if CONFIG_ESP_CUBE_ENABLE_AI
    } else if (strcmp(line, "voice") == 0) {
        /* TTS 音色甄选台调: voice <名> 热切(下一轮 ask/announce 生效);
         * 无参打印当前 + 候选(v3-flash 预置, _v3 后缀命名) */
        if (arg != NULL && arg[0] != 0) {
            ai_session_set_tts_voice(arg);
            printf("  voice -> %s\n", ai_session_tts_voice());
        } else {
            printf("  当前: %s\n  候选(voice <名> 切换, 完整表见官方音色列表):\n"
                   "   女: longanqin_v3 亲和活泼(默认) | longanrou_v3 温柔闺蜜"
                   " | longyuan_v3 温暖治愈 | longwan_v3 细腻柔声\n"
                   "   男: longanyang 阳光(备选) | longanyun_v3 居家暖男"
                   " | longze_v3 温暖元气 | longtian_v3 磁性理智\n",
                   ai_session_tts_voice());
        }
#endif
    } else if (strcmp(line, "face") == 0) {
        /* 表情素材热切: face <名> = /sdcard/face/<名>.efc(M4+ 素材库) */
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: face <名>(SD face/ 下不含 .efc 后缀, 如 face idle)\n");
        } else {
            /* T13: 台调换脸经统一门 face.show(执行器仍走 SOUL_ACT 下游) */
            uint8_t blob[24];
            size_t n = strlcpy((char *)blob, arg, sizeof(blob)) + 1;
            if (n > sizeof(blob)) n = sizeof(blob);  /* 批次 44: strlcpy 返回源全长, 超长参数不钳位则 submit 越界读 */
            uint32_t id = 0;
            cact_res_t r = act_gate_submit("face.show", blob, n,
                                           CACT_ORIGIN_DEBUG, &id);
            act_gate_pump();
            printf("  face %s: %s\n", arg,
                   (r == CACT_RES_OK && act_gate_started(id))
                       ? "已提交(started)" : "被拒绝/失败");
        }
#else
        printf("  face: EFC 未启用\n");
#endif
#if CONFIG_ESP_CUBE_FACE_PERF
    } else if (strcmp(line, "faceperf") == 0) {
        cmd_faceperf(arg);
#endif
#if CONFIG_ESP_CUBE_ENABLE_FACE_ANIM
    } else if (strcmp(line, "uimood") == 0) {
        cmd_uimood(arg);
#endif
    } else if (strcmp(line, "wget") == 0) {
        /* 素材/配置传输通道: wget <url> [相对路径](缺省 face/下载名.efc);
         * PC 侧 python3 -m http.server 配合, http 明文(局域网) */
        cmd_wget(arg);
    } else if (strcmp(line, "think") == 0) {
        /* 灵魂 2.0 台调: 强制触发深思轮(绕过思维闸, 出口门照常) */
#if CONFIG_ESP_CUBE_SOUL_PROACTIVE
        extern bool soul_think_fire(uint8_t r, bool f, float s);
        printf("  think: %s\n", soul_think_fire(0xFF, true, 1.0f)
               ? "已触发(结果见 soulthink 日志)" : "忙(上次深思未收口)");
#else
        printf("  think: SOUL_PROACTIVE 未开\n");
#endif
#if CONFIG_ESP_CUBE_ENABLE_SOUL
    } else if (strcmp(line, "bb") == 0) {
        /* L1 因子黑板快照(可观测性; 评分器/深思/prompt 同源) */
        static char bbuf[720];
        soul_bb_dump(bbuf, sizeof(bbuf));
        printf("  bb: %s\n", bbuf);
    } else if (strcmp(line, "bh") == 0) {
        /* L2 行为注册表(最近一轮分数/阈值) */
        static char hbuf[512];
        soul_behavior_dump(hbuf, sizeof(hbuf));
        printf("  bh:\n%s\n", hbuf);
    } else if (strcmp(line, "bbset") == 0) {
        /* 黑板台调覆写: bbset <键名|序号> <值> [ttl秒](缺省 300;
         * 如 bbset quiet 1 600 → 验证 radio_stop 勿扰自停) */
        char key[24];
        float val;
        int ttl = 300;
        int n = arg != NULL
            ? sscanf(arg, "%23s %f %d", key, &val, &ttl) : 0;
        if (n >= 2) {
            int id = -1;
            if (key[0] >= '0' && key[0] <= '9') {
                id = atoi(key);
            } else {
                for (int i = 0; i < SOUL_BB_CNT; i++) {
                    if (strcmp(key, soul_bb_name(i)) == 0) { id = i; break; }
                }
            }
            if (id >= 0 && id < SOUL_BB_CNT) {
                soul_bb_override(id, val, ttl > 0 ? (uint32_t)ttl : 0);
                printf("  bbset %s=%+.2f ttl=%ds\n",
                       soul_bb_name(id), val, ttl);
            } else {
                printf("  bbset: 未知键 %s(bb 看键名)\n", key);
            }
        } else {
            printf("  用法: bbset <键名> <值> [ttl秒](如 bbset quiet 1 600)\n");
        }
#endif
    } else if (strcmp(line, "sdretry") == 0) {
        /* SD 台调: sdretry [0|1] 0=SDMMC slot0 / 1=SDSPI(SPI2);
         * 失败可反复重试(插拔卡后无需重启) */
        int mode = arg != NULL ? atoi(arg) : 0;
        if (mode != 0 && mode != 1) {
            printf("  用法: sdretry [0|1]  0=MMC slot0 / 1=SDSPI\n");
        } else {
            printf("  sd retry(mode=%d): %s\n", mode,
                   esp_err_to_name(app_storage_mount_mode(mode)));
        }
    } else if (strcmp(line, "sdprobe") == 0) {
        /* SD 台调: 引脚电平全景(插/拔卡各跑一次对比) */
        app_storage_probe();
#if CONFIG_ESP_CUBE_ENABLE_AI
    } else if (strcmp(line, "ask") == 0) {
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: ask <文本>(如: ask 放一首周杰伦的晴天)\n");
        } else {
            esp_err_t err = ai_session_submit_text(arg);
            printf("  ai: %s\n", err == ESP_OK
                   ? "已提交(AI 回复将经 TTS 播出, 串口同步打印)"
                   : esp_err_to_name(err));
        }
    } else if (strcmp(line, "selftest") == 0) {
        /* 自动化验证: TTS 合成人声注入麦克走完整轮(唤醒→ASR→LLM→技能) */
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: selftest <文本>\n");
        } else {
            printf("  ST: submit(\"%s\")\n", arg);
            esp_err_t err = ai_session_selftest(arg);
            if (err != ESP_OK) printf("  ST: %s\n", esp_err_to_name(err));
        }
    } else if (strcmp(line, "stwake") == 0) {
        /* 自动化验证: 合成唤醒词注入麦克, 期望 wn9 检出 ★ WAKE */
        printf("  ST: wake inject\n");
        ai_session_stwake();
#endif
    } else if (strcmp(line, "certup") == 0) {
        /* 证书灌入: certup <ca|srv|key> <base64 块>(分块追加) */
        if (arg == NULL || strtok(arg, " ") == NULL) {
            printf("  用法: certup <ca|srv|key> <base64 块>\n");
        } else {
            char *b64 = strtok(NULL, " ");
            printf("  certup %s: %s\n", arg,
                   pair_cert_append(arg, b64) == 0 ? "追加 OK" : "失败");
        }
    } else if (strcmp(line, "certdone") == 0) {
        if (arg == NULL) {
            printf("  用法: certdone <ca|srv|key>\n");
        } else {
            printf("  certdone %s: %s\n", arg,
                   pair_cert_commit(arg) == 0 ? "入 NVS OK" : "失败");
        }
    } else if (strcmp(line, "certstat") == 0) {
        pair_cert_status();
    } else if (strcmp(line, "pairwin") == 0) {
        /* 开配对窗口(120s): 屏显 PIN 的物理入口归 T16 */
        char pin[PAIR_PIN_LEN + 1];
        if (pair_window_open_cmd() == PAIR_RES_OK) {
            pair_window_pin(pin);
            printf("  PIN: %s (120s)\n", pin);
        } else {
            printf("  pairwin: 失败\n");
        }
    } else if (strcmp(line, "pairunbid") == 0) {
        /* FR-02 取消配对: 撤销指定 client_id(表满时腾位; NVS 持久) */
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: pairunbid <client_id>\n");
        } else {
            long id = strtol(arg, NULL, 10);
            int rc = pair_revoke_cmd((uint32_t)id);
            printf("  pairunbid %ld: %s (剩余 %d)\n", id,
                   rc == PAIR_RES_OK ? "已撤销" : "未找到",
                   pair_client_count_pub());
        }
    } else if (strcmp(line, "mem") == 0) {
        /* docs/22 预算对账: 一次性快照(启动完成时已自动审一次);
         * 长跑 P50/P95 采样仍用 baseline */
        mem_budget_dump();
    } else if (strcmp(line, "memcheck") == 0) {
        /* §55.10 堆腐蚀专项: TLSF 全域完整性校验(只读诊断,
         * heap_caps_check_integrity 逐块走查空闲链/魔数) */
        bool ok = heap_caps_check_integrity(MALLOC_CAP_INVALID, true);
        printf("  heap integrity: %s\n", ok ? "PASS" : "CORRUPT");
#if CONFIG_ESP_CUBE_ENABLE_PROV
    } else if (strcmp(line, "prov") == 0) {
        /* P3a SoftAP 首配窗口: 实际工作在瞬态 prov 任务(T20 教训:
         * CLI 上下文不做重活); 此处只开窗/收窗/查状态 */
        if (arg != NULL && strncmp(arg, "start", 5) == 0) {
            char *sp = strchr(arg, ' ');
            int t = sp != NULL ? atoi(sp + 1) : 0;
            printf("  prov start: %s\n", esp_err_to_name(app_prov_start(t)));
        } else if (arg != NULL && strcmp(arg, "stop") == 0) {
            printf("  prov stop: %s\n", esp_err_to_name(app_prov_stop()));
        } else {
            printf("  prov: 窗口 %s | 用法: prov start [秒=180] | prov stop\n",
                   app_prov_active() ? "开启中" : "关闭");
        }
#endif
    } else if (strcmp(line, "pairserve") == 0) {
        printf("  pair server: %s\n",
               pair_server_start() == 0 ? "started :443" : "start 失败(证书齐?)");
    } else if (strcmp(line, "pairstop") == 0) {
        pair_server_stop();
        printf("  pair server stopped\n");
    } else if (strcmp(line, "baseline") == 0) {
        /* T44 基线采集: N×500ms 采样内存/任务水位 → P50/P95/P99 */
        int n = arg != NULL ? atoi(arg) : 20;
        if (n < 5) n = 5;
        if (n > 200) n = 200;
        unsigned *s_free = heap_caps_malloc((size_t)n * sizeof(unsigned),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        unsigned *s_larg = heap_caps_malloc((size_t)n * sizeof(unsigned),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        unsigned *s_ps = heap_caps_malloc((size_t)n * sizeof(unsigned),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_free == NULL || s_larg == NULL || s_ps == NULL) {
            printf("  baseline: 缓冲分配失败\n");
            free(s_free); free(s_larg); free(s_ps);
        } else {
            for (int i = 0; i < n; i++) {
                s_free[i] = esp_get_free_internal_heap_size();
                s_larg[i] = heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                s_ps[i] = (unsigned)heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            /* 升序排序取分位 */
            unsigned tmp;
            for (int pass = 0; pass < n; pass++) {
                for (int i = 0; i + 1 < n - pass; i++) {
                    if (s_free[i] > s_free[i + 1]) {
                        tmp = s_free[i]; s_free[i] = s_free[i + 1];
                        s_free[i + 1] = tmp;
                    }
                    if (s_larg[i] > s_larg[i + 1]) {
                        tmp = s_larg[i]; s_larg[i] = s_larg[i + 1];
                        s_larg[i + 1] = tmp;
                    }
                    if (s_ps[i] > s_ps[i + 1]) {
                        tmp = s_ps[i]; s_ps[i] = s_ps[i + 1];
                        s_ps[i + 1] = tmp;
                    }
                }
            }
            printf("  baseline n=%d (B)\n", n);
            printf("  internal free : min=%u p50=%u p95=%u p99=%u\n",
                   s_free[0], s_free[n / 2], s_free[(n * 95) / 100],
                   s_free[(n * 99) / 100]);
            printf("  largest block : min=%u p50=%u p95=%u p99=%u\n",
                   s_larg[0], s_larg[n / 2], s_larg[(n * 95) / 100],
                   s_larg[(n * 99) / 100]);
            printf("  psram free    : min=%u p50=%u p95=%u p99=%u\n",
                   s_ps[0], s_ps[n / 2], s_ps[(n * 95) / 100],
                   s_ps[(n * 99) / 100]);
            free(s_free); free(s_larg); free(s_ps);
        }
    } else if (strcmp(line, "cputop") == 0) {
        /* 逐任务 CPU%(运行时统计; UI 卡顿诊断, 批次 21) */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        /* 大缓冲走堆(PSRAM): 内部静态曾压垮 CLI 任务栈分配(批次 21) */
        char *cbuf = heap_caps_malloc(2400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (cbuf == NULL) {
            printf("  cputop: 缓冲分配失败\n");
        } else {
            vTaskGetRunTimeStats(cbuf);
            printf("  Task            State Prio  CPU%%   Time(us)\n%s", cbuf);
            free(cbuf);
        }
#else
        printf("  cputop: FREERTOS_GENERATE_RUN_TIME_STATS 未开\n");
#endif
    } else if (strcmp(line, "netstat") == 0) {
        /* 链路二分(批次 20): RSSI 读失败=关联丢失(AP 侧); RSSI 正常但
         * ARP 不通=数据路径死(hosted RX); lwip 统计看丢包/内存错误 */
        wifi_ap_record_t ap;
        esp_err_t rssi_e = esp_wifi_sta_get_ap_info(&ap);
        if (rssi_e == ESP_OK) {
            printf("  link: rssi=%d 信道=%d (关联在)\n",
                   ap.rssi, ap.primary);
        } else {
            printf("  link: get_ap_info %s —— 关联丢失(AP 侧踢出?)\n",
                   esp_err_to_name(rssi_e));
        }
        wifi_ps_type_t ps;
        if (esp_wifi_get_ps(&ps) == ESP_OK) {
            printf("  power save: %s\n",
                   ps == WIFI_PS_NONE ? "NONE(常醒)" :
                   ps == WIFI_PS_MIN_MODEM ? "MIN_MODEM(modem sleep)" :
                   "MAX_MODEM");
        }
#if CONFIG_LWIP_STATS
        printf("  lwip link: recv=%u drop=%u err=%u\n",
               (unsigned)lwip_stats.link.recv,
               (unsigned)lwip_stats.link.drop,
               (unsigned)lwip_stats.link.err);
#if MEM_STATS
        printf("  lwip mem: err=%u avail=%u\n",
               (unsigned)lwip_stats.mem.err,
               (unsigned)lwip_stats.mem.avail);
#endif
        printf("  lwip tcp: drop=%u err=%u\n",
               (unsigned)lwip_stats.tcp.drop,
               (unsigned)lwip_stats.tcp.err);
#endif
#if CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
        {
            char *tbuf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (tbuf != NULL) {
                vTaskList(tbuf);
                printf("  tasks(state prio hwm-words):\n%s", tbuf);
                free(tbuf);
            }
        }
#endif
        printf("  free internal: %u (largest block %u)\n",
               (unsigned)esp_get_free_internal_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else if (strcmp(line, "uimodel") == 0) {
        /* T18: UI 状态快照(断网屏/SD/校时/隐私三态证据) */
        extern const ui_model_t *ui_model(void);
        const ui_model_t *m = ui_model();
        printf("  wifi_up=%d sd_ok=%d time_synced=%d\n",
               (int)m->wifi_up, (int)m->sd_ok, (int)m->time_synced);
        printf("  ai_state=%d (0=待机 1-2=采集 3-4=云发送)\n",
               (int)m->ai_state);
    } else if (strcmp(line, "vol") == 0) {
        /* 音量直调(R0 豁免: 连续属性调节, 同 ui_ctl_set_volume;
         * 非离散动作不经统一门) */
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: vol <0-100>\n");
        } else {
            int pct = atoi(arg);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            audio_mixer_set_master_volume(pct);
            printf("  vol: %d%%\n", pct);
        }
    } else if (strcmp(line, "heapstat") == 0) {
        printf("  free internal: %u (min %u)\n",
               (unsigned)esp_get_free_internal_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size());
        printf("  free all: %u\n", (unsigned)esp_get_free_heap_size());
    } else if (strcmp(line, "truststat") == 0) {
        /* docs/20 §7: 信任域状态(不打印任何密钥) */
#if CONFIG_ESP_CUBE_ENABLE_SD
        printf("  SD EPSCV2: %s\n",
               app_storage_crypt_active() ? "启用(透明容器)" : "未启用");
#else
        printf("  SD EPSCV2: (本目标无 SD)\n");
#endif
        pair_cert_status();
        printf("  pair server: %s\n", pair_server_running() ? "on" : "off");
        extern int pair_client_count_pub(void);
        printf("  已配对客户端: %d\n", pair_client_count_pub());
    } else if (strcmp(line, "trustreset") == 0) {
        /* docs/20 §6: 恢复出厂口径(不可逆): 清 trust+pairc 区 */
        if (arg != NULL && strcmp(arg, "CONFIRM") == 0) {
            nvs_handle_t h;
            if (nvs_open("trust", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
            if (nvs_open("pairc", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
            printf("  trustreset: trust+pairc 已清(重启后 sd_key 重生, "
                   "SD 旧记忆不可读; 重新配对+重新灌证书)\n");
        } else {
            printf("  用法: trustreset CONFIRM (不可逆!)\n");
        }
    } else {
        printf("  未知命令(%s), help 查看\n", line);
    }
    fflush(stdout);
    ESP_LOGD(TAG, "cmd done");
}

/* 行装配状态机(双通道共享: UART0/CH343 与 USB-Serial-JTAG 各喂各的
 * 字节流, 命令行状态单份 —— 两通道同时敲字会串行, 实际单人使用无碍) */
static char s_line[CLI_LINE_MAX];
static int s_len;

static void cli_char(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        printf("\r\n");
        if (s_len > 0) {
            s_line[s_len] = 0;
            s_len = 0;
            handle_line(s_line);
        }
    } else if (ch == 0x08 || ch == 0x7f) {      /* 退格 */
        if (s_len > 0) {
            s_len--;
            printf("\b \b");
        }
    } else if (s_len == 0 && (ch & 0x80)) {
        /* 行首噪声字节(实测 0xFF: 主机打开串口的 DTR 切换毛刺, 间歇
         * 出现)——命令恒以 ASCII 开头, 直接丢弃; 行中 UTF-8 不受影响 */
    } else if (s_len < CLI_LINE_MAX - 1 && ch >= 0x20 && ch != 0x7f) {
        /* 接受 ≥0x80 字节: UTF-8 中文关键词直通(搜索必需) */
        s_line[s_len++] = (char)ch;
        putchar(ch);                        /* 本地回显(双控制台都见) */
        fflush(stdout);
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    /* 只装 RX 驱动; TX 走 esp_stdio 默认直写(见文件头) */
    esp_err_t err = uart_driver_install(CLI_UART, CLI_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart driver: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    /* 第二输入通道: USB-Serial-JTAG(/dev/ttyACM1)。CH343 主控台 TX 依赖
     * 主机 USB 状态(2026-11-18 两次 URB 挂起、无 root 不可 reset 实证),
     * 而 USB-SJ 独立枚举且烧录通道一直健康; stdout 本就双发
     * (CONSOLE_SECONDARY_USB_SERIAL_JTAG=y), 响应天然回显 ACM1 —— CLI
     * 从此双通道, 单点失效不再阻塞排障(docs/19 §55.7)。 */
    const usb_serial_jtag_driver_config_t usj_cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install((usb_serial_jtag_driver_config_t *)&usj_cfg)
        != ESP_OK) {
        ESP_LOGW(TAG, "usj driver fail(仅 UART0 输入)");
    }
#endif

    printf("\r\n=== EspCube CLI(help 查看命令) ===\r\n");
    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(CLI_UART, &ch, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            cli_char(ch);
            continue;
        }
#if CONFIG_IDF_TARGET_ESP32P4
        /* UART0 空闲间隙轮询 USB-SJ(≤100ms 延迟, 命令行足够) */
        uint8_t ub[32];
        int m = usb_serial_jtag_read_bytes(ub, sizeof(ub), 0);
        for (int i = 0; i < m; i++) {
            cli_char(ub[i]);
        }
#endif
    }
}

esp_err_t cli_start(void)
{
    /* §52 v5: EFC 解码已全部收拢到 taskLVGL 执行(ui_face_exec_in_lvgl),
     * CLI 不再触碰 PIE 指令 → 栈位置不再敏感(v4.1 曾用 DMA caps 挤占
     * taskLVGL 预算致 "cli degraded: no console")。
     * 栈预分配(docs/22 §3B): 内部静态 —— 控制台是排障通道, 曾因内部
     * 碎片创建失败(§52 级联教训), 链接期占位后此失败模式不存在 */
    static StaticTask_t s_cli_tcb;
    /* RTCRAM 定置(docs/22 §3B v6.1): 非 DMA/非 PIE/非实时栈入 LP SRAM,
     * 释放 HP DRAM 给 taskLVGL/hosted(v6 全入 DRAM 致 dma largest 崩到
     * 8704, 板上 boot loop 实证)。P4 无 RTC_DATA_ATTR 宏, 用原生段属性 */
    static StackType_t s_cli_stack[CUBE_TASK_CLI_STACK / sizeof(StackType_t)]
        __attribute__((section(".rtc.bss")));
    TaskHandle_t h = xTaskCreateStatic(cli_task, CUBE_TASK_CLI_NAME,
                                       CUBE_TASK_CLI_STACK, NULL,
                                       CUBE_TASK_CLI_PRIO,
                                       s_cli_stack, &s_cli_tcb);
    return h != NULL ? ESP_OK : ESP_FAIL;
}
