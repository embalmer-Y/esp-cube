#!/usr/bin/env python3
"""liveness_watch.py —— 设备存活旁证探测器（freeze-and-face-blackscreen-fix 任务 13.4）。

每 2 s 执行一次 `ping -c1 -W1 <设备IP>`。每次探测向 /tmp/liveness.jsonl 追加一行 JSON 并立即 flush、fsync：
  {"ts": "2026-11-16T21:55:12.345+08:00", "seq": 1, "target": "192.168.2.120", "status": "ok", "rtt_ms": 0.412}
  status 取值：ok 收到应答（rtt_ms 为往返毫秒）；timeout 无应答（rtt_ms 为 null）；
              error ping 本身失败（detail 给出原因）。

--mgmt（可选）：每次探测后，用 tools/certs 中的 mTLS 客户端证书（与 tools/mgmt_client.py 相同的 CA、
证书与 TLS 1.2 参数）发起 GET https://<设备IP>:443/v1/mgmt/creds，作为第二旁证，结果写入行内 "mgmt" 字段。
该端点只读，只返回凭据名与版本；本工具丢弃响应体，不记录任何值。未出示有效证书时设备返回 401，记为 denied。
本工具不新增端点，也不访问未鉴权端点。每次探测都会与设备做一次 TLS 握手，会给被测设备增加负载。

本工具不打开任何串口（打开 ttyACM 会复位板子，见任务 1.5）。Ctrl-C 或 --max-s 到期后结束，已写入的行均已落盘。

用法:
  python3 tools/liveness_watch.py <设备IP> [--interval 2] [--max-s S] [--mgmt]
  python3 tools/liveness_watch.py --selftest
示例:
  python3 tools/liveness_watch.py 192.168.2.120 --mgmt
"""
import argparse
import functools
import http.client
import ipaddress
import json
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

OUT_PATH = "/tmp/liveness.jsonl"
DEFAULT_INTERVAL_S = 2.0
PING_HARD_TIMEOUT_S = 5.0  # 兜底；ping 自身由 -W1 限制在约 1 s 内
MGMT_PORT = 443
MGMT_PATH = "/v1/mgmt/creds"  # 只读：凭据名与版本，无值
MGMT_TIMEOUT_S = 1.0  # 与 ping 的至多 1 s 合计不超过 2 s 周期
CERTS = Path(__file__).resolve().parent / "certs"
RTT_RE = re.compile(r"time[=<]\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
HOST_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9.\-]{0,252}")


def validate_target(text: str) -> str:
    """接受 IP 字面量或主机名。以 '-' 开头的串会被 ping 当作选项，直接拒绝。"""
    try:
        ipaddress.ip_address(text)
        return text
    except ValueError:
        pass
    if HOST_RE.fullmatch(text):
        return text
    raise argparse.ArgumentTypeError(f"不是 IP 地址或主机名: {text!r}")


def _short(text: str, limit: int = 160) -> str:
    return text if len(text) <= limit else text[:limit] + "..."


def parse_ping_output(stdout: str) -> float | None:
    """从 ping 输出取往返时间（毫秒）；没有 time= 字段时返回 None。"""
    m = RTT_RE.search(stdout)
    return float(m.group(1)) if m else None


def ping_once(target: str) -> dict:
    """执行一次 ping -c1 -W1，返回 status 与 rtt_ms；出错时附 detail。"""
    try:
        proc = subprocess.run(
            ["ping", "-c1", "-W1", target],
            capture_output=True, text=True, timeout=PING_HARD_TIMEOUT_S, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "rtt_ms": None}
    except OSError as exc:
        return {"status": "error", "rtt_ms": None,
                "detail": _short(f"{type(exc).__name__}: {exc}")}
    if proc.returncode == 0:
        rtt = parse_ping_output(proc.stdout)
        if rtt is not None:
            return {"status": "ok", "rtt_ms": rtt}
        return {"status": "error", "rtt_ms": None, "detail": "reply without time= field"}
    if proc.returncode == 1:  # iputils：没有收到应答
        return {"status": "timeout", "rtt_ms": None}
    first = (proc.stderr or proc.stdout).strip().splitlines()[:1]
    return {"status": "error", "rtt_ms": None,
            "detail": _short(f"ping rc={proc.returncode}: {first[0] if first else ''}")}


