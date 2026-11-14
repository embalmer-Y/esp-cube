/* pair_port_psa.c —— pair 密码学原语 port(目标装配; T15)
 * SHA-256 = PSA one-shot; rng = esp_random。
 * 装配方(P4/S31 固件)把此 port 传给 pair_init。
 */
#include <stdbool.h>
#include <string.h>

#include "psa/crypto.h"
#include "esp_random.h"

#include "srp_port.h"

static bool s_psa_inited;

static void pair_hash(void *ud, const uint8_t *data, size_t len,
                      uint8_t out[32])
{
    (void)ud;
    if (!s_psa_inited) {
        /* 失败即卡死 pairing(不会发生: psa_crypto_init 幂等) */
        (void)psa_crypto_init();
        s_psa_inited = true;
    }
    size_t olen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, len,
                         out, 32, &olen) != PSA_SUCCESS || olen != 32) {
        memset(out, 0, 32);
    }
}

static int pair_rng(void *ud, uint8_t *out, size_t len)
{
    (void)ud;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t r = esp_random();
        memcpy(out + i, &r, 4);
    }
    for (size_t i = len - len % 4; i < len; i++) {
        out[i] = (uint8_t)esp_random();
    }
    return 0;
}

const srp_port_t *pair_port_psa(void)
{
    static const srp_port_t p = { NULL, pair_hash, pair_rng };
    return &p;
}
