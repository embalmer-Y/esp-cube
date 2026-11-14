/*
 * store_slot.c —— 双槽可恢复提交协议实现(docs/17 §8.2; 见 store_slot.h)
 */
#include <stdbool.h>
#include <string.h>

#include "store_slot.h"

#define STORE_MAGIC      0x5354534Cu   /* 'STSL' little-endian 视角 */
#define STORE_HDR_VER    1u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t hdr_ver;
    uint16_t committed;      /* 0=staging 1=committed */
    uint32_t generation;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t hdr_crc;        /* 前 20 字节的 CRC32 */
} store_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(store_hdr_t) == STORE_HDR_SIZE, "头部布局应为 24B");

/* CRC32 流式(init/update/final; 两段载荷共用) */
#define CRC32_INIT 0xFFFFFFFFu
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}
static uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t store_crc32(const void *data, size_t len)
{
    return crc32_final(crc32_update(CRC32_INIT, data, len));
}

static uint32_t store_crc32_2(const void *a, size_t al, const void *b,
                              size_t bl)
{
    uint32_t c = crc32_update(CRC32_INIT, a, al);
    return crc32_final(crc32_update(c, b, bl));
}

const char *store_slot_status_name(store_status_t st)
{
    switch (st) {
    case STORE_STATUS_OK:                return "ok";
    case STORE_STATUS_EMPTY:             return "empty";
    case STORE_STATUS_RECOVERY_REQUIRED: return "recovery_required";
    case STORE_STATUS_IO_ERROR:          return "io_error";
    case STORE_STATUS_INVALID:           return "invalid";
    default:                             return "?";
    }
}

static void hdr_fill2(store_hdr_t *h, uint32_t gen, uint16_t committed,
                      const void *head, size_t head_len, const void *body,
                      size_t body_len);

static void hdr_fill(store_hdr_t *h, uint32_t gen, uint16_t committed,
                     const void *payload, size_t len)
{
    hdr_fill2(h, gen, committed, payload, len, NULL, 0);
}

static void hdr_fill2(store_hdr_t *h, uint32_t gen, uint16_t committed,
                      const void *head, size_t head_len, const void *body,
                      size_t body_len)
{
    memset(h, 0, sizeof(*h));
    h->magic = STORE_MAGIC;
    h->hdr_ver = STORE_HDR_VER;
    h->committed = committed;
    h->generation = gen;
    h->payload_len = (uint32_t)(head_len + body_len);
    h->payload_crc = store_crc32_2(head, head_len, body, body_len);
    h->hdr_crc = store_crc32(h, 20);     /* 到 hdr_crc 字段之前 */
}

static bool hdr_valid(const store_hdr_t *h, uint32_t max_payload,
                      size_t file_len)
{
    if (h->magic != STORE_MAGIC || h->hdr_ver != STORE_HDR_VER) return false;
    if (h->generation == 0) return false;
    if (h->payload_len == 0 || h->payload_len > max_payload) return false;
    if ((size_t)h->payload_len + STORE_HDR_SIZE != file_len) return false;
    if (h->hdr_crc != store_crc32(h, 20)) return false;
    return true;
}

/* 读整槽文件到 scratch; 返回 0=OK 1=IO 2=NOENT(文件大于 scratch 视为损坏) */
static int slot_read(const store_io_t *io, void *ud, int slot,
                     uint8_t *scratch, uint32_t scratch_cap, size_t *len)
{
    int rc = io->read(ud, slot, scratch, scratch_cap, len);
    if (rc != 0) return rc;
    if (*len > scratch_cap) return 1;    /* 不可能: read 已按 cap 截断 */
    return 0;
}

