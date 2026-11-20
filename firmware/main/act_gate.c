/*
 * act_gate.c —— cube_act 统一动作门装配(L5; docs/17 §15.4, docs/18 T13)
 *
 * 职责: 注册首批能力(radio.play/music.stop/audio.volume/face.show) +
 * 递归锁 port + 默认上下文初始化。执行器保持既有 SOUL_ACT 下游链
 * (app_player/ui_port 订阅)不变 —— 本文件只换"决策→执行"的关口。
 *
 * 回执语义(首段, docs/17 §5.2): started=已交付执行器(受理级),
 * 不是真实出声确认; 播放结束/被替换的终态由取消与同能力替换给出,
 * 真实 started(T35)接 PLAYER_STATE 后收口。用户停止经 cact_user_stop
 * (组1 锁存), 在途 radio.play 被取消并发布 stop(幂等)。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"    /* xTaskCreateWithCaps(psnsd PSRAM 栈, docs/22 §3B) */
#include "esp_log.h"

#include "cube_act.h"
#include "nvs.h"
#include "cube_event_bus.h"
#include "audio_mixer.h"
#include "app_player.h"
#include "soul.h"
#include "persona.h"
#include "app_storage.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "act_gate.h"

static const char *TAG = "actgate";

static SemaphoreHandle_t s_lock;

static void lock_enter(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}
static void lock_exit(void)
{
    xSemaphoreGiveRecursive(s_lock);
}
static const cact_lock_port_t s_lock_port = { lock_enter, lock_exit };

/* ---- 执行器: 交付=发布 SOUL_ACT 或直调播放器受理 ---- */
static void act_publish(uint8_t kind, const char *content)
{
    cube_soul_act_t a = { .kind = kind };
    strlcpy(a.content, content ? content : "", sizeof(a.content));
    cube_event_publish(CUBE_EVT_SOUL_ACT, &a, sizeof(a));
}

static cact_res_t v_none(const cact_desc_t *d, const uint8_t *p, size_t n)
{
    (void)d;
    return n == 0 ? CACT_RES_OK : CACT_RES_INVALID;
}

/* radio.play 参数: "station\0<idx>\0"(可省, idx 为 0 起电台下标) */
static bool vol_station(const uint8_t *p, size_t n, int *idx)
{
    *idx = -1;
    if (n == 0) return true;                   /* 无参: 电台轮播 */
    if (n != 10) return false;                 /* "station"(7+NUL)+idx+NUL */
    if (memcmp(p, "station", 8) != 0) return false;
    const char *v = (const char *)p + 8;
    if (v[0] < '0' || v[0] > '9') return false;
    *idx = v[0] - '0';                          /* 台表 ≤9 个, 单数字 */
    return true;
}
static cact_res_t v_radio_play(const cact_desc_t *d, const uint8_t *p,
                               size_t n)
{
    (void)d;
    int idx = -1;
    if (!vol_station(p, n, &idx)) return CACT_RES_INVALID;
    if (idx >= 0 && idx >= app_player_station_num()) return CACT_RES_INVALID;
    return CACT_RES_OK;
}
static cact_res_t ex_radio_play(const cact_desc_t *d, uint32_t id,
                                const uint8_t *p, size_t n)
{
    (void)d;
    int idx = -1;
    (void)vol_station(p, n, &idx);
    esp_err_t e = ESP_OK;
    if (idx >= 0) {
        e = app_player_play_index(idx);
    } else {
        act_publish(3, "play");                 /* 既有轮播链路 */
    }
    if (e != ESP_OK) {
        cact_report(cact_default(), id, CACT_ST_FAILED, "player_reject");
        return CACT_RES_OK;                     /* 已按失败回执收口 */
    }
    cact_report(cact_default(), id, CACT_ST_STARTED, "dispatched");
    /* T34 信号源: 用户显式播放 = 正向(自主播放曝光不计——曝光不证明喜欢) */
    {
        cact_receipt_t rc;
        if (cact_receipt_last(cact_default(), id, &rc) == CACT_RES_OK) {
            uint32_t pid = persona_active_id(act_gate_persona());
            if (pid != 0 &&
                (rc.source == CACT_ORIGIN_PHYSICAL_USER ||
                 rc.source == CACT_ORIGIN_VOICE_UNVERIFIED)) {
                char ref[16];
                snprintf(ref, sizeof(ref), "play:%lu", (unsigned long)id);
                persona_evo_signal(act_gate_persona(), pid,
                                   PSN_DIM_INTEREST, 1, ref);
            }
        }
    }
    return CACT_RES_OK;
}
static void ex_radio_play_cancel(const cact_desc_t *d, uint32_t id)
{
    (void)d;
    act_publish(3, "stop");                     /* 幂等保证底层停 */
    cact_report(cact_default(), id, CACT_ST_CANCELLED, "cancelled");
}

