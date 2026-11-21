/*
 * test_soul_mem.c —— 记忆系统 P0 回归(宿主, 自带 main; docs/15 P0-1/P0-2/M1/M4/M5)
 *
 * 编译运行(仓库根; 或直接 tools/host_tests.sh):
 *   gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I firmware/components/app_soul/test/host_stubs \
 *       -I firmware/components/app_soul/include \
 *       -I firmware/components/app_storage/include \
 *       -o /tmp/test_soul_mem \
 *       firmware/components/app_soul/test/test_soul_mem.c \
 *       firmware/components/app_soul/src/soul_mem.c \
 *   && ASAN_OPTIONS=detect_leaks=0 /tmp/test_soul_mem
 *
 * 惯例同 test_soul_bb_host.c: 零 IDF 依赖, 桩头在 test/host_stubs/。
 * "重启" 用 fork 模拟(soul_mem 无去初始化, 子进程获得全新静态态);
 * SD 用真实临时文件(跨 fork 存活); 分配失败按大小窗口注入。
 * 全部数据为合成文本, 无真实用户内容。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "soul.h"           /* 被测公开接口(soul_mem_prompt_block) */
#include "app_storage.h"    /* 桩实现必须匹配真实签名 */
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "cubecore.h"       /* 桩: 事件总线 + strlcpy 原型 */

/* soul.h 只声明了 soul_mem_prompt_block; boot/record 在固件侧由 soul.c
 * 私有 extern —— 宿主测试自行声明(与 /tmp 复现口径一致) */
void soul_mem_boot(void);
void soul_mem_record(const char *user, const char *reply,
                     float valence, float arousal);

#define SCOPE_KEY   "aa000001"
#define SCOPE_FILE  "v1/personas/" SCOPE_KEY "/memory/episodes.tsv"

static int s_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); s_fail++; \
    } \
} while (0)

/* ---- 桩: strlcpy(glibc 未必提供; ESP-IDF newlib 有) ---- */
size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

/* ---- 桩: FreeRTOS 互斥量(单线程宿主, 恒成功) ---- */
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks)
{
    (void)h; (void)ticks; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t h) { (void)h; }

/* ---- 桩: heap_caps_malloc(按大小窗口注入失败) ---- */
static long s_fail_lo = -1, s_fail_hi = -1;   /* [lo,hi] 内的分配返回 NULL */
void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    if (s_fail_lo >= 0 && (long)n >= s_fail_lo && (long)n <= s_fail_hi) {
        return NULL;
    }
    return malloc(n);
}

/* ---- 桩: SD 存储(按 rel 路径多文件; T21 作用域隔离验证) ---- */
static int s_storage_present;
static int s_writes;

static void storage_use(const char *tag)
{
    (void)tag;                         /* 多文件模式: 清全部测试文件防残留 */
    if (system("rm -f /tmp/soulmem_t_*") != 0) {
        /* 清理失败不阻塞: 各场景 path 按 rel 隔离 */
    }
    s_storage_present = 1;
    s_writes = 0;
}

static void path_of(char *out, size_t cap, const char *rel)
{
    char safe[96];
    strlcpy(safe, rel, sizeof(safe));
    for (char *q = safe; *q; q++) {
        if (*q == '/') *q = '_';
    }
    snprintf(out, cap, "/tmp/soulmem_t_%s", safe);
}
static void storage_absent(void)
{
    s_storage_present = 0;
    s_writes = 0;
}
static void storage_seed(const char *rel, const char *content)
{
    char p[192];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "wb");
    if (f == NULL) { perror("seed"); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}
static char *storage_dump(const char *rel)
{
    if (!s_storage_present) return NULL;
    char p[192];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}
bool app_storage_ok(void) { return s_storage_present; }
char *app_storage_read(const char *rel, size_t *len)
{
    if (!s_storage_present) return NULL;
    char *buf = storage_dump(rel);
    if (buf != NULL && len != NULL) *len = strlen(buf);
    return buf;
}
static bool s_append_fail;
esp_err_t app_storage_append(const char *rel, const char *line)
{
    if (!s_storage_present || s_append_fail) return 1;
    char p[192];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "ab");
    if (f == NULL) return 1;
    fwrite(line, 1, strlen(line), f);
    fputc('\n', f);
    fclose(f);
    return ESP_OK;
}
esp_err_t app_storage_write(const char *rel, const void *data, size_t len)
{
    if (!s_storage_present) return 1;      /* 非 OK */
    char p[192];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "wb");
    if (f == NULL) return 1;
    fwrite(data, 1, len, f);
    fclose(f);
    s_writes++;
    return ESP_OK;
}

