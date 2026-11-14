#!/usr/bin/env python3
"""pair_client.py —— 配对测试客户端(T15 续真机链; SRP-6a 档=srp_ref.py)

用法: python3 tools/pair_client.py <设备IP> [PIN] [--badpin]
  无 PIN  = 正常全链(step1→step3→bind)
  --badpin= 故意错 PIN ×5, 期望第 5 次后 locked

档与固件 components/pair/src/srp6a.h 注释一致(k=3/H=SHA256/K=H(S))。
证书校验: 用 CA 验签但不校验主机名(测试客户端; 正式 App T16 按 SAN)。
"""
import base64
import http.client
import json
import ssl
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import srp_ref  # noqa: E402

CERTS = Path(__file__).parent / "certs"


def http_post(host, path, body=None, timeout=120):
    ctx = ssl.create_default_context(cafile=str(CERTS / "ca.crt"))
    ctx.check_hostname = False            # 测试客户端(见文件头)
    conn = http.client.HTTPSConnection(host, 443, context=ctx,
                                       timeout=timeout)
    payload = json.dumps(body) if body is not None else "{}"
    conn.request("POST", path, body=payload,
                 headers={"Content-Type": "application/json"})
    resp = conn.getresponse()
    data = resp.read().decode()
    conn.close()
    return resp.status, json.loads(data) if data else {}


def http_get(host, path, timeout=120):
    ctx = ssl.create_default_context(cafile=str(CERTS / "ca.crt"))
    ctx.check_hostname = False
    conn = http.client.HTTPSConnection(host, 443, context=ctx,
                                       timeout=timeout)
    conn.request("GET", path)
    resp = conn.getresponse()
    data = resp.read().decode()
    conn.close()
    return resp.status, json.loads(data) if data else {}


def client_pubkey_der() -> bytes:
    """生成测试客户端 P-256 密钥对, 返回 SEC1 未压缩公钥(65B)"""
    priv = subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout"],
        capture_output=True, check=True).stdout
    pub = subprocess.run(
        ["openssl", "ec", "-pubout", "-outform", "DER"],
        input=priv, capture_output=True, check=True).stdout
    # SPKI 尾部 65B = SEC1 未压缩点(04|X|Y)
    return pub[-65:]


def full_pair(host, pin: str) -> int:
    st, dev = http_get(host, "/v1/device")
    print(f"[device] {st} {dev}")
    salt_hex, B_hex = None, None
    # 用参考实现的 client_flow 需要 bytes 形参——直接组装 SRP 消息
    import secrets
    user = "espcube"
    st, r1 = http_post(host, "/v1/pair/step1")
    assert st == 200, f"step1 {st} {r1}"
    salt = bytes.fromhex(r1["salt"])
    B_int = int.from_bytes(bytes.fromhex(r1["B"]), "big")
    print(f"[step1] salt={r1['salt'][:12]}.. B ok")

    a = secrets.token_bytes(32)
    x = srp_ref.unpadded(srp_ref.srp_x(user, pin, salt))
    A = pow(srp_ref.G, srp_ref.unpadded(a), srp_ref.N)
    u = srp_ref.unpadded(srp_ref.H(srp_ref.pad(A) + srp_ref.pad(B_int)))
    base = (B_int - 3 * pow(srp_ref.G, x, srp_ref.N)) % srp_ref.N
    e = srp_ref.unpadded(a) + u * x
    S = pow(base, e, srp_ref.N)
    K = srp_ref.H(srp_ref.pad(S))
    m1 = srp_ref.m1_of(user, salt, srp_ref.pad(A), srp_ref.pad(B_int), K)

    st, r2 = http_post(host, "/v1/pair/step2",
                       {"A": srp_ref.pad(A).hex()})
    assert st == 200, f"step2 {st} {r2}"
    print("[step2] A accepted")
    st, r3 = http_post(host, "/v1/pair/step3", {"m1": m1.hex()})
    assert st == 200, f"step3 {st} {r3}"
    m2 = bytes.fromhex(r3["m2"])
    expect_m2 = srp_ref.m2_of(srp_ref.pad(A), m1, K)
    assert m2 == expect_m2, "M2 mismatch!"
    print("[step3] M1 ok, M2 verified")
    pk = client_pubkey_der()
    st, r4 = http_post(host, "/v1/pair/bind",
                       {"pubkey": pk.hex()})
    assert st == 200, f"bind {st} {r4}"
    print(f"[bind] client_id={r4['client_id']}")
    return r4["client_id"]


def badpin_flood(host) -> None:
    import secrets
    user = "espcube"
    for attempt in range(1, 6):
        st, r1 = http_post(host, "/v1/pair/step1")
        assert st == 200, f"step1 #{attempt}: {st} {r1}"
        salt = bytes.fromhex(r1["salt"])
        B_int = int.from_bytes(bytes.fromhex(r1["B"]), "big")
        a = secrets.token_bytes(32)
        x = srp_ref.unpadded(srp_ref.srp_x(user, "000000", salt))
        A = pow(srp_ref.G, srp_ref.unpadded(a), srp_ref.N)
        u = srp_ref.unpadded(srp_ref.H(srp_ref.pad(A) + srp_ref.pad(B_int)))
        base = (B_int - 3 * pow(srp_ref.G, x, srp_ref.N)) % srp_ref.N
        S = pow(base, srp_ref.unpadded(a) + u * x, srp_ref.N)
        K = srp_ref.H(srp_ref.pad(S))
        m1 = srp_ref.m1_of(user, salt, srp_ref.pad(A), srp_ref.pad(B_int), K)
        http_post(host, "/v1/pair/step2", {"A": srp_ref.pad(A).hex()})
        st, r3 = http_post(host, "/v1/pair/step3", {"m1": m1.hex()})
        print(f"[flood #{attempt}] {st} {r3}")
        if r3.get("error") == "locked":
            st, r1 = http_post(host, "/v1/pair/step1")
            print(f"[post-locked step1] {st} {r1} (期望 403 window)")
            return
    print("!! 5 次未锁定(异常)")


def main() -> None:
    host = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else None
    if mode == "--badpin":
        badpin_flood(host)
    else:
        pin = mode if mode else None
        assert pin, "需要 PIN(先在设备 CLI pairwin)"
        full_pair(host, pin)


if __name__ == "__main__":
    main()