def make_mgmt_context() -> ssl.SSLContext:
    """TLS 1.2 客户端上下文：CA 为 tools/certs/ca.crt，客户端证书与私钥为 tools/certs/client.{crt,key}。
    参数与 tools/mgmt_client.py 一致：校验服务端证书链，不校验主机名。"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.check_hostname = False
    ctx.load_verify_locations(str(CERTS / "ca.crt"))
    ctx.load_cert_chain(str(CERTS / "client.crt"), str(CERTS / "client.key"))
    return ctx


def mgmt_probe(target: str, ctx: ssl.SSLContext) -> dict:
    """只读 GET /v1/mgmt/creds（mTLS）。读完即丢弃响应体，只保留状态码与耗时。"""
    t0 = time.monotonic()
    conn = http.client.HTTPSConnection(target, MGMT_PORT, context=ctx, timeout=MGMT_TIMEOUT_S)
    try:
        conn.request("GET", MGMT_PATH)
        resp = conn.getresponse()
        resp.read()
        code = resp.status
    except TimeoutError:
        return {"status": "timeout", "http": None, "ms": None}
    except (OSError, http.client.HTTPException) as exc:
        return {"status": "error", "http": None, "ms": None,
                "detail": _short(f"{type(exc).__name__}: {exc}")}
    finally:
        conn.close()
    ms = round((time.monotonic() - t0) * 1000, 1)
    if code == 200:
        return {"status": "ok", "http": code, "ms": ms}
    if code in (401, 403):
        return {"status": "denied", "http": code, "ms": ms}
    return {"status": "error", "http": code, "ms": ms, "detail": f"http {code}"}


def probe_record(seq: int, target: str, ping_fn, mgmt_fn=None) -> dict:
    """组装一行记录。ts 取探测开始时刻（本机时区，毫秒精度）。"""
    rec = {"ts": datetime.now().astimezone().isoformat(timespec="milliseconds"),
           "seq": seq, "target": target}
    rec.update(ping_fn(target))
    if mgmt_fn is not None:
        rec["mgmt"] = mgmt_fn(target)
    return rec


def write_record(fh, rec: dict) -> None:
    """追加一行 JSON，随即 flush 与 fsync，使每次探测在进程异常退出后仍然保留。"""
    fh.write(json.dumps(rec) + "\n")
    fh.flush()
    os.fsync(fh.fileno())


def _sleep_until(t: float) -> None:
    delay = t - time.monotonic()
    if delay > 0:
        time.sleep(delay)


def watch(target, fh, ping_fn, mgmt_fn=None, interval=DEFAULT_INTERVAL_S,
          max_s=0.0, max_probes=None) -> int:
    """按固定节拍（起点相隔 interval 秒）探测，直到 Ctrl-C、max_s 到期或写满 max_probes 行。
    探测耗时超过节拍时跳过已错过的节拍，不补发。返回写入的行数。"""
    if interval <= 0:
        raise ValueError("interval 必须大于 0")
    start = time.monotonic()
    deadline = start + max_s if max_s > 0 else None
    next_t = start
    count = 0
    try:
        while max_probes is None or count < max_probes:
            if deadline is not None and next_t >= deadline:
                _sleep_until(deadline)  # 跑满 max_s 的墙钟时长后结束
                break
            _sleep_until(next_t)
            write_record(fh, probe_record(count + 1, target, ping_fn, mgmt_fn))
            count += 1
            next_t += interval
            now = time.monotonic()
            while next_t <= now:
                next_t += interval
    except KeyboardInterrupt:
        pass  # Ctrl-C 是正常的结束方式；每行已 flush
    return count


FAKE_PING = [
    {"status": "ok", "rtt_ms": 0.412},
    {"status": "timeout", "rtt_ms": None},
    {"status": "ok", "rtt_ms": 1.25},
    {"status": "error", "rtt_ms": None, "detail": "ping rc=2: unknown host"},
]
FAKE_MGMT = [
    {"status": "ok", "http": 200, "ms": 12.5},
    {"status": "timeout", "http": None, "ms": None},
    {"status": "denied", "http": 401, "ms": 3.1},
    {"status": "error", "http": None, "ms": None,
     "detail": "ConnectionRefusedError: [Errno 111] Connection refused"},
]
PING_TEXT_CASES = [
    ("64 bytes from 192.0.2.10: icmp_seq=1 ttl=64 time=0.412 ms", 0.412),
    ("64 bytes from 127.0.0.1: icmp_seq=1 ttl=64 time=0.031 ms", 0.031),
    ("1 packets transmitted, 0 received, 100% packet loss", None),
]
BAD_TARGETS = ["-f", "--help", "", "bad target"]
ST_TARGET = "192.0.2.10"  # TEST-NET-1，仅供自检使用；自检不发包


def record_problems(rec: dict, seq: int, target: str) -> list[str]:
    """检查一行记录的格式，返回问题列表（空表示合格）。"""
    problems = []
    try:
        if datetime.fromisoformat(rec["ts"]).tzinfo is None:
            problems.append("ts 无时区")
    except (KeyError, TypeError, ValueError):
        problems.append("ts 缺失或不是 ISO 8601")
    if rec.get("seq") != seq:
        problems.append(f"seq={rec.get('seq')} 应为 {seq}")
    if rec.get("target") != target:
        problems.append("target 不一致")
    status = rec.get("status")
    rtt = rec.get("rtt_ms")
    if status not in ("ok", "timeout", "error"):
        problems.append(f"status 非法: {status!r}")
    elif status == "ok" and not (isinstance(rtt, float) and rtt >= 0):
        problems.append("ok 行缺少非负 rtt_ms")
    elif status != "ok" and rtt is not None:
        problems.append("非 ok 行的 rtt_ms 应为 null")
    if "mgmt" in rec:
        m = rec["mgmt"]
        if not isinstance(m, dict) or m.get("status") not in ("ok", "timeout", "error", "denied"):
            problems.append("mgmt 字段非法")
    return problems


def selftest() -> int:
    """不联网的格式自检：解析样例、目标校验、mTLS 证书加载，以及用假 ping/假管理通道跑一次真实的 watch()。"""
    results: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, ok, detail))

    for text, want in PING_TEXT_CASES:
        got = parse_ping_output(text)
        check(f"parse_ping_output 期望 {want}", got == want, f"得到 {got}")
    for bad in BAD_TARGETS:
        try:
            validate_target(bad)
            check(f"拒绝目标 {bad!r}", False, "未被拒绝")
        except argparse.ArgumentTypeError:
            check(f"拒绝目标 {bad!r}", True)
    try:
        make_mgmt_context()
        check("mTLS 证书加载（tools/certs）", True)
    except (OSError, ssl.SSLError) as exc:
        check("mTLS 证书加载（tools/certs）", False, type(exc).__name__)

    ping_it = iter(FAKE_PING)
    mgmt_it = iter(FAKE_MGMT)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "selftest.jsonl"
        with path.open("w", encoding="utf-8") as fh:
            n = watch(ST_TARGET, fh, lambda _t: dict(next(ping_it)),
                      lambda _t: dict(next(mgmt_it)), interval=0.01,
                      max_probes=len(FAKE_PING))
        lines = path.read_text(encoding="utf-8").splitlines()
    print("# liveness_watch --selftest：假 ping/假管理通道，未联网")
    for line in lines:
        print(line)
    check(f"写入 {len(FAKE_PING)} 行", n == len(FAKE_PING) and len(lines) == len(FAKE_PING),
          f"n={n} 行数={len(lines)}")
    recs = [json.loads(line) for line in lines]
    for seq, rec in enumerate(recs, 1):
        problems = record_problems(rec, seq, ST_TARGET)
        check(f"第 {seq} 行格式", not problems, "; ".join(problems))
    check("status 顺序与假 ping 一致",
          [r.get("status") for r in recs] == [p["status"] for p in FAKE_PING])
    check("mgmt status 顺序与假管理通道一致",
          [r.get("mgmt", {}).get("status") for r in recs] == [m["status"] for m in FAKE_MGMT])

    failed = [r for r in results if not r[1]]
    for name, ok, detail in results:
        suffix = f"  ({detail})" if detail and not ok else ""
        print(f"[{'PASS' if ok else 'FAIL'}] {name}{suffix}")
    print(f"selftest: {len(results) - len(failed)}/{len(results)} 项通过")
    return 1 if failed else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="设备存活旁证探测器（任务 13.4）：每 2 s ping 设备，可选 mTLS 管理通道旁证，"
                    "结果追加到 /tmp/liveness.jsonl。",
        epilog="示例: python3 tools/liveness_watch.py 192.168.2.120 --mgmt",
    )
    ap.add_argument("device_ip", nargs="?", type=validate_target, metavar="设备IP",
                    help="设备 IP 或主机名（必填，没有默认值）")
    ap.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_S,
                    help="探测间隔秒数（默认 %(default)s）")
    ap.add_argument("--max-s", type=float, default=0.0,
                    help="最长运行秒数（0 为直到 Ctrl-C）")
    ap.add_argument("--mgmt", action="store_true",
                    help="每次探测后用 mTLS 客户端证书只读请求 /v1/mgmt/creds")
    ap.add_argument("--selftest", action="store_true",
                    help="不联网的格式自检（假 ping、假管理通道）后退出")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.device_ip is None:
        ap.error("需要设备 IP，例如 192.168.2.120")
    if args.interval <= 0:
        ap.error("--interval 必须大于 0")
    if args.max_s < 0:
        ap.error("--max-s 不能为负数")
    if shutil.which("ping") is None:
        ap.error("找不到 ping 命令")

    mgmt_fn = None
    if args.mgmt:
        try:
            ctx = make_mgmt_context()
        except (OSError, ssl.SSLError) as exc:
            ap.error(f"--mgmt 无法加载 tools/certs 中的证书: {type(exc).__name__}")
        mgmt_fn = functools.partial(mgmt_probe, ctx=ctx)

    try:
        fh = open(OUT_PATH, "a", encoding="utf-8")
    except OSError as exc:
        print(f"liveness_watch: 无法打开 {OUT_PATH}: {exc}", file=sys.stderr)
        return 2
    with fh:
        n = watch(args.device_ip, fh, ping_once, mgmt_fn, args.interval, args.max_s)
    print(f"liveness_watch: 已追加 {n} 行到 {OUT_PATH}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
