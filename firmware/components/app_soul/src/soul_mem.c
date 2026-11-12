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
#define MEM_MAX     60
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

static mem_item_t s_items[MEM_MAX];
static int s_count;
static SemaphoreHandle_t s_lock;
static uint32_t s_next_seq = 1;
static char s_block[MEM_BLOCK];   /* prompt 块(重建于写入/启动) */

static void mem_key(char *k, size_t cap, uint32_t seq)
{
    snprintf(k, cap, MEM_KEY_PFX "%08lx", (unsigned long)seq);
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

void soul_mem_boot(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return;
    nvs_handle_t h;
    if (nvs_open(MEM_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t ns = 0;
        if (nvs_get_u32(h, MEM_KEY_NXT, &ns) == ESP_OK) s_next_seq = ns;
        nvs_close(h);
    }
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(MEM_NS, MEM_KEY_PFX, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it != NULL && s_count < MEM_MAX) {
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
    ESP_LOGI(TAG, "mem boot: %d episode(s)", s_count);
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
        nvs_save(m);                /* reinforce 持久化(小blob重写) */
    }
}

void soul_mem_record(const char *user, const char *reply,
                     float valence, float arousal)
{
    if (s_lock == NULL || user == NULL || user[0] == '\0') return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (s_count >= MEM_MAX) {       /* 淘汰最旧 */
        uint32_t victim = s_items[0].seq;
        memmove(&s_items[0], &s_items[1],
                sizeof(s_items[0]) * (s_count - 1));
        s_count--;
        nvs_drop(victim);
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
    nvs_save(m);
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
