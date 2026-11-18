/*
 * mem_budget.c —— 内存预算审计(docs/22 §3E/§4 执法点, 2026-11-18)
 *
 * 背景: 三大内存根因(§52 PIE 栈落 RTCRAM / hosted 15872B DMA 挤兑 /
 * voice+CLI 栈级联失败)共同教训 = 固定开销靠运行期堆碰运气。本模块
 * 把"预算"变成板上可对账的数字: 每次调用打印 internal/dma/psram 三域
 * 水位, 并对照下限表逐条判定。
 *
 * 数值来源 = v5 固件(f1d0482f) 2026-11-18 1h 回归实测:
 *   app_main largest internal=155648 / pre_ui largest=31744 dma=25600
 *   / startup complete 内部剩 ~26.6KB(free 30509027 - psram 30482404)。
 * 下限是"当前已知危险线", 不是目标 —— P1 去内部化后应上调 dma 行。
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mem_budget.h"

static const char *TAG = "membudget";

/* 单条预算下限: largest/free 低于此值 = 该域已被挤入危险区 */
typedef struct {
    const char *name;      /* 规则名(docs/22 行号) */
    uint32_t floor;        /* 下限(字节) */
    char why[48];          /* 为什么是这个数 */
} mem_floor_t;

#if CONFIG_IDF_TARGET_ESP32P4
/* P4C5 下限: hosted SDIO 聚合缓冲单次 dma_alloc 15872B(§52 网络失效
 * 根因), 下限 16KB 即"hosted 下一笔必失败"; 稳态内部 free 实测 26.6KB,
 * 24KB 为挤兑告警线; psram 电台播放期 HLS 缓冲合法吃 ~2MB(2026-11-18
 * ×10 压测实测 28.4MB free), 哨兵取 26MB 只拦真异常。 */
static const mem_floor_t s_floors[] = {
    { "dma_largest",   16384, "hosted 15872B 单次需求(docs/22 §3E)" },
    { "internal_free", 24576, "稳态实测 26.6KB, 低于此=已挤兑" },
    { "psram_free",  26U<<20, "HLS 播放期合法 ~2MB 之外的异常哨兵" },
};
#else
/* S31: 无 hosted(原生 WiFi), 暂无稳态实测 —— 首次上板用 `mem` 采样
 * 校准后回填(docs/22 §6 待办); 先给保守哨兵避免静默泄漏。 */
static const mem_floor_t s_floors[] = {
    { "dma_largest",    8192, "WiFi 驱动 DMA 瞬态(S31 待校准)" },
    { "internal_free", 12288, "S31 待校准" },
    { "psram_free",   6U<<20, "S31 8MB 板哨兵" },
};
#endif

void mem_budget_audit(const char *when)
{
    unsigned in_free  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned in_larg  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned dma_larg = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    unsigned ps_free  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    unsigned ps_larg  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "audit[%s] internal free=%u largest=%u | dma largest=%u | psram free=%u largest=%u",
             when, in_free, in_larg, dma_larg, ps_free, ps_larg);

    /* 下限对账(取对应域实测值; FAIL 只告警 —— 见文件头) */
    unsigned vals[3] = { dma_larg, in_free, ps_free };
    for (size_t i = 0; i < sizeof(s_floors) / sizeof(s_floors[0]); i++) {
        if (vals[i] < s_floors[i].floor) {
            ESP_LOGW(TAG, "audit[%s] FAIL %s=%u < %u (%s)",
                     when, s_floors[i].name, vals[i], s_floors[i].floor,
                     s_floors[i].why);
        }
    }
}

void mem_budget_dump(void)
{
    mem_budget_audit("cli");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);
}