/* ---- 桩: 事件总线(只捕获订阅) ---- */
static cube_evt_cb_t s_sub_cb;
esp_err_t cube_event_subscribe(cube_evt_cb_t cb, void *user_ctx)
{
    (void)user_ctx;
    s_sub_cb = cb;
    return ESP_OK;
}

/* ---- 与 soul_mem.c 的 mem_item_t 镜像(打包布局必须一致) ---- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t seq;
    long long t;
    float strength;
    long long last_access;
    signed char av;
    signed char aa;
    char user[96];
    char reply[48];
} mirror_item_t;
#pragma pack(pop)
_Static_assert(sizeof(mirror_item_t) == 174,
               "镜像布局与 soul_mem mem_item_t 不一致, 请同步");
#define MIRROR_MAGIC 0x534D3031u

/* ---- fork 辅助: 子进程失败 → 场景失败 ---- */
static int wait_child_ok(int pid)
{
    int st = 1;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("  child pid=%d raw status=0x%x (exited=%d sig=%d code=%d)\n",
               pid, st, WIFEXITED(st), WTERMSIG(st),
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ============ 场景 ============ */

/* S1: 全字段往返(中英/空回复/最大长度/负情绪), 两轮重启 + 回写不丢原文 */
static void scenario_roundtrip(void)
{
    storage_use("rt");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    soul_mem_record("用户喜欢爵士乐", "好的，为你播放爵士电台", 0.4f, 0.3f);
    char u95[96], r47[48];
    memset(u95, 'a', sizeof(u95) - 1); u95[sizeof(u95) - 1] = 0;
    memcpy(u95, "U95LONG", 7);
    memset(r47, 'b', sizeof(r47) - 1); r47[sizeof(r47) - 1] = 0;
    memcpy(r47, "R47REPLY", 8);
    soul_mem_record(u95, r47, -0.9f, 0.1f);        /* 印象深 + 满长 */
    soul_mem_record("空回复条目", "", 0.0f, 0.0f);
    soul_mem_record("记得提醒我周三吃药", "好的，已记下", 0.1f, 0.2f);

    pid_t pid = fork();
    if (pid == 0) {                                 /* 重启 #1 */
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        const char *blk = soul_mem_prompt_block();
        CHECK(strstr(blk, "用户喜欢爵士乐") != NULL, "user1 loaded");
        CHECK(strstr(blk, "你当时说:好的，为你播放") != NULL,
              "reply1 paired with user1");
        CHECK(strstr(blk, "R47REPLY") != NULL, "long reply loaded");
        soul_mem_record("重启后新条目", "重启回复", 0, 0);
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "reboot1 checks");

    pid = fork();                                   /* 重启 #2: 查回写 */
    if (pid == 0) {
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        const char *blk = soul_mem_prompt_block();
        CHECK(strstr(blk, "用户喜欢爵士乐") != NULL,
              "original user survives rewrite");
        /* top-3 选择可能不含最新条目(强度/新近度决定), 落盘才算数 */
        char *dump = storage_dump(SCOPE_FILE);
        CHECK(dump != NULL && strstr(dump, "用户喜欢爵士乐") != NULL,
              "file keeps original user");
        CHECK(dump != NULL && strstr(dump, "重启后新条目") != NULL,
              "file keeps newest entry");
        CHECK(dump != NULL && strstr(dump, "重启回复") != NULL,
              "file keeps new reply");
        free(dump);
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "reboot2 checks");
}

/* S2: docs/15 P0-1 原样复现行 —— 旧加载器把 reply 当 user、回写丢原文 */
static void scenario_legacy_line(void)
{
    storage_use("legacy");
    storage_seed(SCOPE_FILE,
                 "1700000000\t1\t0.300\t0\t0\t1700000000\t"
                 "USER_ORIGINAL\tASSISTANT_REPLY\n");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    long w0 = s_writes;
    const char *blk = soul_mem_prompt_block();
    CHECK(strstr(blk, "USER_ORIGINAL") != NULL, "legacy user parsed as user");
    CHECK(strstr(blk, "你当时说:ASSISTANT_REPLY") != NULL,
          "legacy reply stays reply");
    CHECK(s_writes == w0, "boot rebuild must not write (M1 不写盘)");
    soul_mem_record("新条目", "回复", 0, 0);
    char *dump = storage_dump(SCOPE_FILE);
    CHECK(dump != NULL && strstr(dump, "USER_ORIGINAL") != NULL,
          "rewrite keeps original user");
    CHECK(dump != NULL && strstr(dump, "ASSISTANT_REPLY") != NULL,
          "rewrite keeps original reply");
    free(dump);
}

/* S3: 内容含 \t 的旧歧义行 → 隔离, 不误归属 */
static void scenario_anomaly_line(void)
{
    storage_use("anom");
    storage_seed(SCOPE_FILE,
                 "1700000000\t1\t0.300\t0\t0\t1700000000\t"
                 "干净条目用户\t干净回复\n"
                 "1700000001\t2\t0.300\t0\t0\t1700000001\t"
                 "含\t制表符\tEXTRA\n");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    const char *blk = soul_mem_prompt_block();
    CHECK(strstr(blk, "干净条目用户") != NULL, "clean line loaded");
    CHECK(strstr(blk, "EXTRA") == NULL, "ambiguous line quarantined");
}

/* S4: docs/15 P0-2 精确触发形态 —— 两条满长记录, 第二条负情绪;
 * 旧代码在此 ASan global-buffer-overflow, 修复后必须完整存活 */
static void scenario_bounded_block(void)
{
    storage_use("bound");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    char u95[96], r47[48];
    memset(u95, 'a', sizeof(u95) - 1); u95[sizeof(u95) - 1] = 0;
    memcpy(u95, "U95LONG", 7);
    memset(r47, 'b', sizeof(r47) - 1); r47[sizeof(r47) - 1] = 0;
    memcpy(r47, "R47REPLY", 8);
    soul_mem_record(u95, r47, 0.2f, 0.1f);
    char v95[96];
    memset(v95, 'c', sizeof(v95) - 1); v95[sizeof(v95) - 1] = 0;
    memcpy(v95, "V95NEG", 6);
    soul_mem_record(v95, r47, -0.9f, 0.1f);        /* (印象深) → 旧溢出点 */
    const char *blk = soul_mem_prompt_block();
    CHECK(strlen(blk) < 384, "block within budget");
    CHECK(memchr(blk, 0, 384) != NULL, "nul terminated within buffer");
    CHECK(strstr(blk, "U95LONG") != NULL, "first entry present");
}

/* S5: 多条满长记录反复重建 → 块恒有界且可用 */
static void scenario_many_long(void)
{
    storage_use("many");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    char u95[96];
    for (int i = 0; i < 10; i++) {
        memset(u95, 'e', sizeof(u95) - 1); u95[sizeof(u95) - 1] = 0;
        snprintf(u95, 12, "LONG%02u", (unsigned)i);
        soul_mem_record(u95, "回复正文", 0, 0);
    }
    const char *blk = soul_mem_prompt_block();
    CHECK(strlen(blk) < 384, "block bounded after many long records");
    CHECK(memchr(blk, 0, 384) != NULL, "nul terminated");
}

/* S6: docs/15 M5 —— 512 数组分配失败退 60, SD 在位; cap 不得谎报 512 */
static void scenario_degraded_cap(void)
{
    storage_use("cap");
    s_fail_lo = 80000; s_fail_hi = 100000;   /* 只失败 512×174 的数组 */
    soul_mem_boot();                          /* 60 条数组; SD resize 再失败 */
    soul_mem_scope_bind(SCOPE_KEY, true);
    s_fail_lo = -1; s_fail_hi = -1;
    for (int i = 0; i < 100; i++) {           /* ASan 盯第 61 次越界写 */
        char u[32];
        snprintf(u, sizeof(u), "用户%02d号", i);
        soul_mem_record(u, "回", 0, 0);
    }
    pid_t pid = fork();
    if (pid == 0) {
        s_fail = 0;
        s_fail_lo = 80000; s_fail_hi = 100000;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        s_fail_lo = -1; s_fail_hi = -1;
        const char *blk = soul_mem_prompt_block();
        CHECK(blk[0] != '\0', "degraded boot still serves block");
        char *dump = storage_dump(SCOPE_FILE);
        CHECK(dump != NULL, "file exists");
        if (dump != NULL) {
            int lines = 0;
            for (const char *p = dump; *p; p++) lines += (*p == '\n');
            CHECK(lines <= 60, "no more than degraded cap on file");
            free(dump);
        }
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "degraded reboot checks");
}

/* S7: docs/15 M5 —— 全部分配失败: 安全停用, 绝不在未分配数组上写 */
static void scenario_all_alloc_fail(void)
{
    storage_use("off");
    s_fail_lo = 1; s_fail_hi = 1 << 30;
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);   /* disabled: 内部安全返回 */
    s_fail_lo = -1; s_fail_hi = -1;
    soul_mem_record("不应记录", "x", 0, 0);
    CHECK(s_writes == 0, "no write when disabled");
    const char *blk = soul_mem_prompt_block();
    CHECK(blk[0] != '\0', "empty-state served when disabled");
    pid_t pid = fork();
    if (pid == 0) {                            /* 分配恢复后可用 */
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        soul_mem_record("恢复后首条", "回复", 0, 0);
        CHECK(s_writes >= 1, "recovers after alloc restored");
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "recovery checks");
}

/* S8(T21): 作用域门 —— 未绑定/未授权时长期记忆不写 */
static void scenario_scope_gate(void)
{
    storage_absent();
    soul_mem_boot();
    /* 未绑定: record no-op */
    soul_mem_record("未绑定不应记录", "x", 0, 0);
    CHECK(s_writes == 0, "no write before scope bind");
    CHECK(strstr(soul_mem_prompt_block(), "未绑定不应记录") == NULL,
          "record ignored before bind");

    /* 绑定但授权关: record no-op */
    soul_mem_scope_bind(SCOPE_KEY, false);
    soul_mem_record("授权关不应记录", "y", 0, 0);
    CHECK(s_writes == 0, "no write without consent");
    CHECK(strstr(soul_mem_prompt_block(), "授权关不应记录") == NULL,
          "record ignored without consent");
    CHECK(strstr(soul_mem_prompt_block(), "共同回忆") != NULL,
          "empty-state prompt served");
}

/* S9: 写侧净化 —— 控制字符不得破坏 TSV 行, 重载字段不位移 */
static void scenario_sanitize(void)
{
    storage_use("sani");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    soul_mem_record("带\t制表符\n和换行", "回\t复", 0, 0);
    char *dump = storage_dump(SCOPE_FILE);
    CHECK(dump != NULL, "file written");
    if (dump != NULL) {
        const char *line = strstr(dump, "带 制表符");
        CHECK(line != NULL, "sanitized user on file");
        if (line != NULL) {
            const char *ls = line;              /* 回退到行首再数分隔符 */
            while (ls > dump && ls[-1] != '\n') ls--;
            int tabs = 0;
            for (const char *p = ls; *p && *p != '\n'; p++) {
                tabs += (*p == '\t');
            }
            CHECK(tabs == 7, "exactly 7 separators on stored line");
        }
        free(dump);
    }
    pid_t pid = fork();
    if (pid == 0) {
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        const char *blk = soul_mem_prompt_block();
        CHECK(strstr(blk, "带 制表符 和换行") != NULL, "user roundtrip");
        CHECK(strstr(blk, "你当时说:回 复") != NULL, "reply roundtrip");
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "sanitize reboot checks");
}


/* S10(T21 核心): A/B 角色记忆隔离 —— 写 A 不见 B, 切回 A 数据仍在 */
static void scenario_scope_isolation(void)
{
    storage_use("iso");
    soul_mem_boot();
    soul_mem_scope_bind("bb000001", true);
    soul_mem_record("甲角色的秘密", "甲回复", 0.3f, 0.2f);
    CHECK(s_writes >= 1, "A scope persisted");

    soul_mem_scope_bind("cc000002", true);      /* 切到乙: 空 */
    CHECK(strstr(soul_mem_prompt_block(), "甲角色的秘密") == NULL &&
          strstr(soul_mem_prompt_block(), "共同回忆") != NULL,
          "B scope starts empty (empty-state prompt)");
    soul_mem_record("乙角色的秘密", "乙回复", 0.3f, 0.2f);

    soul_mem_scope_bind("bb000001", true);      /* 切回甲: 数据仍在 */
    CHECK(strstr(soul_mem_prompt_block(), "甲角色的秘密") != NULL,
          "A memory visible again");
    CHECK(strstr(soul_mem_prompt_block(), "乙角色的秘密") == NULL,
          "B memory invisible in A");

    /* 子进程模拟重启: 各作用域文件独立恢复 */
    pid_t pid = fork();
    if (pid == 0) {
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind("cc000002", true);
        CHECK(strstr(soul_mem_prompt_block(), "乙角色的秘密") != NULL,
              "B scope survives reboot");
        CHECK(strstr(soul_mem_prompt_block(), "甲角色的秘密") == NULL,
              "A not leaked into B after reboot");
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "B reboot checks");
}

/* S12(T21 续): 按当前 query 检索精排 —— 中文 bigram 命中/不命中 */
static void scenario_query_retrieval(void)
{
    storage_use("qr");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    soul_mem_record("记得提醒我周三吃药", "好的已记下", 0.1f, 0.1f);   /* seq1 */
    soul_mem_record("用户喜欢听爵士乐", "好的", 0.1f, 0.1f);           /* seq2 */
    soul_mem_record("上周聊过周末爬山计划", "嗯嗯", 0.1f, 0.1f);       /* seq3 */

    /* query 命中"吃药"条目 → 该条进 prompt 且强化; 无关条目可被挤出 */
    soul_mem_query_rebuild("你现在要提醒我吃药了吗");
    const char *blk = soul_mem_prompt_block();
    const char *hit = strstr(blk, "记得提醒我周三吃药");
    const char *miss = strstr(blk, "爵士乐");
    CHECK(hit != NULL, "query hit entry ranked in");
    /* 命中项 score 最高 → 在块中先于无关项(top-3 可能全容纳, 比顺序) */
    CHECK(hit != NULL && (miss == NULL || hit < miss),
          "hit entry outranks unrelated");

    /* 命中项强化而未命中项不强化(rebuild after unrelated query) */
    soul_mem_query_rebuild("爵士乐歌单");
    blk = soul_mem_prompt_block();
    CHECK(strstr(blk, "用户喜欢听爵士乐") != NULL, "second query hits");

    /* 空 query → 默认块(不崩) */
    soul_mem_query_rebuild("");
    CHECK(soul_mem_prompt_block()[0] != '\0', "empty query keeps block");
}

/* S11(T23): 删除防复活 —— 台账先行, 残留/回滚/重启均不复活 */
/* S13: B-6b 反思触发面(imp 启发式派生+mark 水位+recent 取材) */
static void scenario_reflect_imp(void)
{
    storage_use("rimp");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    CHECK(soul_mem_imp_pending() == 0, "empty -> 0 pending");
    soul_mem_record("平静的一句", "嗯", 0.0f, 0.0f);   /* imp=10 */
    soul_mem_record("开心的一天", "真好", 0.9f, 0.5f); /* imp=45 */
    CHECK(soul_mem_imp_pending() == 55, "pending = 10+45");
    soul_mem_imp_mark();
    CHECK(soul_mem_imp_pending() == 0, "mark -> 0");
    /* 重启: wm 仅 RAM → 全量重开(v1 偏差, docs/24 §12.2-1) */
    pid_t pid = fork();
    if (pid == 0) {
        s_fail = 0;
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        CHECK(soul_mem_imp_pending() == 55, "reboot reopens window");
        char buf[512];
        int n = soul_mem_recent(buf, sizeof(buf), 15);
        CHECK(n >= 2, "recent rows");
        CHECK(strstr(buf, "主人说") != NULL, "recent format");
        CHECK(strstr(buf, "开心的一天") != NULL, "recent content");
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "reflect reboot checks");
}

static void scenario_scope_delete(void)
{
    storage_use("del");
    soul_mem_boot();
    soul_mem_scope_bind(SCOPE_KEY, true);
    soul_mem_record("将被删除的记忆", "删回复", 0.2f, 0.1f);   /* seq=1 */
    soul_mem_record("保留的记忆", "留回复", 0.2f, 0.1f);       /* seq=2 */
    CHECK(strstr(soul_mem_prompt_block(), "将被删除的记忆") != NULL,
          "before delete visible");

    /* 台账写失败 → PENDING: RAM 已隐藏但不虚报接受 */
    s_append_fail = true;
    soul_mem_del_res_t r = soul_mem_delete(1);
    CHECK(r == SOUL_MEM_DEL_PENDING, "ledger fail -> pending");
    s_append_fail = false;
    CHECK(strstr(soul_mem_prompt_block(), "将被删除的记忆") == NULL,
          "hidden even when pending");

    /* 重试 → OK: 台账落盘+重写 */
    r = soul_mem_delete(1);
    CHECK(r == SOUL_MEM_DEL_OK, "retry accepted");
    CHECK(strstr(soul_mem_prompt_block(), "保留的记忆") != NULL,
          "other memory intact");
    char *dump = storage_dump(SCOPE_FILE);
    CHECK(dump != NULL && strstr(dump, "将被删除的记忆") == NULL,
          "rewrite drops deleted entry");
    free(dump);

    /* 幂等: 再删 → OK(已在台账) */
    CHECK(soul_mem_delete(1) == SOUL_MEM_DEL_OK, "idempotent delete");
    /* 不存在 → NOT_FOUND */
    CHECK(soul_mem_delete(999) == SOUL_MEM_DEL_NOT_FOUND, "not found");

    /* 防复活: 手动把已删行追加回 tsv(模拟备份/回滚/旧残留) → 重启载入 */
    pid_t pid = fork();
    if (pid == 0) {
        s_fail = 0;
        FILE *f = fopen("/tmp/soulmem_t_v1_personas_aa000001_memory_episodes.tsv",
                        "a");
        if (f != NULL) {
            fputs("1700000100\t1\t0.300\t0\t0\t1700000100\t"
                  "将被删除的记忆\t删回复\n", f);
            fclose(f);
        }
        soul_mem_boot();
        soul_mem_scope_bind(SCOPE_KEY, true);
        CHECK(strstr(soul_mem_prompt_block(), "将被删除的记忆") == NULL,
              "deleted blocked by ledger after reboot");
        CHECK(strstr(soul_mem_prompt_block(), "保留的记忆") != NULL,
              "kept memory survives");
        _exit(s_fail ? 1 : 0);
    }
    CHECK(wait_child_ok(pid), "anti-resurrection reboot");
}

/* ============ 派发: 每个场景独立进程(静态态隔离) ============ */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* fork+_exit: 子进程输出不丢 */
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "roundtrip",    scenario_roundtrip },
        { "legacy_line",  scenario_legacy_line },
        { "anomaly_line", scenario_anomaly_line },
        { "bounded_blk",  scenario_bounded_block },
        { "many_long",    scenario_many_long },
        { "degraded_cap", scenario_degraded_cap },
        { "alloc_fail",   scenario_all_alloc_fail },
        { "scope_gate",   scenario_scope_gate },
        { "scope_iso",    scenario_scope_isolation },
        { "scope_delete", scenario_scope_delete },
        { "query_ret",    scenario_query_retrieval },
        { "reflect_imp",  scenario_reflect_imp },
        { "sanitize",     scenario_sanitize },
    };
    /* 调试: 指定场景名则内联直跑(不派发 fork), 崩溃现场直接可见 */
    if (argc > 1) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (strcmp(argv[1], cases[i].name) == 0) {
                cases[i].fn();
                printf("%s: %s\n", cases[i].name,
                       s_fail ? "FAIL" : "PASS");
                return s_fail ? 1 : 0;
            }
        }
        fprintf(stderr, "no such scenario: %s\n", argv[1]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pid_t pid = fork();
        if (pid == 0) {
            s_fail = 0;   /* fork 继承父进程计数, 子进程必须清零独立计 */
            cases[i].fn();
            if (s_fail) {
                printf("  scenario %s internal fails=%d\n",
                       cases[i].name, s_fail);
            }
            _exit(s_fail ? 1 : 0);
        }
        if (!wait_child_ok(pid)) {
            printf("FAIL scenario %s\n", cases[i].name);
            s_fail++;
        } else {
            printf("PASS scenario %s\n", cases[i].name);
        }
    }
    if (s_fail == 0) {
        printf("test_soul_mem: ALL PASS\n");
        return 0;
    }
    printf("test_soul_mem: %d FAILURE(S)\n", s_fail);
    return 1;
}
