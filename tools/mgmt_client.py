#!/usr/bin/env python3
"""mgmt_client.py —— T17 管理通道板验客户端(mTLS REQUIRED 语义验证)

用法: python3 tools/mgmt_client.py <设备IP> [--no-cert]
验证(docs/18 T17):
  - 无客户端证书 → 401(管理端点拒)
  - 有客户端证书(CA 签发) → 凭据写/读/轮换/版本冲突 412/删除
  - 抓包非明文: HTTPS 传输(由 -s 抓包模式配合外部 tcpdump 验证)
"""
import base64
import http.client
import json
import socket
import ssl
import sys
from pathlib import Path

CERTS = Path(__file__).parent / "certs"


def conn(host, mtls=True, timeout=30):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.check_hostname = False
    ctx.load_verify_locations(str(CERTS / "ca.crt"))
    if mtls:
        ctx.load_cert_chain(str(CERTS / "client.crt"),
                            str(CERTS / "client.key"))
    c = http.client.HTTPSConnection(host, 443, context=ctx, timeout=timeout)
    return c


def req(host, method, path, body=None, mtls=True):
    c = conn(host, mtls=mtls)
    payload = json.dumps(body) if body is not None else None
    headers = {"Content-Type": "application/json"} if payload else {}
    c.request(method, path, body=payload, headers=headers)
    r = c.getresponse()
    data = r.read().decode()
    c.close()
    return r.status, json.loads(data) if data else {}


def main() -> None:
    host = sys.argv[1]
    no_cert = "--no-cert" in sys.argv
    if no_cert:
        st, r = req(host, "GET", "/v1/mgmt/creds", mtls=False)
        print(f"[no-cert GET creds] {st} {r} (期望 401)")
        sys.exit(0 if st == 401 else 1)

    key_hex = b"sk-boardtest-9876543210abcdef".hex()
    # 1) 新建(expected=0)
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": 0})
    print(f"[create] {st} {r} (期望 200 ver=1)")
    assert st == 200 and r.get("ver") == 1, "create failed"
    # 2) 并发冲突: expected=0 对已存在 → 412+当前版本
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": 0})
    print(f"[conflict] {st} {r} (期望 412 ver=1)")
    assert st == 412 and r.get("ver") == 1, "conflict semantics failed"
    # 3) 陈旧 expected → 412
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": 7})
    print(f"[stale] {st} {r} (期望 412 ver=1)")
    assert st == 412 and r.get("ver") == 1, "stale conflict failed"
    # 4) 正确轮换 → ver=2
    rot_hex = b"sk-rotated-fedcba0987654321".hex()
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": rot_hex, "expected": 1})
    print(f"[rotate] {st} {r} (期望 200 ver=2)")
    assert st == 200 and r.get("ver") == 2, "rotate failed"
    # 5) list(名字+版本, 无值)
    st, r = req(host, "GET", "/v1/mgmt/creds")
    print(f"[list] {st} {r}")
    assert st == 200 and any(c["name"] == "llm.api_key" and c["ver"] == 2
                             for c in r.get("creds", [])), "list failed"
    # 6) settings 版本化 blob
    st, r = req(host, "PUT", "/v1/mgmt/settings",
                {"value": b'{"vol":35,"tts":"tongtong"}'.hex(),
                 "expected": 0})
    print(f"[settings put] {st} {r} (期望 200)")
    assert st == 200, "settings put failed"
    st, r = req(host, "GET", "/v1/mgmt/settings")
    val = bytes.fromhex(r["value"]).decode() if r.get("value") else ""
    print(f"[settings get] {st} ver={r.get('ver')} value={val}")
    assert st == 200 and '"vol":35' in val, "settings get failed"
    # 7) 删除(陈旧 expected → 412; 正确 → 200; 再删 400 notfound)
    st, r = req(host, "DELETE", "/v1/mgmt/creds/llm.api_key",
                {"expected": 1})
    print(f"[del stale] {st} {r} (期望 412)")
    assert st == 412, "delete stale failed"
    st, r = req(host, "DELETE", "/v1/mgmt/creds/llm.api_key",
                {"expected": 2})
    print(f"[del] {st} {r} (期望 200)")
    assert st == 200, "delete failed"
    print("MGMT-ALL-OK")


if __name__ == "__main__":
    main()
