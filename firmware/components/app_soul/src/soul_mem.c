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
#include <stdio.h>
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
static int s_cap = MEM_MAX;             /* 运行时后端容量 */
static bool s_sd;                       /* SD 后端在位 */
static int s_count;
static SemaphoreHandle_t s_lock;
static uint32_t s_next_seq = 1;
static char s_block[MEM_BLOCK];   /* prompt 块(重建于写入/启动) */

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
    size_t off = 0;
    for (int i = 0; i < s_count && off < (256 * MEM_MAX_SD) - 320; i++) {
        mem_item_t *m = &s_items[i];
        off += snprintf(buf + off, 256 * MEM_MAX_SD - off,
                        "%lld\t%u\t%.3f\t%d\t%d\t%lld\t%s\t%s\n",
                        (long long)m->t, (unsigned)m->seq, m->strength,
                        m->affect_v, m->affect_a, (long long)m->last_access,
                        m->user, m->reply);
    }
    esp_err_t e = app_storage_write(MEM_FILE, buf, off);
    free(buf);
    /* 台调期留痕: 写盘失败曾是静默丢(2026-11-12 SD 验收 0 episodes 排障) */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sd save fail: %s", esp_err_to_name(e));
    }
}

static void sd_load_all(void)
{
    size_t len = 0;
    char *buf = app_storage_read(MEM_FILE, &len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "sd load: read fail/absent");   /* 台调期留痕 */
        return;                                        /* 空库: 全新开始 */
    }
    char *p = buf;
    while (p < buf + len && s_count < MEM_MAX_SD) {
        char *nl = strchr(p, '\n');
        if (nl == NULL) break;
        *nl = 0;
        mem_item_t m;
        long long t, la; unsigned seq; int av, aa; float st;
        if (sscanf(p, "%lld\t%u\t%f\t%d\t%d\t%lld\t",
                   &t, &seq, &st, &av, &aa, &la) == 6) {
            char *u = strchr(p, '\t');       /* 第7字段起手工切(user 不含 \t) */
            for (int i = 0; i < 6 && u; i++) u = strchr(u + 1, '\t');
            if (u != NULL) {
                memset(&m, 0, sizeof(m));
                m.magic = MEM_MAGIC;
                m.t = t; m.seq = seq; m.strength = st;
                m.affect_v = (int8_t)av; m.affect_a = (int8_t)aa;
                m.last_access = la;
                char *r = strchr(u + 1, '\t');
                if (r != NULL) {
                    *r = 0;
                    strlcpy(m.reply, r + 1, MEM_REP);
                }
                strlcpy(m.user, u + 1, MEM_USR);
                s_items[s_count++] = m;
                if (m.seq >= s_next_seq) s_next_seq = m.seq + 1;
            }
        }
        p = nl + 1;
    }
    free(buf);
}

static void mem_key(char *k, size_t cap, uint32_t seq)
{
    snprintf(k, cap, MEM_KEY_PFX "%08lx", (unsigned long)seq);
}

static void nvs_save(const mem_item_t *it);   /* 前向(NVS 后端) */
static void nvs_drop(uint32_t seq);

static void persist_item(const mem_item_t *it)
{
    if (s_sd) { sd_save_all_locked(); return; }
    nvs_save(it);
}

static void persist_drop(uint32_t seq)
{
    if (s_sd) { sd_save_all_locked(); return; }
    nvs_drop(seq);
}