static cact_res_t v_stop(const cact_desc_t *d, const uint8_t *p, size_t n)
{
    return v_none(d, p, n);
}
static cact_res_t ex_music_stop(const cact_desc_t *d, uint32_t id,
                                const uint8_t *p, size_t n)
{
    (void)d; (void)p; (void)n;
    esp_err_t e = app_player_stop();
    if (e != ESP_OK) {
        cact_report(cact_default(), id, CACT_ST_FAILED, "player_reject");
        return CACT_RES_OK;
    }
    /* 瞬时动作: 停播指令交付即完成(不占队列, T13 板上暴露的队头阻塞) */
    cact_report(cact_default(), id, CACT_ST_COMPLETED, "delivered");
    /* T34 信号源: 用户类 stop = 对当前音乐负反馈(聚合达阈值才演化) */
    {
        cact_receipt_t rc;
        if (cact_receipt_last(cact_default(), id, &rc) == CACT_RES_OK) {
            uint32_t pid = persona_active_id(act_gate_persona());
            if (pid != 0 &&
                (rc.source == CACT_ORIGIN_PHYSICAL_USER ||
                 rc.source == CACT_ORIGIN_VOICE_UNVERIFIED)) {
                char ref[16];
                snprintf(ref, sizeof(ref), "stop:%lu", (unsigned long)id);
                persona_evo_signal(act_gate_persona(), pid,
                                   PSN_DIM_INTEREST, -1, ref);
            }
        }
    }
    return CACT_RES_OK;
}

/* audio.volume 参数: "v\0<0..100>\0" */
static cact_res_t v_volume(const cact_desc_t *d, const uint8_t *p, size_t n)
{
    (void)d;
    if (n != 1 + 1 + 3 + 1 && n != 1 + 1 + 2 + 1 && n != 1 + 1 + 1 + 1) {
        return CACT_RES_INVALID;
    }
    if (p[0] != 'v' || p[1] != '\0' || p[n - 1] != '\0') {
        return CACT_RES_INVALID;
    }
    int v = 0;
    for (size_t i = 2; i + 1 < n; i++) {
        if (p[i] < '0' || p[i] > '9') return CACT_RES_INVALID;
        v = v * 10 + (p[i] - '0');
    }
    return (v >= 0 && v <= 100) ? CACT_RES_OK : CACT_RES_INVALID;
}
static cact_res_t ex_volume(const cact_desc_t *d, uint32_t id,
                            const uint8_t *p, size_t n)
{
    (void)d;
    if (v_volume(d, p, n) != CACT_RES_OK) return CACT_RES_INVALID;
    int v = 0;
    for (size_t i = 2; i + 1 < n; i++) v = v * 10 + (p[i] - '0');
    audio_mixer_set_master_volume(v);           /* 内部发音量事件(UI 同步) */
    cact_report(cact_default(), id, CACT_ST_COMPLETED, "delivered");
    return CACT_RES_OK;
}

/* face.show 参数: 素材段名(可打印 ASCII, NUL 结尾) */
static cact_res_t v_face(const cact_desc_t *d, const uint8_t *p, size_t n)
{
    (void)d;
    if (n < 2 || p[n - 1] != '\0' || n > 32) return CACT_RES_INVALID;
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] < 0x20 || p[i] > 0x7e) return CACT_RES_INVALID;
    }
    return CACT_RES_OK;
}
static cact_res_t ex_face_show(const cact_desc_t *d, uint32_t id,
                               const uint8_t *p, size_t n)
{
    (void)d;
    if (v_face(d, p, n) != CACT_RES_OK) return CACT_RES_INVALID;
    act_publish(1, (const char *)p);
    cact_report(cact_default(), id, CACT_ST_COMPLETED, "delivered");
    return CACT_RES_OK;
}