/* 校验 scratch 中整个槽文件; 合法 committed 返回 true 并填 gen */
static bool slot_validate(uint8_t *scratch, size_t len, uint32_t max_payload,
                          uint32_t *gen_out, bool *committed_out)
{
    if (len < STORE_HDR_SIZE) return false;
    store_hdr_t h;
    memcpy(&h, scratch, sizeof(h));
    if (!hdr_valid(&h, max_payload, len)) return false;
    uint32_t crc = store_crc32(scratch + STORE_HDR_SIZE, h.payload_len);
    if (crc != h.payload_crc) return false;
    *gen_out = h.generation;
    *committed_out = (h.committed == 1);
    return true;
}

store_status_t store_slot_init(store_slot_t *s, const store_io_t *io,
                               uint32_t max_payload, uint8_t *scratch,
                               uint32_t scratch_cap)
{
    if (s == NULL || io == NULL || scratch == NULL ||
        scratch_cap < max_payload + STORE_HDR_SIZE) {
        return STORE_STATUS_INVALID;
    }
    memset(s, 0, sizeof(*s));
    s->io = io;
    s->max_payload = max_payload;
    s->scratch = scratch;
    s->scratch_cap = scratch_cap;
    s->active_slot = -1;
    s->generation = 0;

    int files_present = 0;
    int best_slot = -1;
    uint32_t best_gen = 0;
    for (int slot = STORE_SLOT_A; slot <= STORE_SLOT_B; slot++) {
        size_t len = 0;
        int rc = slot_read(io, io->ud, slot, scratch, scratch_cap, &len);
        if (rc == 2) continue;                 /* 不存在 */
        if (rc != 0) {
            s->status = STORE_STATUS_RECOVERY_REQUIRED;
            return s->status;                  /* 读失败: 停写保护 */
        }
        files_present++;
        uint32_t gen = 0;
        bool committed = false;
        if (slot_validate(scratch, len, max_payload, &gen, &committed) &&
            committed && gen > best_gen) {
            best_gen = gen;
            best_slot = slot;
        }
    }
    if (files_present == 0) {
        s->status = STORE_STATUS_EMPTY;
        return s->status;
    }
    if (best_slot < 0) {
        s->status = STORE_STATUS_RECOVERY_REQUIRED;
        return s->status;
    }
    s->active_slot = best_slot;
    s->generation = best_gen;
    s->status = STORE_STATUS_OK;
    return s->status;
}

store_status_t store_slot_commit(store_slot_t *s, const void *payload,
                                 size_t len, uint32_t *new_gen)
{
    return store_slot_commit_parts(s, payload, len, NULL, 0, new_gen);
}

