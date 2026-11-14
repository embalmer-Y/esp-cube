/*
 * soul_mem.c —— 记忆系统轻量版(M3; Kaleido kaleido_memory 的 NVS 剪裁)
 *
 * 保留 Kaleido 记忆的核心语义, 裁掉文件系统/覆盖表/embedding:
 *   - episodic: 每轮对话一条 {user 摘要, 回复首句, 时间, 情绪定点};
 *     NVS blob 存储, 上限 60 条(NVS 分区 24KB 内的紧凑预算), 满即
 *     淘汰最旧(Kaleido 是停写, 嵌入式设备"忘掉最早的"更合理);
 *   - 检索: score = 0.5·recency + 0.3·strength + 0.2·match(2-gram
 *     字符匹配); 命中即 reinforce(strength+0.125, "越回忆越牢");
 *   - 注入: "[你们的共同回忆(未必精确)]" top-3, 相对时间(今天/昨天/
 *     N天前); 空库显式空态(防幻觉纪律, Kaleido 30号 §8.3);
 *   - core(用户既定事实)层: 暂不做(LLM 结构化抽取留给后续)。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "soul.h"
#include "app_storage.h"
#include "cubecore.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

#include "sdkconfig.h"

#if CONFIG_ESP_CUBE_ENABLE_SOUL

#define MEM_NS      "soulmem"
#define MEM_KEY_NXT "nextseq"
#define MEM_KEY_PFX "e"           /* eXXXXXXXX */
#define MEM_MAX     60       /* NVS 后端容量(SD 后端 512, 跨年对话不丢) */
#define MEM_MAX_SD  512
#define MEM_FILE    "mem/episodes.jsonl"
#define MEM_USR     96            /* 用户句(UTF-8 截断) */
#define MEM_REP     48            /* 回复首句 */
#define MEM_BLOCK   384           /* prompt 块预算 */

static const char *TAG = "soulmem";

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;               /* 'SM01' */
    uint32_t seq;
    int64_t  t;                   /* unix 秒 */
    float    strength;            /* [0,1] reinforce 递增 */
    int64_t  last_access;         /* 惰性遗忘/巩固用 */
    int8_t   affect_v;            /* valence 定点 [-127,127] */
    int8_t   affect_a;            /* arousal 定点 [0,127] */
    char     user[MEM_USR];
    char     reply[MEM_REP];
} mem_item_t;                     /* ~165B × 60 ≈ 9.9KB(NVS 内) */
#pragma pack(pop)

#define MEM_MAGIC 0x534D3031

/* ⚠ 必须 PSRAM: 512 条×~168B≈86KB, 静态数组会把内部堆压垮 ——
 * esp_hosted SDIO 聚合缓冲(15872B 连续内部 DMA)分配失败, 轻则 WiFi
 * 不通重则 assert 重启循环(2026-11-12 板上定位: 静态 86KB BSS 是
 * M4→本批唯一的大额内部内存增量) */
static mem_item_t *s_items;
static int s_cap = MEM_MAX;             /* 运行时后端容量(必须与实际分配一致) */
static bool s_sd;                       /* SD 后端在位 */
static bool s_disabled;                 /* 容量两次分配均失败: 整体安全停用 */
static bool s_scope_ok;                 /* T21: 已绑定 persona 作用域 */
static bool s_consent_mem;              /* 当前角色记忆授权(入口注入) */
static char s_scope_file[80];           /* v1/personas/<key>/memory/episodes.tsv */
/* T23 删除台账: 追加式独立记录, load 时过滤 —— 即使 episodes 文件残留
 * 旧内容(备份/回滚/再导入), 已删 seq 也被阻断(防复活, docs/17 §8.2) */
#define MEM_DEL_MAX 64
static uint32_t s_deleted[MEM_DEL_MAX];
static int s_deleted_n;
static char s_del_log_file[96];
static int s_count;
static int s_anomalies;                 /* 旧格式歧义行(含 \t)隔离计数 */
static SemaphoreHandle_t s_lock;
static uint32_t s_next_seq = 1;
static char s_block[MEM_BLOCK];   /* prompt 块(重建于写入/启动) */