static cact_desc_t s_caps[] = {
    { .cap_id = "radio.play",  .group_id = 1, .category = CACT_CAT_AUDIO,
      .risk = CACT_RISK_LOW, .supports_cancel = true,
      .validate = v_radio_play, .execute = ex_radio_play,
      .cancel = ex_radio_play_cancel },
    { .cap_id = "music.stop",  .group_id = 1, .category = CACT_CAT_AUDIO,
      .risk = CACT_RISK_LOW, .supports_cancel = false,
      .validate = v_stop, .execute = ex_music_stop, .cancel = NULL },
    { .cap_id = "audio.volume", .group_id = 0, .category = CACT_CAT_GENERIC,
      .risk = CACT_RISK_LOW, .supports_cancel = false,
      .validate = v_volume, .execute = ex_volume, .cancel = NULL },
    { .cap_id = "face.show",   .group_id = 0, .category = CACT_CAT_FACE,
      .risk = CACT_RISK_LOW, .supports_cancel = true,
      .validate = v_face, .execute = ex_face_show,
      .cancel = NULL },
};

/* ---- persona 装配(T20): 双槽持久化 + 默认角色 + epoch 注入 ---- */
static persona_svc_t s_psn_svc;
static uint8_t *s_psn_scratch;            /* PSRAM 4KB(store_slot 组装区) */
static uint8_t *s_psn_io_buf;             /* PSRAM 4KB(write_at 读改写) */

static uint64_t psn_rng(void)
{
    return ((uint64_t)esp_random() << 32) | esp_random();
}
static void psn_rel(char *rel, size_t cap, int slot)
{
    snprintf(rel, cap, "v1/personas/registry.%c", slot == STORE_SLOT_B ? 'b' : 'a');
}
static int psn_io_read(void *ud, int slot, void *buf, size_t cap, size_t *len)
{
    (void)ud;
    char rel[48];
    psn_rel(rel, sizeof(rel), slot);
    size_t n = 0;
    char *d = app_storage_read(rel, &n);
    if (d == NULL) return 2;
    size_t k = n < cap ? n : cap;
    memcpy(buf, d, k);
    free(d);
    *len = k;
    return 0;
}
static int psn_io_write_full(void *ud, int slot, const void *buf, size_t len)
{
    (void)ud;
    char rel[48];
    psn_rel(rel, sizeof(rel), slot);
    return app_storage_write(rel, buf, len) == ESP_OK ? 0 : 1;
}
static int psn_io_write_at(void *ud, int slot, size_t off, const void *buf,
                           size_t len)
{
    (void)ud;
    char rel[48];
    psn_rel(rel, sizeof(rel), slot);
    size_t n = 0;
    char *d = app_storage_read(rel, &n);
    if (d == NULL || n > 4096) { free(d); return 1; }
    memcpy(s_psn_io_buf, d, n);
    free(d);
    if (off + len > n) return 1;
    memcpy(s_psn_io_buf + off, buf, len);
    return app_storage_write(rel, s_psn_io_buf, n) == ESP_OK ? 0 : 1;
}
static const store_io_t s_psn_io = {
    psn_io_read, psn_io_write_full, psn_io_write_at, NULL,
};

static void psn_epoch_inject(void)
{
    cact_set_persona_epoch(cact_default(), persona_epoch(&s_psn_svc));
}

/* 记忆作用域绑定(T21): 当前激活角色 + 其记忆授权 → soul_mem */
static void psn_mem_scope_apply(void)
{
    uint32_t id = persona_active_id(&s_psn_svc);
    if (id == 0) {
        ESP_LOGW(TAG, "mem scope: no active persona (memory off)");
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "%08lx", (unsigned long)id);
#if CONFIG_ESP_CUBE_ENABLE_SOUL
    soul_mem_scope_bind(key, persona_consent_memory(&s_psn_svc, id));
#endif
}

