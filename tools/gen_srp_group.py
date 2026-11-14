#!/usr/bin/env python3
"""gen_srp_group.py —— SRP-6a 大素数群: 生成 / 校验

背景: IDF 6.1 mbedtls 4.x 已移除 SRP; 自实现方案需要一个大素数群。
不转录 RFC 5054 常量(转录错误=隐蔽协议缺陷), 本地生成安全素数:
  p = 2q + 1 (q 素数, Miller-Rabin 64 轮)
  g = 整群生成元(对安全素数: pow(g,2,p)!=1 且 pow(g,q,p)!=1)
生成结果粘贴进两处(同源免疫转录差):
  - firmware/components/pair/src/srp6a.c   (C 常量)
  - tools/srp_ref.py                        (Python 参考实现常量)

用法:
  python3 tools/gen_srp_group.py gen        # 生成(数秒~数十秒)
  python3 tools/gen_srp_group.py check <p_hex> <g>   # 校验已嵌入常量
"""
import secrets
import sys


def is_probable_prime(n: int, rounds: int = 64) -> bool:
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


# 小素数预筛积(命中即免 MR 大数幂运算; 3071-bit 候选 MR 一轮≈秒级)
_SMALL_PRIMES = [p for p in range(3, 2000, 2) if all(
    p % d for d in range(3, int(p ** 0.5) + 1, 2))]
_PROD = 1
for _p in _SMALL_PRIMES:
    _PROD *= _p


def _passes_prefilter(n: int) -> bool:
    from math import gcd
    return gcd(n, _PROD) == 1


def gen_safe_prime(bits: int, progress=None) -> tuple[int, int]:
    """bits 位素数 p, 返回 (p, q); q = (p-1)/2 为 bits-1 位。

    筛法: 小素数 gcd 预筛(便宜) → MR 16 轮(gen 档; 最终常量另经
    check 模式 64 轮权威校验)。
    """
    target = bits - 1
    tried = 0
    while True:
        q = secrets.randbits(target)
        q |= (1 << (target - 1)) | 1        # 顶位保证量级, 末位保证奇
        tried += 1
        if tried % 200 == 0 and progress is not None:
            progress(f"  candidates={tried}")
        if not _passes_prefilter(q):
            continue
        if not is_probable_prime(q, rounds=16):
            continue
        p = 2 * q + 1
        if _passes_prefilter(p) and is_probable_prime(p, rounds=16):
            if progress is not None:
                progress(f"  found q after {tried} candidates")
            return p, q


def gen_generator(p: int, q: int) -> int:
    for g in range(2, 64):
        if pow(g, 2, p) != 1 and pow(g, q, p) != 1:
            return g
    raise RuntimeError("no generator in 2..63")


def cmd_gen() -> None:
    p, q = gen_safe_prime(
        3072, progress=lambda m: print(m, flush=True))
    g = gen_generator(p, q)
    n_hex = f"{p:0768x}"
    print("# ---- 粘贴到 srp6a.c / srp_ref.py(同源) ----")
    print(f"# N = {p.bit_length()}-bit safe prime; g = {g}")
    for i in range(0, len(n_hex), 64):
        print(f"#   {n_hex[i:i+64]}")
    print(f"N_HEX = \"{n_hex}\"")
    print(f"G = {g}")
    # 结果落盘(重跑/中断不丢), 同时输出供粘贴
    with open("tools/pair_group.txt", "w", encoding="utf-8") as f:
        f.write(f"# gen_srp_group.py 生成(安全素数 3072-bit; "
                f"srp6a.c 与 srp_ref.py 同源)\nN_HEX = \"{n_hex}\"\nG = {g}\n")
    cmd_check(n_hex, g)


def cmd_check(p_hex: str, g_s: str) -> None:
    p = int(p_hex, 16)
    g = int(g_s)
    assert p.bit_length() == 3072, f"p bitlen {p.bit_length()} != 3072"
    assert is_probable_prime(p), "p is NOT prime"
    assert is_probable_prime((p - 1) // 2), "(p-1)/2 is NOT prime (not safe)"
    assert pow(g, 2, p) != 1 and pow(g, (p - 1) // 2, p) != 1, "g bad"
    # 群自检: g^(ab mod p-1) == (g^a)^b (小指数)
    y = pow(pow(g, 12345, p), 67890, p)
    z = pow(g, 12345 * 67890 % (p - 1), p)
    assert y == z, "group self-test failed"
    print(f"# check OK: 3072-bit safe prime, g={g} is generator")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        cmd_gen()
    elif len(sys.argv) >= 4 and sys.argv[1] == "check":
        cmd_check(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