/* 写入前净化: \t/\n/\r 等 C0 控制字符以空格替代, 保证 TSV 行永不被
 * 内容破坏(docs/15 P0-1 的写侧根治; 读侧仍按"恰好 7 个分隔符"严检) */
static void sanitize_text(char *dst, const char *src, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src;
         *p != '\0' && o + 1 < cap; p++) {
        unsigned char c = *p;
        dst[o++] = (c < 0x20 && c != '\0') ? ' ' : (char)c;
    }
    dst[o] = '\0';
}

/* ---- SD 后端: episodes.jsonl(实际 TSV 行; RAM 镜像为事实源,
 * 记录/淘汰后全量回写 —— 87KB@512 条 ≈0.2s, 记忆频度下可接受) ----
 * 行格式: t\tseq\tstrength\taffect_v\taffect_a\tlast_access\tuser\treply */
static void sd_save_all_locked(void)
{
    /* ⚠ 必须走 PSRAM: 128KB 全量回写缓冲在内部堆上被 WiFi/BT/音频挤压
     * 后大概率分配失败且静默丢写(板上实测 mem #7 写入后重启 0 episodes,
     * 2026-11-12 验收定位) */
    char *buf = heap_caps_malloc(256 * MEM_MAX_SD, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGW(TAG, "sd save oom");
        return;
    }
    /* snprintf 返回"想要"的长度; 逐条钳位, 放不下即放弃整次回写
     * (RAM 镜像仍是事实源, 半截文件会永久丢尾部记录 —— docs/15 M7) */
    size_t off = 0;
    bool truncated = false;
    for (int i = 0; i < s_count && !truncated; i++) {
        mem_item_t *m = &s_items[i];
        int n = snprintf(buf + off, 256 * MEM_MAX_SD - off,
                         "%lld\t%u\t%.3f\t%d\t%d\t%lld\t%s\t%s\n",
                         (long long)m->t, (unsigned)m->seq, m->strength,
                         m->affect_v, m->affect_a, (long long)m->last_access,
                         m->user, m->reply);
        if (n < 0 || (size_t)n >= 256 * MEM_MAX_SD - off) {
            truncated = true;
            break;
        }
        off += (size_t)n;
    }
    if (truncated) {
        ESP_LOGW(TAG, "sd save truncated, skip write (%d items)", s_count);
        free(buf);
        return;
    }
    esp_err_t e = app_storage_write(s_scope_file, buf, off);
    free(buf);
    /* 台调期留痕: 写盘失败曾是静默丢(2026-11-12 SD 验收 0 episodes 排障) */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sd save fail: %s", esp_err_to_name(e));
    }
}