/* trait 慢变量注入(S2.5b/B-5): 激活角色卡上的 trait → soul(无则中性) */
static void psn_trait_apply(void)
{
#if CONFIG_ESP_CUBE_ENABLE_SOUL
    uint32_t id = persona_active_id(&s_psn_svc);
    if (id == 0) return;
    float tr[PSN_TRAIT_MAX];
    if (persona_trait_read(&s_psn_svc, id, tr)) {
        soul_trait_load(tr);
        ESP_LOGI(TAG, "trait from persona #%08lx", (unsigned long)id);
    }
#endif
}

persona_svc_t *act_gate_persona(void) { return &s_psn_svc; }

/* 清除全部角色资料(T22 用户裁决: 测试阶段资料不保留)——注册表/角色记忆/
 * 旧全局 episodes 一并移除, 然后重建全新默认角色。不可逆(已获用户授权)。 */
void act_gate_persona_clear(void)
{
    int removed = 0;
    char rel[96], path[128];
    for (int i = 0; i < s_psn_svc.n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%08lx",
                 (unsigned long)s_psn_svc.slot[i].persona_id);
        snprintf(rel, sizeof(rel),
                 "v1/personas/%s/memory/episodes.tsv", key);
        app_storage_path(path, sizeof(path), rel);
        if (remove(path) == 0) removed++;
        snprintf(rel, sizeof(rel), "v1/personas/%s/memory/deleted.log", key);
        app_storage_path(path, sizeof(path), rel);
        if (remove(path) == 0) removed++;
    }
    const char *regs[] = { "v1/personas/registry.a",
                           "v1/personas/registry.b",
                           "mem/episodes.jsonl" };   /* 旧全局记忆(用户裁决) */
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        app_storage_path(path, sizeof(path), regs[i]);
        if (remove(path) == 0) removed++;
    }
    ESP_LOGW(TAG, "persona clear: %d file(s) removed (user authorized)",
             removed);
    /* 重置并重建全新默认角色(测试阶段; App 可再改) */
    persona_init(&s_psn_svc, psn_rng);
    /* 8224 与 persona_sd_try 一致(store_slot 要求 ≥max_payload 8000+24;
     * 批次 44 修复: 旧值 4096 使 psnclear 后 bind 必 INVALID → persona
     * 持久化静默失效) */
    (void)persona_store_bind(&s_psn_svc, &s_psn_io, s_psn_scratch, 8224);
    (void)persona_load(&s_psn_svc);
    uint32_t id = 0;
    persona_base_t f;
    memset(&f, 0, sizeof(f));
    strlcpy(f.display_name, "小灵", sizeof(f.display_name));
    strlcpy(f.default_voice, "longanrou_v3", sizeof(f.default_voice));
    if (persona_create_draft(&s_psn_svc, "小灵", &id) == PSN_RES_OK &&
        persona_edit_draft(&s_psn_svc, id, &f) == PSN_RES_OK &&
        persona_confirm(&s_psn_svc, id) == PSN_RES_OK &&
        persona_activate_begin(&s_psn_svc, id, 0) == PSN_RES_OK &&
        persona_activate_commit(&s_psn_svc) == PSN_RES_OK &&
        persona_save(&s_psn_svc) == PSN_RES_OK) {
        ESP_LOGI(TAG, "persona fresh default: #%08lx", (unsigned long)id);
    } else {
        ESP_LOGW(TAG, "persona fresh default create failed");
    }
    psn_epoch_inject();
    psn_mem_scope_apply();
    psn_trait_apply();
}

/* trait 演化落盘(S2.5b/B-5): soul 日界 EMA 步生效 → persona 卡 v3 */
static void psn_trait_evo_cb(cube_evt_id_t id, const void *data, size_t len,
                             void *ctx)
{
    (void)ctx;
    if (id != CUBE_EVT_TRAIT_EVO || data == NULL || len < sizeof(cube_trait_evo_t))
        return;
    uint32_t id_act = persona_active_id(&s_psn_svc);
    if (id_act == 0) return;
    const cube_trait_evo_t *evo = (const cube_trait_evo_t *)data;
    if (persona_trait_write(&s_psn_svc, id_act, evo->trait) != PSN_RES_OK)
        return;
    if (persona_save(&s_psn_svc) != PSN_RES_OK) {
        ESP_LOGW(TAG, "trait evo save fail (kept in memory)");
    } else {
        ESP_LOGI(TAG, "trait evo persisted #%08lx", (unsigned long)id_act);
    }
}

