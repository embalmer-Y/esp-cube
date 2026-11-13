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
#include "cube_tasks.h"
#include "app_ncm.h"
#include "app_player.h"
#include "app_assist.h"
#include "app_storage.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "ai_skills.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CUBE_ENABLE_AI
#include "ai_session.h"
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
#if CONFIG_ESP_CUBE_ENABLE_AI
               "  ask <文本>    AI 会话(LLM 自主调用技能点歌/控台)\n"
#endif
        );
    } else if (strcmp(line, "ncm") == 0) {
        cmd_ncm(arg);
    } else if (strcmp(line, "ncmn") == 0) {
        play_index((arg != NULL ? atoi(arg) : 0) - 1);
    } else if (strcmp(line, "radio") == 0) {
        printf("  player: %s\n",
               app_player_start(NULL) == ESP_OK ? "radio started" : "fail");
    } else if (strcmp(line, "st") == 0) {
        int n = arg != NULL ? atoi(arg) : 1;
        printf("  player: %s\n",
               app_player_play_index(n - 1) == ESP_OK ? "ok" : "fail");
    } else if (strcmp(line, "stop") == 0) {
        app_player_stop();
        printf("  stopped\n");
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
#if CONFIG_ESP_CUBE_ENABLE_EFC
        if (arg == NULL || arg[0] == 0) {
            printf("  用法: face <名>(SD face/ 下不含 .efc 后缀, 如 face idle)\n");
        } else {
            extern esp_err_t ui_port_face_req(const char *rel);
            esp_err_t e = ui_port_face_req(arg);
            printf("  face %s: %s(结果见 uiport 日志)\n", arg,
                   e == ESP_OK ? "已提交" : esp_err_to_name(e));
        }
#else
        printf("  face: EFC 未启用\n");
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
    } else {
        printf("  未知命令(%s), help 查看\n", line);
    }
    fflush(stdout);
    ESP_LOGD(TAG, "cmd done");
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

    printf("\r\n=== EspCube CLI(help 查看命令) ===\r\n");
    char line[CLI_LINE_MAX];
    int len = 0;
    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(CLI_UART, &ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;                       /* 无输入: 让出(喂看门狗) */
        }
        if (ch == '\r' || ch == '\n') {
            printf("\r\n");
            if (len > 0) {
                line[len] = 0;
                len = 0;
                handle_line(line);
            }
        } else if (ch == 0x08 || ch == 0x7f) {      /* 退格 */
            if (len > 0) {
                len--;
                printf("\b \b");
            }
        } else if (len == 0 && (ch & 0x80)) {
            /* 行首噪声字节(实测 0xFF: 主机打开串口的 DTR 切换毛刺, 间歇
             * 出现)——命令恒以 ASCII 开头, 直接丢弃; 行中 UTF-8 不受影响 */
        } else if (len < CLI_LINE_MAX - 1 && ch >= 0x20 && ch != 0x7f) {
            /* 接受 ≥0x80 字节: UTF-8 中文关键词直通(搜索必需) */
            line[len++] = (char)ch;
            putchar(ch);                    /* 本地回显 */
            fflush(stdout);
        }
    }
}

esp_err_t cli_start(void)
{
    BaseType_t ok = xTaskCreate(cli_task, CUBE_TASK_CLI_NAME,
                                CUBE_TASK_CLI_STACK, NULL,
                                CUBE_TASK_CLI_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