static void sd_load_all(void)
{
    size_t len = 0;
    char *buf = app_storage_read(s_scope_file, &len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "sd load: read fail/absent");   /* 台调期留痕 */
        return;                                        /* 空库: 全新开始 */
    }
    char *p = buf;
    while (p < buf + len && s_count < s_cap) {   /* 尊重降级容量(docs/15 M5) */
        char *nl = strchr(p, '\n');
        if (nl == NULL) break;
        *nl = 0;
        mem_item_t m;
        long long t, la; unsigned seq; int av, aa; float st;
        if (sscanf(p, "%lld\t%u\t%f\t%d\t%d\t%lld\t",
                   &t, &seq, &st, &av, &aa, &la) == 6) {
            /* P0-1(docs/15): 前 6 字段是纯数字, 第 6 个 \t 之后才是 user;
             * 旧实现从第 1 个 \t 起又跳 6 次, 落在 user/reply 之间 →
             * reply 被当 user, 原 user 丢失且下轮全量回写把它覆盖掉。
             * 现按"恰好 7 个分隔符"严检: 第 7 个之后还有 \t 说明旧内容
             * 含制表符, user/reply 归属无法判定 → 隔离计数, 不猜测。 */
            char *t6 = NULL, *t7 = NULL;
            char *q = p;
            for (int k = 0; k < 7; k++) {
                q = strchr(q, '\t');
                if (q == NULL) break;
                if (k == 5) t6 = q;          /* 第 6 个: user 起点前 */
                if (k == 6) t7 = q;          /* 第 7 个: user 结束处 */
                q++;
            }
            if (t6 != NULL && t7 != NULL && strchr(t7 + 1, '\t') == NULL) {
                memset(&m, 0, sizeof(m));
                m.magic = MEM_MAGIC;
                m.t = t; m.seq = seq; m.strength = st;
                m.affect_v = (int8_t)av; m.affect_a = (int8_t)aa;
                m.last_access = la;
                *t7 = '\0';                  /* user 终止; reply 从 t7+1 到行尾 */
                strlcpy(m.user, t6 + 1, MEM_USR);
                strlcpy(m.reply, t7 + 1, MEM_REP);
                bool deleted = false;         /* T23 台账过滤(防复活) */
                for (int di = 0; di < s_deleted_n; di++) {
                    if (s_deleted[di] == m.seq) { deleted = true; break; }
                }
                if (deleted) continue;
                s_items[s_count++] = m;
                if (m.seq >= s_next_seq) s_next_seq = m.seq + 1;
            } else {
                s_anomalies++;               /* 歧义行: 保留在盘上, 不载入 */
            }
        }
        p = nl + 1;
    }
    free(buf);
    if (s_anomalies > 0) {
        ESP_LOGW(TAG, "sd load: %d anomalous line(s) quarantined",
                 s_anomalies);
    }
}

static void rebuild_block_locked(const char *query, bool allow_persist);

/* SD 后端需要 512 条容量; 60 条小数组则原地扩容(失败保持现容量:
 * SD 持久化仍可用, 只是容量降级 —— 绝不允许 cap 谎报成 512,
 * docs/15 M5 的越界根因) */
static bool s_items_sd_sized;           /* 数组实际按 MEM_MAX_SD 分配 */

static bool mem_resize_for_sd(void)
{
    if (s_items_sd_sized) {              /* 数组已是 512: 只需放开容量 */
        s_cap = MEM_MAX_SD;
        return true;
    }
    mem_item_t *n = heap_caps_malloc(MEM_MAX_SD * sizeof(mem_item_t),
                                     MALLOC_CAP_SPIRAM);
    if (n == NULL) return false;
    if (s_count > 0) memcpy(n, s_items, sizeof(mem_item_t) * s_count);
    free(s_items);
    s_items = n;
    s_items_sd_sized = true;
    s_cap = MEM_MAX_SD;
    return true;
}

/* 持久化只走 SD(T21/A-01: 无 SD 禁记忆, record 门保证 s_sd 必真;
 * 旧 NVS 后端随作用域机制移除 —— 记忆按 persona 隔离后 NVS 单空间
 * 无法表达作用域, 且 docs/16 A-01 裁定缺卡不回退存对话) */
static void persist_item(const mem_item_t *it)
{
    (void)it;
    sd_save_all_locked();
}

static void persist_drop(uint32_t seq)
{
    (void)seq;
    sd_save_all_locked();
}

/* SD 就绪热切换: NVS 条目并入 SD 后端(首次记录时随全量回写迁移),
 * 之后容量升 512 —— soul_start 早于 WiFi, 后端必须可迟到 */
