/*
 * face_perf_stats.c —— 表情帧耗时统计实现(纯 C, 零 IDF 依赖)
 * 契约与百分位口径见 face_perf_stats.h。
 */
#include "face_perf_stats.h"

#include <stddef.h>
#include <string.h>

int face_perf_ring_init(face_perf_ring_t *r, uint32_t *buf, uint32_t cap,
                        uint32_t ncol)
{
    if (r == NULL) return -1;
    memset(r, 0, sizeof(*r));
    if (buf == NULL || cap == 0 || ncol == 0 ||
        ncol > FACE_PERF_STATS_MAX_COL ||
        (uint64_t)cap * ncol > UINT32_MAX) {
        return -1;
    }
    r->buf = buf;
    r->cap = cap;
    r->ncol = ncol;
    return 0;
}

void face_perf_ring_reset(face_perf_ring_t *r)
{
    if (r == NULL) return;
    r->head = 0;
    r->len = 0;
    r->pushed = 0;
}

void face_perf_ring_push(face_perf_ring_t *r, const uint32_t *rec)
{
    if (r == NULL || r->cap == 0 || rec == NULL) return;
    uint32_t *dst = r->buf + (size_t)r->head * r->ncol;
    for (uint32_t c = 0; c < r->ncol; c++) dst[c] = rec[c];
    r->head = (r->head + 1u == r->cap) ? 0u : r->head + 1u;
    if (r->len < r->cap) r->len++;
    if (r->pushed != UINT32_MAX) r->pushed++;
}

uint32_t face_perf_ring_column(const face_perf_ring_t *r, uint32_t col,
                               uint32_t *out, uint32_t out_cap)
{
    if (r == NULL || r->cap == 0 || out == NULL || col >= r->ncol) return 0;
    uint32_t k = r->len < out_cap ? r->len : out_cap;
    /* 最新 k 条的起点; 写成分支避免 head + cap 在大 cap 下溢出 */
    uint32_t idx = r->head >= k ? r->head - k : r->head + (r->cap - k);
    for (uint32_t i = 0; i < k; i++) {
        out[i] = r->buf[(size_t)idx * r->ncol + col];
        idx = (idx + 1u == r->cap) ? 0u : idx + 1u;
    }
    return k;
}

/* 堆排序(升序): 不分配、不递归, 栈用量与 n 无关 */
static void sift_down(uint32_t *a, uint32_t root, uint32_t end)
{
    /* root < end / 2 保证 2 * root + 1 <= end - 1, 且不溢出 */
    while (root < end / 2u) {
        uint32_t child = 2u * root + 1u;
        if (child + 1u < end && a[child] < a[child + 1u]) child++;
        if (a[root] >= a[child]) return;
        uint32_t t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static void heap_sort_u32(uint32_t *a, uint32_t n)
{
    if (n < 2u) return;
    for (uint32_t i = n / 2u; i-- > 0u;) sift_down(a, i, n);
    for (uint32_t end = n - 1u; end > 0u; end--) {
        uint32_t t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift_down(a, 0u, end);
    }
}

int face_perf_summarize(uint32_t *vals, uint32_t n, const uint32_t *thr,
                        uint32_t nthr, face_perf_summary_t *out)
{
    if (out == NULL || (vals == NULL && n > 0u) ||
        (thr == NULL && nthr > 0u) || nthr > FACE_PERF_STATS_MAX_THR) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->nthr = nthr;
    for (uint32_t k = 0; k < nthr; k++) out->thr[k] = thr[k];
    if (n == 0u) return 0;

    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t k = 0; k < nthr; k++) {
            if (vals[i] > thr[k]) out->over[k]++;
        }
    }
    heap_sort_u32(vals, n);
    out->n = n;
    out->p50 = vals[face_perf_idx_p50(n)];
    out->p95 = vals[face_perf_idx_p95(n)];
    out->max = vals[n - 1u];
    return 0;
}

int face_perf_ring_summarize(const face_perf_ring_t *r, uint32_t col,
                             uint32_t *scratch, uint32_t scratch_cap,
                             const uint32_t *thr, uint32_t nthr,
                             face_perf_summary_t *out)
{
    if (r == NULL || r->cap == 0 || col >= r->ncol ||
        (scratch == NULL && scratch_cap > 0u)) {
        return -1;
    }
    uint32_t m = face_perf_ring_column(r, col, scratch, scratch_cap);
    return face_perf_summarize(scratch, m, thr, nthr, out);
}