store_status_t store_slot_commit_parts(store_slot_t *s, const void *head,
                                       size_t head_len, const void *body,
                                       size_t body_len, uint32_t *new_gen)
{
    if (s == NULL || s->io == NULL || head == NULL || new_gen == NULL ||
        (body == NULL && body_len != 0)) {
        return STORE_STATUS_INVALID;
    }
    const void *payload = head;
    size_t len = head_len + body_len;
    if (s->status == STORE_STATUS_RECOVERY_REQUIRED) {
        return STORE_STATUS_RECOVERY_REQUIRED;   /* 停写保护 */
    }
    if (len == 0 || len > s->max_payload) return STORE_STATUS_INVALID;

    const store_io_t *io = s->io;
    int inactive = (s->active_slot == STORE_SLOT_A) ? STORE_SLOT_B
                 : STORE_SLOT_A;                   /* 空/含活动槽皆可 */
    uint32_t gen = s->generation + 1;

    /* 1) staging 写非活动槽(committed=0); 两段拼装进 scratch(内部区) */
    store_hdr_t h;
    hdr_fill2(&h, gen, 0, payload, head_len, body, body_len);
    memcpy(s->scratch, &h, STORE_HDR_SIZE);
    memcpy(s->scratch + STORE_HDR_SIZE, payload, head_len);
    if (body_len != 0) {
        memcpy(s->scratch + STORE_HDR_SIZE + head_len, body, body_len);
    }
    int rc = io->write_full(io->ud, inactive, s->scratch,
                            STORE_HDR_SIZE + len);
    if (rc != 0) return STORE_STATUS_IO_ERROR;

    /* 2) 回读校验 staging(含短写检测) */
    size_t rlen = 0;
    rc = slot_read(io, io->ud, inactive, s->scratch, s->scratch_cap, &rlen);
    if (rc != 0) return STORE_STATUS_IO_ERROR;
    uint32_t vgen = 0;
    bool committed = false;
    if (!slot_validate(s->scratch, rlen, s->max_payload, &vgen, &committed) ||
        committed || vgen != gen) {
        return STORE_STATUS_IO_ERROR;            /* staging 不完整: 旧代不动 */
    }

    /* 3) 翻提交位(仅重写头部 24B; 两段 CRC 保持一致) */
    hdr_fill2(&h, gen, 1, payload, head_len, body, body_len);
    rc = io->write_at(io->ud, inactive, 0, &h, STORE_HDR_SIZE);
    if (rc != 0) return STORE_STATUS_IO_ERROR;

    /* 4) 回读确认提交位与内容一致 */
    rc = slot_read(io, io->ud, inactive, s->scratch, s->scratch_cap, &rlen);
    if (rc != 0) return STORE_STATUS_IO_ERROR;
    if (!slot_validate(s->scratch, rlen, s->max_payload, &vgen, &committed) ||
        !committed || vgen != gen) {
        return STORE_STATUS_IO_ERROR;
    }

    /* 5) 切换活动槽 */
    s->active_slot = inactive;
    s->generation = gen;
    s->status = STORE_STATUS_OK;
    *new_gen = gen;
    return STORE_STATUS_OK;
}

store_status_t store_slot_read(store_slot_t *s, void *buf, size_t cap,
                               size_t *len)
{
    if (s == NULL || s->io == NULL || len == NULL || s->active_slot < 0 ||
        (buf != NULL && buf >= (void *)s->scratch &&
         buf < (void *)(s->scratch + s->scratch_cap))) {
        /* buf 与内部组装区重叠 = API 误用(防自拷贝) */
        return STORE_STATUS_INVALID;
    }
    size_t rlen = 0;
    int rc = s->io->read(s->io->ud, s->active_slot, s->scratch,
                         s->scratch_cap, &rlen);
    if (rc != 0) return STORE_STATUS_IO_ERROR;
    uint32_t gen = 0;
    bool committed = false;
    if (!slot_validate(s->scratch, rlen, s->max_payload, &gen, &committed) ||
        !committed || gen != s->generation) {
        s->status = STORE_STATUS_RECOVERY_REQUIRED;   /* 活动槽劣化 */
        return s->status;
    }
    store_hdr_t h;
    memcpy(&h, s->scratch, sizeof(h));
    if (buf == NULL) {                     /* 只校验 + 原地解析模式 */
        *len = h.payload_len;
        return STORE_STATUS_OK;
    }
    if (cap < h.payload_len) return STORE_STATUS_INVALID;
    memcpy(buf, s->scratch + STORE_HDR_SIZE, h.payload_len);
    *len = h.payload_len;
    return STORE_STATUS_OK;
}

const void *store_slot_payload(const store_slot_t *s)
{
    return (s != NULL) ? (const void *)(s->scratch + STORE_HDR_SIZE) : NULL;
}

size_t store_slot_payload_len(const store_slot_t *s)
{
    if (s == NULL || s->active_slot < 0) return 0;
    size_t rlen = 0;
    if (s->io->read(s->io->ud, s->active_slot, s->scratch, s->scratch_cap,
                    &rlen) != 0) {
        return 0;
    }
    store_hdr_t h;
    memcpy(&h, s->scratch, sizeof(h));
    if (!hdr_valid(&h, s->max_payload, rlen) || h.committed != 1) return 0;
    return h.payload_len;
}

uint32_t store_slot_generation(const store_slot_t *s)
{
    return (s != NULL) ? s->generation : 0;
}
