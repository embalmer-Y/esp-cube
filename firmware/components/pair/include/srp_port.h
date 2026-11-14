/* srp_port.h —— SRP 密码学原语注入口(T15; 与 store_crypt 的 port 同型)
 *
 * hash: 单次哈希 out[32] = H(data[0..len))(档内固定 SHA-256 语义,
 *       由实现方保证); rng: 随机填充, 0=OK。
 * 目标装配 = PSA SHA-256 + esp_random(pair_port_psa.c);
 * 宿主测试 = 独立实现 + 系统/确定性熵。
 */
#ifndef SRP_PORT_H
#define SRP_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ud;
    void (*hash)(void *ud, const uint8_t *data, size_t len,
                 uint8_t out[32]);
    int (*rng)(void *ud, uint8_t *out, size_t len);
} srp_port_t;

#ifdef __cplusplus
}
#endif
#endif /* SRP_PORT_H */