static void nvs_save(const mem_item_t *it)
{
    nvs_handle_t h;
    if (nvs_open(MEM_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    mem_key(k, sizeof(k), it->seq);
    nvs_set_blob(h, k, it, sizeof(*it));
    nvs_set_u32(h, MEM_KEY_NXT, s_next_seq);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_drop(uint32_t seq)
{
    nvs_handle_t h;
    if (nvs_open(MEM_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    mem_key(k, sizeof(k), seq);
    nvs_erase_key(h, k);
    nvs_commit(h);
    nvs_close(h);
}

/* SD 就绪热切换: NVS 条目并入 SD 后端(首次记录时随全量回写迁移),
 * 之后容量升 512 —— soul_start 早于 WiFi, 后端必须可迟到 */
static void mem_sd_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                          void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id != CUBE_EVT_SD_READY || s_sd || s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_sd = true;
    s_cap = MEM_MAX_SD;
    sd_load_all();                       /* SD 既有条目追加进镜像 */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mem backend -> sd (%d episodes)", s_count);
}

void soul_mem_boot(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return;
    s_items = heap_caps_malloc(MEM_MAX_SD * sizeof(mem_item_t),
                               MALLOC_CAP_SPIRAM);
    if (s_items == NULL) {                 /* PSRAM 也紧张: 退 NVS 小容量 */
        ESP_LOGW(TAG, "mem items oom, cap=%d", MEM_MAX);
    }
    if (s_items == NULL) {
        s_items = heap_caps_malloc(MEM_MAX * sizeof(mem_item_t),
                                   MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
    if (app_storage_ok()) {                 /* SD 后端优先(M3+ 强制底座) */
        s_sd = true;
        s_cap = MEM_MAX_SD;
        sd_load_all();
        ESP_LOGI(TAG, "mem boot(sd): %d episode(s)", s_count);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(MEM_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t ns = 0;
        if (nvs_get_u32(h, MEM_KEY_NXT, &ns) == ESP_OK) s_next_seq = ns;
        nvs_close(h);
    }
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(MEM_NS, MEM_KEY_PFX, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it != NULL && s_count < s_cap) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        nvs_handle_t rh;
        if (nvs_open(MEM_NS, NVS_READONLY, &rh) == ESP_OK) {
            mem_item_t m;
            size_t len = sizeof(m);
            if (nvs_get_blob(rh, info.key, &m, &len) == ESP_OK &&
                len == sizeof(m) && m.magic == MEM_MAGIC) {
                s_items[s_count++] = m;
                if (m.seq >= s_next_seq) s_next_seq = m.seq + 1;
            }
            nvs_close(rh);
        }
        err = nvs_entry_next(&it);
    }
    /* 时间升序(旧在前, 淘汰头) */
    for (int i = 1; i < s_count; i++) {
        mem_item_t k = s_items[i];
        int j = i - 1;
        while (j >= 0 && s_items[j].t > k.t) {
            s_items[j + 1] = s_items[j];
            j--;
        }
        s_items[j + 1] = k;
    }
    ESP_LOGI(TAG, "mem boot(nvs): %d episode(s)", s_count);
    cube_event_subscribe(mem_sd_evt_cb, NULL);
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

/* 2-gram 匹配分(UTF-8 字节对; 简单但中英都能吃) */
static float text_match(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la < 6 || lb < 6) return 0;
    int hits = 0, total = 0;
    for (size_t i = 0; i + 1 < la; i += 3) {     /* 中文按 3 字节步进 */
        char g[3] = { a[i], a[i + 1], 0 };
        if (a[i] & 0x80) g[1] = a[i + 1];
        if (g[0] == 0) break;
        total++;
        if (memchr(b, g[0], lb) != NULL) hits++;
    }
    return total ? (float)hits / total : 0;
}

static void rebuild_block_locked(const char *query)
{
    int64_t now = (int64_t)time(NULL);
    /* top-3: score = 0.5·recency + 0.3·strength + 0.2·match */
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
    size_t off = snprintf(s_block, sizeof(s_block),
                          "[你们的共同回忆(未必精确, 用\"你记得\"的语气"
                          "提及, 不要当作当下事实)] ");
    int put = 0;
    for (int k = 0; k < 3 && idx[k] >= 0 && off < sizeof(s_block) - 96;
         k++) {
        mem_item_t *m = &s_items[idx[k]];
        /* reinforce + 巩固时间(越回忆越牢) */
        if (m->strength < 1.0f) m->strength += 0.125f;
        m->last_access = now;
        char rel[16];
        rel_time(m->t, rel, sizeof(rel));
        off += snprintf(s_block + off, sizeof(s_block) - off,
                        "%s%s: %s", put++ ? "; " : "", rel, m->user);
        if (m->reply[0]) {
            off += snprintf(s_block + off, sizeof(s_block) - off,
                            "(你当时说:%s)", m->reply);
        }
        if (m->affect_v <= -64) {
            off += snprintf(s_block + off, sizeof(s_block) - off,
                            "(印象深)");
        }
        persist_item(m);            /* reinforce 持久化 */
    }
}

void soul_mem_record(const char *user, const char *reply,
                     float valence, float arousal)
{
    if (s_lock == NULL || user == NULL || user[0] == '\0') return;
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
    strlcpy(m->user, user, MEM_USR);
    strlcpy(m->reply, reply ? reply : "", MEM_REP);
    s_count++;
    persist_item(m);
    rebuild_block_locked(NULL);     /* 无 query 的默认块 */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mem #%u: %s", (unsigned)m->seq, m->user);
}

const char *soul_mem_prompt_block(void)
{
    /* 会话任务上下文调用: 返回当前块(轮末已重建; query 精排 v2 再做) */
    return s_block;
}

#endif /* SOUL */