static bool s_psn_bound;                 /* SD 就绪后 bind+load 一次 */

/* SD 就绪后调用(T20): SD 迟挂载(actgate 初始化在挂载前) —— bind+load
 * 必须等介质就绪, 否则空库误判新建默认角色(docs/17 §8.2 迟挂载语义) */
static void persona_sd_try(void)
{
    if (s_psn_bound || s_psn_scratch == NULL) return;
    if (!app_storage_ok()) return;
    s_psn_bound = true;
    psn_res_t r = persona_store_bind(&s_psn_svc, &s_psn_io, s_psn_scratch,
                                     8224);
    if (r != PSN_RES_OK) {
        ESP_LOGW(TAG, "persona bind fail: %d", r);
        return;
    }
    r = persona_load(&s_psn_svc);
    if (r != PSN_RES_OK) {
        /* RECOVERY_REQUIRED: 停写保护, 无角色运行; 恢复流程归 T23 */
        ESP_LOGW(TAG, "persona load degraded: %d (store protected)", r);
        psn_epoch_inject();
        return;
    }
    if (s_psn_svc.n == 0) {
        /* 全新库: 创建预设默认角色(docs/16 FR-22 预设; 设计假设:
         * 默认"小灵"/longanrou_v3, 用户可经 App 改名/编辑) */
        uint32_t id = 0;
        persona_base_t f;
        memset(&f, 0, sizeof(f));
        strlcpy(f.display_name, "小灵", sizeof(f.display_name));
        strlcpy(f.default_voice, "longanrou_v3", sizeof(f.default_voice));
        if (persona_create_draft(&s_psn_svc, "小灵", &id) == PSN_RES_OK &&
            persona_edit_draft(&s_psn_svc, id, &f) == PSN_RES_OK &&
            persona_confirm(&s_psn_svc, id) == PSN_RES_OK &&
            persona_activate_begin(&s_psn_svc, id, 0) == PSN_RES_OK &&
            persona_activate_commit(&s_psn_svc) == PSN_RES_OK &&
            persona_save(&s_psn_svc) == PSN_RES_OK) {
            ESP_LOGI(TAG, "persona default created: #%08lx 小灵",
                     (unsigned long)id);
        } else {
            ESP_LOGW(TAG, "persona default create failed");
        }
    } else {
        /* epoch 已推进到持久值+1; 落盘新基线(防旧值复活) */
        (void)persona_save(&s_psn_svc);
    }
    psn_epoch_inject();
    psn_mem_scope_apply();
    psn_trait_apply();
    ESP_LOGI(TAG, "persona: %d role(s), active #%08lx epoch %u",
             s_psn_svc.n,
             (unsigned long)persona_active_id(&s_psn_svc),
             persona_epoch(&s_psn_svc));
}

/* SD_READY 回调只投递 —— mount 任务上下文里直接做 SD 文件操作会与
 * SPI 驱动 polling 事务递归冲突(板上 T20 实证 Load access fault)。
 * 重活在一次性任务执行(与 player 订阅同款模式, docs/14 §7)。 */
static void persona_sd_task(void *arg)
{
    (void)arg;
    persona_sd_try();
    vTaskDeleteWithCaps(NULL);    /* 栈在 PSRAM(WithCaps 须配对删除) */
}

static void persona_sd_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                              void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id != CUBE_EVT_SD_READY) return;
    if (s_psn_bound || s_psn_scratch == NULL) return;
    /* 4KB 瞬态栈走 PSRAM(docs/22 §3B): 纯 SD 文件操作(scratch 本就
     * PSRAM), 不占内部预算 */
    if (xTaskCreateWithCaps(persona_sd_task, "psnsd", 4096, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdTRUE) {
        ESP_LOGW(TAG, "persona sd task spawn fail");
    }
}

