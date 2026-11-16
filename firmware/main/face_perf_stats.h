/*
 * face_perf_stats.h —— 表情帧耗时统计(纯 C, 零 IDF 依赖, 宿主可测)
 *
 * 用途(freeze-and-face-blackscreen-fix 任务 4.1): face_perf.c(任务 4.2)把
 * D/R/F/T/I_loop/I_refr 样本写进本模块的样本环, `faceperf report` 与 splash
 * 摘要用 face_perf_summarize() 得到 n、P50/P95/max 与超阈计数。
 * 宿主测试: firmware/main/test/test_face_perf_stats.c(host_tests.sh [18])。
 * design.md 原定把百分位函数放在 sysdiag_core.c; 表情线先于 sysdiag 推进,
 * 改放本文件(L5 纯 C), sysdiag 不依赖它。
 *
 * 内存: 本模块不持有任何静态缓冲、不调用 malloc。样本环存储与百分位用的
 * 排序缓冲都由调用者提供(设备端按执行约定走 PSRAM 堆,
 * heap_caps_malloc(MALLOC_CAP_SPIRAM), 测量时分配、测完释放)。
 * 例: 每帧一条 {D, R, F, T} 记录, ncol = 4、cap = 1024 → 1024 × 16B = 16KB。
 *
 * 并发: 不加锁。同一个环只允许一个写者; 读(column/summarize)与写不得并发,
 * 由调用者串行化(例如在 LVGL 锁内读, 或先停止采样再出报告)。
 *
 * 百分位口径(与 CLI `baseline` 命令一致, firmware/main/cli.c):
 *   样本升序排序后 s[0..n-1],
 *     P50 = s[n / 2]                 (n = 2 时取较大者 s[1])
 *     P95 = s[(n * 95) / 100]        (整数除法向下取整)
 *     max = s[n - 1]
 *   n >= 1 时 (n * 95) / 100 <= n - 1 恒成立, 下标不越界, 无需钳位;
 *   乘法按 64 位计算, 任意 uint32_t n 都不溢出。
 *   n = 0 时 n/p50/p95/max 与全部超阈计数为 0。
 * 超阈计数: over[k] = 满足 v > thr[k](严格大于)的样本数。预算写作
 *   "P95 <= 33ms", 恰好 33ms 不算超。
 * 单位: 本模块不解释单位; face_perf 约定用微秒, 阈值见
 *   FACE_PERF_STATS_THR_US_INIT(33/66/100ms)。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_PERF_STATS_MAX_COL 8   /* 每条记录最多 8 个 uint32_t 字段 */
#define FACE_PERF_STATS_MAX_THR 4   /* 每次汇总最多 4 个阈值 */

/* faceperf report 的超阈口径: 33/66/100ms, 以微秒计 */
#define FACE_PERF_STATS_THR_US_INIT {33000u, 66000u, 100000u}
#define FACE_PERF_STATS_THR_US_COUNT 3u

/* P50/P95 下标规则(唯一来源, 见文件头); n 须 >= 1 */
static inline uint32_t face_perf_idx_p50(uint32_t n)
{
    return n / 2u;
}

static inline uint32_t face_perf_idx_p95(uint32_t n)
{
    return (uint32_t)(((uint64_t)n * 95u) / 100u);
}

/*
 * 样本环: cap 条记录, 每条 ncol 个 uint32_t, 存储 buf 至少 cap * ncol 个元素。
 * 写满后覆盖最旧记录。字段由调用者解释, 本模块只按列读出。
 */
typedef struct {
    uint32_t *buf;
    uint32_t cap;     /* 记录容量; 0 = 未初始化/初始化失败, push 为空操作 */
    uint32_t ncol;    /* 每条记录的字段数, 1..FACE_PERF_STATS_MAX_COL */
    uint32_t head;    /* 下一条写入位置 */
    uint32_t len;     /* 有效记录数, <= cap */
    uint32_t pushed;  /* 累计 push 次数(饱和于 UINT32_MAX); 被覆盖数 = pushed - len */
} face_perf_ring_t;

/* 参数非法(r/buf 为 NULL、cap 为 0、ncol 越界、cap*ncol 超出 uint32_t)
 * 返回 -1, 此时 r(非 NULL 时)被置为 cap = 0 的惰性状态; 成功返回 0。 */
int face_perf_ring_init(face_perf_ring_t *r, uint32_t *buf, uint32_t cap,
                        uint32_t ncol);

/* 清空记录与计数, 保留 buf/cap/ncol */
void face_perf_ring_reset(face_perf_ring_t *r);

/* 追加一条记录(读 rec[0..ncol-1]); r 未初始化或 rec 为 NULL 时为空操作 */
void face_perf_ring_push(face_perf_ring_t *r, const uint32_t *rec);

/*
 * 把第 col 列拷到 out, 按从旧到新排列; 返回拷贝条数 min(len, out_cap)。
 * out_cap < len 时只取最新的 out_cap 条。col 越界或参数为 NULL 返回 0。
 */
uint32_t face_perf_ring_column(const face_perf_ring_t *r, uint32_t col,
                               uint32_t *out, uint32_t out_cap);

typedef struct {
    uint32_t n;
    uint32_t p50;
    uint32_t p95;
    uint32_t max;
    uint32_t nthr;
    uint32_t thr[FACE_PERF_STATS_MAX_THR];
    uint32_t over[FACE_PERF_STATS_MAX_THR];  /* v > thr[k] 的样本数 */
} face_perf_summary_t;

/*
 * 汇总 vals[0..n-1]。会把 vals 原地升序排序(调用者传入可改写的拷贝)。
 * 不分配内存, 栈用量与 n 无关(堆排序)。
 * out 为 NULL、vals 为 NULL 且 n > 0、thr 为 NULL 且 nthr > 0、
 * nthr > FACE_PERF_STATS_MAX_THR 时返回 -1 且不改 vals; 成功返回 0。
 */
int face_perf_summarize(uint32_t *vals, uint32_t n, const uint32_t *thr,
                        uint32_t nthr, face_perf_summary_t *out);

/*
 * 便捷组合: face_perf_ring_column(r, col, scratch, scratch_cap) 后
 * face_perf_summarize(scratch, ...)。scratch_cap < len 时只汇总最新的
 * scratch_cap 条。参数非法(含 col 越界)返回 -1。
 */
int face_perf_ring_summarize(const face_perf_ring_t *r, uint32_t col,
                             uint32_t *scratch, uint32_t scratch_cap,
                             const uint32_t *thr, uint32_t nthr,
                             face_perf_summary_t *out);

#ifdef __cplusplus
}
#endif
