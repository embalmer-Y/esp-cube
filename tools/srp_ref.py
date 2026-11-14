#!/usr/bin/env python3
"""srp_ref.py —— SRP-6a 纯 Python 参考实现(T15 宿主差分测试基准)

协议档必须与 firmware/components/pair/src/srp6a.h 头注一致:
  群 = tools/pair_group.txt(N 3072-bit 安全素数, g 生成元)
  k=3; H=SHA-256; K=H(S); x=H(s|H(user:pin))
  M1=H(H(N)^H(g)|H(user)|s|A|B|K); M2=H(A|M1|K)
  A/B=384B 大端定长; salt=16B

模式:
  selftest —— 参考实现自身往返自检
  vectors  —— DRBG 确定性交换 → 宿主测试黄金向量(C 数组, 打印)
宿主测试的 rng 端口按本文件 DRBG 字节流喂(salt→b→a 顺序), C 与
Python 独立实现必须产出完全一致的 B/A/K/M1/M2。
"""
import hashlib
import sys
from pathlib import Path

GROUP_FILE = Path(__file__).parent / "pair_group.txt"


def load_group() -> tuple[int, int]:
    ns = {}
    exec(GROUP_FILE.read_text(encoding="utf-8"), ns)
    return int(ns["N_HEX"], 16), int(ns["G"])


N, G = load_group()
NN = N.bit_length() // 8                    # 384(A/B 线格式)
PRIV = 32                                    # 私随机指数(srp6a.h SRP_PRIV_LEN)


class DRBG:
    """确定性字节流(仅黄金向量生成用, 非密码学随机源)"""

    def __init__(self, seed: bytes):
        self.seed = seed
        self.counter = 0
        self.buf = b""

    def read(self, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            self.buf += hashlib.sha512(
                self.seed + self.counter.to_bytes(8, "big")).digest()
            self.counter += 1
            take = self.buf[: n - len(out)]
            out += take
            self.buf = self.buf[len(take):]
        return bytes(out)


def H(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def pad(b: int) -> bytes:
    return b.to_bytes(NN, "big")


def unpadded(b: bytes) -> int:
    return int.from_bytes(b, "big")


def srp_x(user: str, pin: str, salt: bytes) -> bytes:
    inner = H(f"{user}:{pin}".encode())
    return H(salt + inner)


def m1_of(user: str, salt: bytes, A: bytes, B: bytes, K: bytes) -> bytes:
    hn = H(pad(N))
    hg = H(G.to_bytes((G.bit_length() + 7) // 8, "big"))
    hu = H(user.encode())
    x = bytes(a ^ b for a, b in zip(hn, hg))
    return H(x + hu + salt + A + B + K)


def m2_of(A: bytes, m1: bytes, K: bytes) -> bytes:
    return H(A + m1 + K)


def mod_mul_add3(v: int) -> int:
    return (3 * v) % N


def server_flow(user: str, pin: str, rng: DRBG):
    salt = rng.read(16)
    b = rng.read(PRIV)
    x = unpadded(srp_x(user, pin, salt))
    v = pow(G, x, N)
    B = (3 * v + pow(G, unpadded(b), N)) % N
    return salt, b, v, B


def client_flow(user: str, pin: str, salt: bytes, B: int, rng: DRBG):
    a = rng.read(PRIV)
    x = unpadded(srp_x(user, pin, salt))
    A = pow(G, unpadded(a), N)
    u = unpadded(H(pad(A) + pad(B)))
    base = (B - 3 * pow(G, x, N)) % N
    e = a_int(a) + u * x
    S = pow(base, e, N)
    K = H(pad(S))
    return a, A, u, K


def a_int(a: bytes) -> int:
    return unpadded(a)


def server_key(v: int, B: int, A: int, b: bytes) -> bytes:
    u = unpadded(H(pad(A) + pad(B)))
    S = pow(A * pow(v, u, N) % N, unpadded(b), N)
    return H(pad(S))


def c_array(name: str, data: bytes, per_line: int = 16) -> str:
    hexs = data.hex()
    rows = [", ".join(f"0x{hexs[i:i+2]}" for i in
                      range(off, min(off + per_line * 2, len(hexs)), 2))
            for off in range(0, len(hexs), per_line * 2)]
    body = ",\n    ".join(rows)
    return f"static const uint8_t {name}[] = {{\n    {body},\n}};"


def selftest() -> None:
    rng = DRBG(b"espcube srp selftest")
    user, pin = "espcube", "421908"
    salt, b, v, B = server_flow(user, pin, rng)
    _, A, u, Kc = client_flow(user, pin, salt, B, rng)
    Ks = server_key(v, B, A, b)
    assert Kc == Ks, "K mismatch"
    m1 = m1_of(user, salt, pad(A), pad(B), Kc)
    m2 = m2_of(pad(A), m1, Kc)
    # 负例: 错 PIN → K 不同 → M1 不同
    salt2, b2, v2, B2 = server_flow(user, "000000", DRBG(b"neg"))
    _, A2, _, K2 = client_flow(user, "999999", salt2, B2, DRBG(b"neg2"))
    assert K2 != server_key(v2, B2, A2, b2), "wrong pin must diverge"
    print(f"selftest OK: K={Kc.hex()[:16]}... u={u} "
          f"M1={m1.hex()[:16]}... M2={m2.hex()[:16]}...")


def vectors() -> None:
    seed = b"espcube srp vector v1"   # rng 消费序: salt16 -> b32 -> a32
    rng = DRBG(seed)
    user, pin = "espcube", "518207"
    salt, b, v, B = server_flow(user, pin, rng)
    _, A, u, Kc = client_flow(user, pin, salt, B, rng)
    Ks = server_key(v, B, A, b)
    assert Kc == Ks
    m1 = m1_of(user, salt, pad(A), pad(B), Kc)
    m2 = m2_of(pad(A), m1, Kc)
    print("/* srp_ref.py 黄金向量(DRBG seed=espcube srp vector v1; "
          "rng 消费序: salt16 -> b384 -> a384) */")
    print(f"/* pin={pin} user={user} */")
    # rng 字节流独立导出: 测试端口按序喂 salt/b/a 三段
    rng2 = DRBG(seed)
    stream = rng2.read(16) + rng2.read(PRIV) + rng2.read(PRIV)
    print(c_array("V_RNG_STREAM", stream))
    print(c_array("V_SALT", salt))
    print(c_array("V_B", pad(B)))
    print(c_array("V_A", pad(A)))
    print(c_array("V_K", Kc))
    print(c_array("V_M1", m1))
    print(c_array("V_M2", m2))
    print(f"/* 附加: modexp 黄金 g^x (x=DRBG 32B) 两条 */")
    for i, tag in enumerate(("XA", "XB")):
        x = DRBG(seed + tag.encode()).read(32)
        gx = pow(G, unpadded(x), N)
        print(c_array(f"V_X_{tag}", x))
        print(c_array(f"V_GX_{tag}", pad(gx)))
    # 3·v + g^b 组合向量(检验 triple/add 链)
    vb = pow(G, unpadded(DRBG(seed + b"V").read(32)), N)
    bb = unpadded(DRBG(seed + b"W").read(PRIV))
    Bv = (3 * vb + pow(G, bb, N)) % N
    print(c_array("V_V", pad(vb)))
    print(c_array("V_GBB", pad(pow(G, bb, N))))
    print(c_array("V_B_COMBO", pad(Bv)))
    print("/* vectors done */")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "selftest"
    if mode == "selftest":
        selftest()
    elif mode == "vectors":
        vectors()
    else:
        print(__doc__)
        sys.exit(2)