void soul_mem_boot(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return;
    s_items = heap_caps_malloc(MEM_MAX_SD * sizeof(mem_item_t),
                               MALLOC_CAP_SPIRAM);
    if (s_items != NULL) {
        /* 数组按 512 分配, 但 NVS 后端仍按 60 预算用(分区 24KB);
         * s_cap 由后端路径决定, 不把 512 当 NVS 容量 */
        s_items_sd_sized = true;
    } else {                               /* PSRAM 也紧张: 退小容量 */
        ESP_LOGW(TAG, "mem items oom, cap=%d", MEM_MAX);
        s_items = heap_caps_malloc(MEM_MAX * sizeof(mem_item_t),
                                   MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        s_items_sd_sized = false;
    }
    if (s_items != NULL) {
        s_disabled = false;
    } else {
        /* docs/15 M5: 两种容量都拿不到 → 记忆系统整体停用 */
        ESP_LOGW(TAG, "mem disabled (no psram)");
        s_disabled = true;
        snprintf(s_block, sizeof(s_block),
                 "[你们的共同回忆] 你还没有和用户的共同回忆。");
        return;
    }
    /* T21: 不在 boot 加载 —— 等 persona 作用域绑定(soul_mem_scope_bind,
     * 由 act_gate 在 SD 就绪+激活角色确定后调用); 旧全局 mem/episodes.jsonl
     * 为隔离区(T22 迁移), 不读不写 */
    snprintf(s_block, sizeof(s_block),
             "[你们的共同回忆] 你还没有和用户的共同回忆。");
}

/* 相对时间(Kaleido 口径: 今天/昨天/N天前/上周) */
static void rel_time(int64_t t, char *out, size_t cap)
{
    int64_t now = (int64_t)time(NULL);
    int days = (int)((now - t) / 86400);
    if (days <= 0) snprintf(out, cap, "今天");
    else if (days == 1) snprintf(out, cap, "昨天");
    else if (days < 7) snprintf(out, cap, "%d天前", days);
    else snprintf(out, cap, "上周");
}

/* 检索匹配(T21 续; docs/15 M2 修复): UTF-8 码点 bigram 命中率。
 * 旧实现按 3 字节步进+单字节 memchr, 中文 bigram 无法对齐 → 不可用。
 * 码点解码含非法序列回退; 词字符=CJK/字母/数字(跳过空白标点)。 */
static uint32_t utf8_next(const char **p, const char *end)
{
    if (*p >= end) return 0;
    unsigned char c = (unsigned char)**p;
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { (*p)++; return 0xFFFD; }
    if (*p + n > end) { *p = end; return 0xFFFD; }
    for (int k = 1; k < n; k++) {
        unsigned char cc = (unsigned char)(*p)[k];
        if ((cc & 0xC0) != 0x80) { *p += k; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *p += n;
    return cp;
}
static bool cp_is_word(uint32_t cp)
{
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9')) return true;
    return cp >= 0x2E80;                /* CJK 及以上粗略纳入 */
}

#define MATCH_CP_MAX 40
/* 收集词字符码点序列 */
static int cp_seq(const char *s, uint32_t *out, int cap)
{
    const char *p = s, *end = s + strlen(s);
    int n = 0;
    while (p < end && n < cap) {
        uint32_t cp = utf8_next(&p, end);
        if (cp_is_word(cp)) out[n++] = cp;
    }
    return n;
}
static float text_match(const char *query, const char *text)
{
    uint32_t q[MATCH_CP_MAX], t[MATCH_CP_MAX];
    int qn = cp_seq(query, q, MATCH_CP_MAX);
    int tn = cp_seq(text, t, MATCH_CP_MAX);
    if (qn < 2 || tn < 2) return 0;
    int total = qn - 1, hits = 0;
    for (int i = 0; i + 1 < qn; i++) {          /* query bigram */
        for (int j = 0; j + 1 < tn; j++) {
            if (t[j] == q[i] && t[j + 1] == q[i + 1]) { hits++; break; }
        }
    }
    return (float)hits / (float)total;
}
/* 有界追加(P0-2, docs/15): snprintf 返回"想要"的长度, 旧代码直接
 * off += 会越过 s_block, sizeof-off 无符号下溢 → global-buffer-overflow
 * (ASan 合成复现)。集中在此钳位: 截断即停, 并回退 UTF-8 续字节,
 * 保证不劈开多字节字符、恒以 NUL 收尾。 */
static bool s_blk_truncated;
static void blk_append(size_t *off, const char *fmt, ...)
{
    if (*off + 1 >= sizeof(s_block)) {
        s_blk_truncated = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_block + *off, sizeof(s_block) - *off, fmt, ap);
    va_end(ap);
    if (n < 0) {                          /* 编码失败: 保终止符 */
        s_block[*off] = '\0';
        s_blk_truncated = true;
        return;
    }
    size_t need = (size_t)n;
    if (*off + need >= sizeof(s_block)) {
        size_t end = sizeof(s_block) - 1;
        while (end > 0 &&
               ((unsigned char)s_block[end - 1] & 0xC0) == 0x80) {
            end--;                        /* 回退 UTF-8 续字节 */
        }
        s_block[end] = '\0';
        *off = end;
        s_blk_truncated = true;
        return;
    }
    *off += need;
}

static void rebuild_block_locked(const char *query, bool allow_persist)
{
    int64_t now = (int64_t)time(NULL);
    /* top-3: score = 0.5·recency + 0.3·strength + 0.2·match(纯读打分) */
    int idx[3] = { -1, -1, -1 };
    float best[3] = { -1, -1, -1 };
    for (int i = 0; i < s_count; i++) {
        float rec = 1.0f;
        int64_t age = now - s_items[i].t;
        if (age > 7 * 86400) rec = 0.2f;
        else if (age > 3 * 86400) rec = 0.5f;
        else if (age > 86400) rec = 0.8f;
        float sc = 0.5f * rec + 0.3f * s_items[i].strength +
                   0.2f * (query ? text_match(query, s_items[i].user) : 0);
        for (int k = 0; k < 3; k++) {
            if (sc > best[k]) {
                for (int j = 2; j > k; j--) {
                    best[j] = best[j - 1];
                    idx[j] = idx[j - 1];
                }
                best[k] = sc;
                idx[k] = i;
                break;
            }
        }
    }
    if (s_count == 0) {
        snprintf(s_block, sizeof(s_block),
                 "[你们的共同回忆] 你还没有和用户的共同回忆。");
        return;
    }
    s_blk_truncated = false;
    size_t off = 0;
    blk_append(&off,
               "[你们的共同回忆(未必精确, 用\"你记得\"的语气"
               "提及, 不要当作当下事实)] ");
    int put = 0;
    for (int k = 0; k < 3 && idx[k] >= 0 && !s_blk_truncated; k++) {
        mem_item_t *m = &s_items[idx[k]];
        /* reinforce(docs/15 M3 修正): 仅真实检索(query 非空)时强化——
         * 轮末默认重建不再自我强化; 启动加载路径不改状态不写盘 */
        if (allow_persist && query != NULL) {
            if (m->strength < 1.0f) m->strength += 0.125f;
            m->last_access = now;
        }
        char rel[16];
        rel_time(m->t, rel, sizeof(rel));
        blk_append(&off, "%s%s: %s", put++ ? "; " : "", rel, m->user);
        if (!s_blk_truncated && m->reply[0]) {
            blk_append(&off, "(你当时说:%s)", m->reply);
        }
        if (!s_blk_truncated && m->affect_v <= -64) {
            blk_append(&off, "(印象深)");
        }
        if (allow_persist && query != NULL) persist_item(m);
    }
}

void soul_mem_record(const char *user, const char *reply,
                     float valence, float arousal)
{
    /* T21: 未绑定作用域 / 无 SD / 未授权 → 长期记忆不写(docs/16 FR-25;
     * 无 SD 禁 NVS 回退存对话 = 台账 A-01 设计假设) */
    if (s_lock == NULL || s_disabled || !s_scope_ok || !s_consent_mem ||
        !s_sd || user == NULL || user[0] == '\0') {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (s_count >= s_cap) {           /* 淘汰最旧 */
        uint32_t victim = s_items[0].seq;
        memmove(&s_items[0], &s_items[1],
                sizeof(s_items[0]) * (s_count - 1));
        s_count--;
        persist_drop(victim);
    }
    mem_item_t *m = &s_items[s_count];
    memset(m, 0, sizeof(*m));
    m->magic = MEM_MAGIC;
    m->seq = s_next_seq++;
    m->t = (int64_t)time(NULL);
    m->strength = 0.3f;             /* Kaleido 缺省重要度 */
    m->last_access = m->t;
    m->affect_v = (int8_t)(valence * 127.0f);
    m->affect_a = (int8_t)(arousal * 127.0f);
    /* TSV 安全: 净化控制字符(docs/15 P0-1 写侧根治), 防行格式被内容破坏 */
    sanitize_text(m->user, user, MEM_USR);
    sanitize_text(m->reply, reply ? reply : "", MEM_REP);
    if (m->user[0] == '\0') {         /* 全控制字符: 不留空记录 */
        xSemaphoreGive(s_lock);
        return;
    }
    s_count++;
    persist_item(m);
    rebuild_block_locked(NULL, false);    /* 轮末默认块: 不强化(docs/15 M3) */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mem #%u: len=%zu", (unsigned)m->seq,
             strlen(m->user));  /* NFR-03: 原文不入常规日志 */
}

const char *soul_mem_prompt_block(void)
{
    /* 会话任务上下文调用: 返回当前块(轮末已重建; query 精排 v2 再做) */
    return s_block;
}


/* 按当前 query 检索精排(T21 续): 会话收到用户文本后调用——top-3 按
 * 码点 bigram 命中重排并强化命中项(docs/15 M2/M3 修复); query 空则
 * 等价默认重建。 */
void soul_mem_query_rebuild(const char *query)
{
    if (s_lock == NULL || s_disabled || query == NULL || query[0] == '\0') {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_scope_ok) rebuild_block_locked(query, true);
    xSemaphoreGive(s_lock);
}

/* ---- persona 作用域绑定(T21; docs/17 §7.2/§8.1) ----
 * key = persona_id 十六进制(数据域锚点); 切换 = 清 RAM → 从该角色文件
 * 加载(无文件=空库); consent_mem=false 时长期记忆不写(可读已有)。
 * 旧全局 mem/episodes.jsonl 为隔离区(T22 迁移), 本机制不触碰。 */
void soul_mem_scope_bind(const char *persona_key, bool consent_mem)
{
    if (s_lock == NULL || s_disabled || persona_key == NULL ||
        persona_key[0] == '\0') {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_consent_mem = consent_mem;
    snprintf(s_scope_file, sizeof(s_scope_file),
             "v1/personas/%s/memory/episodes.tsv", persona_key);
    snprintf(s_del_log_file, sizeof(s_del_log_file),
             "v1/personas/%s/memory/deleted.log", persona_key);
    s_count = 0;                          /* 清旧作用域 RAM 镜像 */
    s_next_seq = 1;
    s_anomalies = 0;
    s_deleted_n = 0;
    s_sd = false;
    /* 删除台账载入(独立于 episodes; 读失败仅按空台账, 防复活靠重写过滤) */
    size_t dlen = 0;
    char *dlog = app_storage_read(s_del_log_file, &dlen);
    if (dlog != NULL) {
        char *dp = dlog;
        while (dp < dlog + dlen && s_deleted_n < MEM_DEL_MAX) {
            char *nl = strchr(dp, '\n');
            if (nl == NULL) break;
            *nl = 0;
            unsigned long v = 0;
            if (sscanf(dp, "%lu", &v) == 1 && v != 0) {
                s_deleted[s_deleted_n++] = (uint32_t)v;
            }
            dp = nl + 1;
        }
        free(dlog);
    }
    if (app_storage_ok()) {
        if (!mem_resize_for_sd()) {
            ESP_LOGW(TAG, "mem scope: degraded cap=%d (oom)", s_cap);
        }
        s_sd = true;
        sd_load_all();                    /* 该角色的既有记忆 */
    }
    s_scope_ok = true;
    rebuild_block_locked(NULL, false);    /* 绑定即可用(不写盘) */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mem scope -> %s (%d episode(s), consent=%d, cap %d)",
             s_scope_file, s_count, s_consent_mem, s_cap);
}

/* ---- 删除(T23; docs/17 §7.2/§8.2): 台账先行, 防复活 ----
 * 返回: OK=持久接受(内存已删+台账落盘+重写); PENDING=台账写失败
 * (内存已隐藏且保持隐藏, 但未持久接受 —— 重试归上层); NOT_FOUND。 */
#define MEM_DEL_PENDING_MAX 8
static uint32_t s_pending_del[MEM_DEL_PENDING_MAX];
static int s_pending_del_n;

/* 台账追加(内部); 成功则入 s_deleted */
static bool del_ledger_append(uint32_t seq)
{
    if (s_deleted_n >= MEM_DEL_MAX) return false;
    char line[24];
    snprintf(line, sizeof(line), "%lu", (unsigned long)seq);
    if (app_storage_append(s_del_log_file, line) != ESP_OK) return false;
    s_deleted[s_deleted_n++] = seq;
    return true;
}

soul_mem_del_res_t soul_mem_delete(uint32_t seq)
{
    if (s_lock == NULL || s_disabled || !s_scope_ok) {
        return SOUL_MEM_DEL_REJECTED;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return SOUL_MEM_DEL_REJECTED;
    }
    /* 先补此前 PENDING 的台账(重试语义) */
    for (int i = 0; i < s_pending_del_n; ) {
        if (del_ledger_append(s_pending_del[i])) {
            s_pending_del[i] = s_pending_del[--s_pending_del_n];
            continue;
        }
        i++;
    }
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].seq == seq) { idx = i; break; }
    }
    bool in_ledger = false;
    for (int i = 0; i < s_deleted_n; i++) {
        if (s_deleted[i] == seq) { in_ledger = true; break; }
    }
    bool in_pending = false;
    for (int i = 0; i < s_pending_del_n; i++) {
        if (s_pending_del[i] == seq) { in_pending = true; break; }
    }
    if (idx < 0 && !in_ledger && !in_pending) {
        xSemaphoreGive(s_lock);
        return SOUL_MEM_DEL_NOT_FOUND;
    }
    /* 1) RAM 立即隐藏(无论台账是否落盘 —— docs/17: 先失效再持久) */
    if (idx >= 0) {
        memmove(&s_items[idx], &s_items[idx + 1],
                sizeof(s_items[0]) * (size_t)(s_count - idx - 1));
        s_count--;
        rebuild_block_locked(NULL, false);
    }
    /* 2) 台账追加(落盘后删除才算持久接受; 已在台账则幂等成功);
     * 失败 → PENDING: seq 入待持久队列, RAM 保持隐藏, 重试补台账 */
    soul_mem_del_res_t res = SOUL_MEM_DEL_OK;
    if (!in_ledger) {
        if (!del_ledger_append(seq) && !in_pending) {
            res = SOUL_MEM_DEL_PENDING;
            if (s_pending_del_n < MEM_DEL_PENDING_MAX) {
                s_pending_del[s_pending_del_n++] = seq;
            }
        } else if (in_ledger || in_pending) {
            res = SOUL_MEM_DEL_OK;
        }
    }
    if (in_ledger) res = SOUL_MEM_DEL_OK;
    /* 3) 持久接受后重写 episodes(当前有效集) */
    if (res == SOUL_MEM_DEL_OK && s_sd) {
        sd_save_all_locked();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mem delete #%u -> %s", (unsigned)seq,
             res == SOUL_MEM_DEL_OK ? "ok" :
             res == SOUL_MEM_DEL_PENDING ? "pending" : "rejected");
    return res;
}

#endif /* SOUL */
