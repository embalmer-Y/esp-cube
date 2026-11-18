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


def run_flow(host: str) -> bool:
    """mTLS 管理全套断言流; 返回是否全绿(appsim 编排复用)。
    2026-11-18 修正: 版本断言改相对基准(设备语义=删除后墓碑递增防 ABA,
    ver 不从 1 起; 幂等前置先清残留——App 真实姿势=永远先读后写)。"""
    key_hex = b"sk-boardtest-9876543210abcdef".hex()
    # 0) 幂等前置: 残留则按当前版本删除(顺带验证按版本删)
    st, r = req(host, "GET", "/v1/mgmt/creds")
    assert st == 200, f"list failed {st}"
    stale = next((c for c in r.get("creds", []) if c["name"] == "llm.api_key"), None)
    if stale:
        st, r = req(host, "DELETE", "/v1/mgmt/creds/llm.api_key",
                    {"expected": stale["ver"]})
        print(f"[cleanup] del ver={stale['ver']} -> {st} (期望 200)")
        assert st == 200, "cleanup delete failed"
    # 1) 新建(expected=0; ver 为墓碑后基准 v0, 不假设具体值)
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": 0})
    v0 = r.get("ver", -1)
    print(f"[create] {st} ver={v0} (期望 200 ver>=1)")
    assert st == 200 and v0 >= 1, "create failed"
    # 2) 并发冲突: expected=0 对已存在 → 412+当前版本
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": 0})
    print(f"[conflict] {st} {r} (期望 412 ver={v0})")
    assert st == 412 and r.get("ver") == v0, "conflict semantics failed"
    # 3) 陈旧 expected → 412
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": key_hex, "expected": v0 + 5})
    print(f"[stale] {st} {r} (期望 412 ver={v0})")
    assert st == 412 and r.get("ver") == v0, "stale conflict failed"
    # 4) 正确轮换 → v0+1
    rot_hex = b"sk-rotated-fedcba0987654321".hex()
    st, r = req(host, "PUT", "/v1/mgmt/creds/llm.api_key",
                {"value": rot_hex, "expected": v0})
    print(f"[rotate] {st} {r} (期望 200 ver={v0 + 1})")
    assert st == 200 and r.get("ver") == v0 + 1, "rotate failed"
    # 5) list(名字+版本, 无值)
    st, r = req(host, "GET", "/v1/mgmt/creds")
    print(f"[list] {st} {r}")
    assert st == 200 and any(c["name"] == "llm.api_key" and c["ver"] == v0 + 1
                             for c in r.get("creds", [])), "list failed"
    # 6) settings: 常驻 blob(承载 llmprofile, T25 接线)——DELETE 被设备
    #    拒绝(400)是保护语义, 不可删。无损测法: 读原值→原值回写(expected=
    #    原ver)→断言 ver+1 且值不变。
    st, r = req(host, "GET", "/v1/mgmt/settings")
    assert st == 200, "settings get failed"
    sv, sval = r.get("ver", 0), r.get("value")
    st, r = req(host, "DELETE", "/v1/mgmt/settings", {"expected": sv})
    print(f"[settings del-reject] {st} (期望 400, 常驻保护)")
    assert st == 400, "settings delete should be rejected"
    st, r = req(host, "PUT", "/v1/mgmt/settings",
                {"value": sval, "expected": sv})
    # credstore 版本=全局单调计数(2026-11-19 实测: settings 从 6 跳 14,
    # 因 llm.api_key 先写到 13), 断言只要求递增+后续 GET 值不变
    print(f"[settings rewrite] {st} {r} (期望 200 ver>{sv})")
    assert st == 200 and r.get("ver", 0) > sv, "settings rewrite failed"
    st, r = req(host, "GET", "/v1/mgmt/settings")
    assert st == 200 and r.get("value") == sval, "settings value changed!"
    print(f"[settings verify] ver={r.get('ver')} 值不变 ✓")
    # 7) 删除(陈旧 expected → 412; 正确 → 200)
    st, r = req(host, "DELETE", "/v1/mgmt/creds/llm.api_key",
                {"expected": v0})
    print(f"[del stale] {st} {r} (期望 412)")
    assert st == 412, "delete stale failed"
    st, r = req(host, "DELETE", "/v1/mgmt/creds/llm.api_key",
                {"expected": v0 + 1})
    print(f"[del] {st} {r} (期望 200)")
    assert st == 200, "delete failed"
    print("MGMT-ALL-OK")
    return True


def main() -> None:
    host = sys.argv[1]
    no_cert = "--no-cert" in sys.argv
    if no_cert:
        st, r = req(host, "GET", "/v1/mgmt/creds", mtls=False)
        print(f"[no-cert GET creds] {st} {r} (期望 401)")
        sys.exit(0 if st == 401 else 1)
    try:
        ok = run_flow(host)
    except AssertionError as e:
        print(f"MGMT-FAIL: {e}")
        sys.exit(1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
