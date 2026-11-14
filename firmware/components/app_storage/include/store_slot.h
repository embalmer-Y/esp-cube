/*
 * store_slot.h —— 双槽可恢复提交协议(docs/17 §8.2; docs/18 T07)
 *
 * 不变量:
 *   1. 任何时刻至少保留一个"已验证提交代"; 新代未通过回读校验前,
 *      活动槽不变 —— 绝不因半截写入把旧数据当空库。
 *   2. 槽文件自描述: 头部含 magic/代数/长度/内容 CRC/提交标记,
 *      staging(committed=0)写入成功并回读校验后才翻提交位。
 *   3. 有文件但无有效提交代 → RECOVERY_REQUIRED, 拒绝写(保护现场,
 *      等上层恢复流程; docs/17 §8.2 第 4 步)。
 *   4. 单写 owner: 本协议不做加锁, 并发串行化由上层存储 worker 负责
 *      (docs/17 §15.4 StorageWorker)。
 *
 * 与 docs/17 §8.2 的差异说明: 原文描述"独立 manifest 文件"; 本实现把
 * manifest 作为槽文件内嵌头部 —— 恢复不变量相同(每槽自证提交代),
 * 少一对 manifest 文件的写序问题。故障注入见 test/test_store_slot.c。
 *
 * 删除防回退(撤销代次/删除意图)属 T20/T23 域, 不在本协议范围。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_SLOT_A     0
#define STORE_SLOT_B     1
#define STORE_HDR_SIZE   24

typedef enum {
    STORE_STATUS_OK = 0,             /* 已恢复/提交到有效提交代 */
    STORE_STATUS_EMPTY,              /* 无任何槽文件: 全新库 */
    STORE_STATUS_RECOVERY_REQUIRED,  /* 有文件无有效提交代: 停写待恢复 */
    STORE_STATUS_IO_ERROR,           /* 本次操作 IO 失败(旧代不受影响) */
    STORE_STATUS_INVALID,            /* 参数/容量非法 */
} store_status_t;

const char *store_slot_status_name(store_status_t st);

/* IO 抽象(实现方: SD/文件; 返回 0=OK 1=IO 错 2=不存在) */
typedef struct {
    int (*read)(void *ud, int slot, void *buf, size_t cap, size_t *len);
    int (*write_full)(void *ud, int slot, const void *buf, size_t len);
    int (*write_at)(void *ud, int slot, size_t off, const void *buf,
                    size_t len);
    void *ud;
} store_io_t;

typedef struct {
    const store_io_t *io;
    uint32_t max_payload;      /* 单槽 payload 上限 */
    uint8_t *scratch;          /* 调用方提供, ≥ max_payload + STORE_HDR_SIZE */
    uint32_t scratch_cap;
    uint32_t generation;       /* 当前有效提交代(0=空库) */
    int active_slot;           /* -1=尚无 */
    store_status_t status;     /* 持久状态(与单次操作返回值区分) */
} store_slot_t;

/* 恢复: 读两槽并选最高有效提交代; 返回最终状态 */
store_status_t store_slot_init(store_slot_t *s, const store_io_t *io,
                               uint32_t max_payload, uint8_t *scratch,
                               uint32_t scratch_cap);

/* 提交新代: staging 写非活动槽 → 回读校验 → 翻提交位 → 回读确认。
 * 任一步失败返回 IO_ERROR 且旧代/活动槽不变; RECOVERY_REQUIRED 时拒绝。 */
store_status_t store_slot_commit(store_slot_t *s, const void *payload,
                                 size_t len, uint32_t *new_gen);

/* 两段式提交(payload = head+body 两段拼接; 免调用方中转缓冲)。
 * head/body 不得指向 s->scratch(内部组装区) —— ASan 可查重叠。 */
store_status_t store_slot_commit_parts(store_slot_t *s, const void *head,
                                       size_t head_len, const void *body,
                                       size_t body_len, uint32_t *new_gen);

/* 读回当前有效提交代 payload(含 CRC 重验; cap 须 ≥ payload_len;
 * buf 不得与内部 scratch 重叠 —— 传 NULL 则只校验, 配合
 * store_slot_payload() 原地解析, 免一次拷贝) */
store_status_t store_slot_read(store_slot_t *s, void *buf, size_t cap,
                               size_t *len);
const void *store_slot_payload(const store_slot_t *s);  /* 上次 read 后有效 */
size_t store_slot_payload_len(const store_slot_t *s);
uint32_t store_slot_generation(const store_slot_t *s);

/* CRC32(IEEE, 多项式 0xEDB88320; 无表实现, 供协议与测试共用) */
uint32_t store_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