/* T38: cact 设置持久化(NVS "cact" 区; 存储=函数注入保持 L0 纯度) */
static int cact_nvs_save(const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("cact", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, "cfg", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static int cact_nvs_load(void *buf, size_t cap, size_t *len)
{
    nvs_handle_t h;
    if (nvs_open("cact", NVS_READONLY, &h) != ESP_OK) return 1;
    size_t need = 0;
    esp_err_t e = nvs_get_blob(h, "cfg", NULL, &need);
    if (e != ESP_OK) {
        nvs_close(h);
        return 1;                           /* 首次开机 */
    }
    if (need > cap) {
        nvs_close(h);
        return -1;
    }
    e = nvs_get_blob(h, "cfg", buf, &need);
    nvs_close(h);
    if (e != ESP_OK) return -1;
    *len = need;
    return 0;
}

static const cact_store_t CACT_NVS_STORE = { cact_nvs_save, cact_nvs_load };

void act_gate_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        ESP_LOGW(TAG, "act gate degraded: no lock (single-owner mode)");
    } else {
        cact_set_lock_port(&s_lock_port);
    }
    cact_default_init();
    /* T38: 设置持久化(自主/勿扰/分类; 修改即持久, 重启恢复) */
    cact_attach_store(&CACT_NVS_STORE);
    cact_restore(cact_default());
    for (size_t i = 0; i < sizeof(s_caps) / sizeof(s_caps[0]); i++) {
        cact_res_t r = cact_register(cact_default(), &s_caps[i]);
        if (r != CACT_RES_OK) {
            ESP_LOGW(TAG, "cap %s register fail: %d", s_caps[i].cap_id, r);
        }
    }
    /* 等价迁移: 现有行为无意图持久化; T20 恢复机制接入后改为
     * "验证已保存意图配置"后再开(docs/17 §5.5) */
    cact_boot_gate_open(cact_default(), true);
    /* persona 两段初始化(T20): 先运行态(无角色, epoch=1), SD 就绪事件
     * 后 persona_sd_try() 再 bind+load(迟挂载语义见 persona_sd_try) */
    persona_init(&s_psn_svc, psn_rng);
    s_psn_scratch = heap_caps_calloc(8224, 1, MALLOC_CAP_SPIRAM);
    s_psn_io_buf = heap_caps_calloc(4096, 1, MALLOC_CAP_SPIRAM);
    if (s_psn_scratch == NULL || s_psn_io_buf == NULL) {
        ESP_LOGW(TAG, "persona store degraded (no psram scratch)");
    } else {
        cube_event_subscribe(persona_sd_evt_cb, NULL);
        cube_event_subscribe(psn_trait_evo_cb, NULL);
        persona_sd_try();              /* SD 已就绪时立即装载(热重启) */
    }
    ESP_LOGI(TAG, "act gate up: %zu caps, autonomous=default-on",
             sizeof(s_caps) / sizeof(s_caps[0]));
    /* T14/批次 16 原设计接线(批次 44 恢复: 重构时曾丢失, 致 EPSCV2
     * 透明容器从未启用, SD 记忆/凭据全明文): SD 已挂载(main 先 storage
     * 后 act_gate), 以 PSA port + trust sd_key 启用透明容器 */
    app_storage_crypt_enable();
    ESP_LOGI(TAG, "storage crypt: %s",
             app_storage_crypt_active() ? "EPSCV2 on" : "OFF (trust fail)");
}

/* ---- 共享 helper(CLI/触摸/技能各入口统一走此提交) ---- */
cact_res_t act_gate_submit(const char *cap, const uint8_t *params,
                           size_t params_len, cact_origin_t origin,
                           uint32_t *out_id)
{
    cact_req_t r;
    memset(&r, 0, sizeof(r));
    r.origin = origin;
    strlcpy(r.capability, cap, sizeof(r.capability));
    if (params != NULL && params_len > 0 && params_len <= CACT_PARAMS_MAX) {
        memcpy(r.params, params, params_len);
        r.params_len = params_len;
    }
    return cact_submit(cact_default(), &r, out_id);
}

void act_gate_pump(void) { cact_pump(cact_default()); }

bool act_gate_started(uint32_t id)
{
    cact_receipt_t rc;
    return cact_receipt_last(cact_default(), id, &rc) == CACT_RES_OK &&
           rc.status == CACT_ST_STARTED;
}

void act_gate_user_stop_audio(const char *reason)
{
    cact_user_stop(cact_default(), 1, reason);   /* 取消在途+锁存组1 */
#if CONFIG_ESP_CUBE_ENABLE_SOUL
    soul_note_user_stop();                       /* 停止归因→radio_start 退避 */
#endif
}

void act_gate_resume_audio(void) { cact_user_resume(cact_default(), 1); }
