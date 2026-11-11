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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "cube_tasks.h"
#include "app_ncm.h"
#include "app_player.h"
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
