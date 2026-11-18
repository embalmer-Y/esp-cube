#!/usr/bin/env python3
"""appsim.py —— Android App v1(配置版) Linux 模拟器(P3b 前置)

用户指令(2026-11-18): App 通过网络通信, 先在 Linux 用脚本把手机端全部
功能调通, 最后再开发 App 本体。本脚本 = App 的网络行为面单一入口:

  python3 tools/appsim.py discover [IP]   发现+可达性(TCP :443 + /v1/device)
  python3 tools/appsim.py pair [IP]       自动配对(串口 pairwin 抓 PIN → SRP-6a 全链)
  python3 tools/appsim.py mgmt [IP]       mTLS 管理全套(凭据 CRUD/412/设置)
  python3 tools/appsim.py all [IP]        一键: pairserve 保活→discover→pair→mgmt

复用板验家底: pair_client(SRP)/mgmt_client(mTLS 断言流)/serial_excl(独占
不复位)。身份模型(现状, M1 口径): TLS 层 = tools/certs/client.crt(设备
NVS pairc 区 CA 校验, gen_pair_certs.sh 同源); pair 层 = bind 登记随机
公钥(App v1 应改绑证书公钥, docs/17 §9.2; 设备侧 mgmt 授权只看 CA 链)。

板卡前置: /dev/ttyACM0 可独占(自动起 pairserve 用); 默认 IP 192.168.2.120
(无 mDNS, docs/17 §9.1 "或等价方式"的现状=IP 直连, App 侧发现入口待 P3b)。
"""
import re
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import serial  # noqa: E402

import mgmt_client  # noqa: E402
import pair_client  # noqa: E402

PORT = "/dev/ttyACM0"
BAUD = 115200
DEFAULT_HOST = "192.168.2.120"


# ---- 串口原语(命令→回显捕获) ----
# 坑(2026-11-18 实测): 本板 CH343 上 serial_excl.open_exclusive 的
# dtr=False/rts=False 预置开口会在开 port 瞬间触发 EN 复位脉冲(抓到
# rst:0x1 POWERON 完整 ROM boot ×2); 裸 serial.Serial(DTR/RTS 默认高)
# 多次实证不复位。独占性靠"同一时间只一个进程"纪律(本工具单线程序)。
def cli_cmd(cmd: str, wait_re: str = None, timeout: float = 12.0):
    """发送 CLI 命令, 返回全部回显文本; wait_re 命中提前返回"""
    ser = serial.Serial(PORT, BAUD, timeout=1)
    try:
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            d = ser.read(256)
            if d:
                buf += d
                if wait_re and re.search(wait_re, buf.decode("utf-8", "replace")):
                    break
        return buf.decode("utf-8", "replace")
    finally:
        ser.close()


# ---- 发现+可达性(FR-03/05 App 侧) ----
def discover(host: str) -> bool:
    print(f"[discover] {host}")
    try:
        with socket.create_connection((host, 443), timeout=3):
            print("[discover] TCP :443 可达")
    except OSError as e:
        print(f"[discover] 不可达: {e}(设备未起 pairserve? all 子命令会自动拉起)")
        return False
    st, dev = pair_client.http_get(host, "/v1/device")
    print(f"[discover] GET /v1/device -> {st} {dev}")
    return st == 200


def ensure_server(host: str) -> bool:
    """pairserve 保活: 443 不通则串口拉起(boot 不自启, 现状)"""
    try:
        with socket.create_connection((host, 443), timeout=2):
            return True
    except OSError:
        pass
    print("[ensure] 443 不通, 串口拉起 pairserve ...")
    out = cli_cmd("pairserve", wait_re=r"started :443|start 失败", timeout=15)
    m = re.search(r"pair server: (.+)", out)
    print(f"[ensure] {m.group(1) if m else out.strip()[-120:]}")
    for _ in range(5):                     # TLS 监听就绪再探
        try:
            with socket.create_connection((host, 443), timeout=2):
                return True
        except OSError:
            time.sleep(1)
    return False


# ---- 配对(FR-02; App 视角: 用户在设备物理开窗, 这里自动化为串口开窗) ----
def do_pair(host: str) -> bool:
    print("[pair] 串口开窗(120s)...")
    out = cli_cmd("pairwin", wait_re=r"PIN: \d{6}|pairwin: 失败", timeout=10)
    m = re.search(r"PIN: (\d{6})", out)
    if not m:
        print(f"[pair] 开窗失败: {out.strip()[-120:]}")
        return False
    pin = m.group(1)
    print(f"[pair] PIN={pin} → SRP-6a 全链")
    try:
        cid = pair_client.full_pair(host, pin)
    except AssertionError as e:
        print(f"[pair] 失败: {e}")
        return False
    print(f"[pair] 绑定成功 client_id={cid}")
    return True


SUBCMDS = {
    "discover": lambda h: discover(h),
    "pair": lambda h: do_pair(h),
    "mgmt": lambda h: mgmt_client.run_flow(h),
}


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in (*SUBCMDS, "all"):
        print(__doc__)
        sys.exit(2)
    host = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_HOST
    steps = ["discover", "pair", "mgmt"] if sys.argv[1] == "all" else [sys.argv[1]]
    if "all" in sys.argv[1:] or sys.argv[1] == "all":
        if not ensure_server(host):
            print("[all] pairserve 拉起失败, 终止")
            sys.exit(1)
    ok = True
    for s in steps:
        print(f"===== {s} =====")
        try:
            ok = SUBCMDS[s](host) and ok
        except Exception as e:                     # 编排层不吞栈但续跑后续步骤
            print(f"[{s}] 异常: {e!r}")
            ok = False
        if not ok and s != "discover":
            break
    print("APPSIM:", "ALL-OK" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
