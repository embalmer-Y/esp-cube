#!/usr/bin/env python3
"""reboot_soak.py —— 冷热重启浸泡观察(T45 首段; docs/18 T45)

用法: python3 tools/reboot_soak.py [次数, 默认 30] [单次窗口秒, 默认 45]
每次: RTS 复位 → 抓启动日志 → 判定(got ip / abort / assert / panic / 挂死)。
产出: /tmp/soak_report.json(逐次记录 + 汇总) + 终端汇总表。
T45 目标 ≥100 次; 本脚本可分批执行(报告文件累加由调用方处理)。
"""
import json
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200


def one_cycle(win_s: float) -> dict:
    rec = {"t": time.strftime("%H:%M:%S")}
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.5)
    ser.rts = False
    ser.reset_input_buffer()
    deadline = time.time() + win_s
    buf = []
    while time.time() < deadline:
        d = ser.read(8192)
        if d:
            buf.append(d.decode("utf-8", "replace"))
            text = "".join(buf)
            if any(k in text for k in ("abort()", "assert failed",
                                       "Guru Meditation")):
                # 捕获现场后再收 2s
                t2 = time.time() + 2
                while time.time() < t2:
                    d = ser.read(8192)
                    if d:
                        buf.append(d.decode("utf-8", "replace"))
                break
            if "got ip" in text:
                # 网络就绪后再收 6s 观察早期稳定性
                t2 = time.time() + 6
                while time.time() < t2:
                    d = ser.read(4096)
                    if d:
                        buf.append(d.decode("utf-8", "replace"))
                break
    ser.close()
    text = "".join(buf).replace("\r", "")
    rec["got_ip"] = "got ip" in text
    rec["abort"] = "abort()" in text
    rec["assert"] = "assert failed" in text
    rec["guru"] = "Guru Meditation" in text
    rec["hang"] = not rec["got_ip"] and not rec["abort"] \
        and not rec["assert"] and not rec["guru"]
    rec["err"] = rec["abort"] or rec["assert"] or rec["guru"] or rec["hang"]
    if rec["err"]:
        rec["tail"] = text[-800:]
    return rec


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    win = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
    recs = []
    fails = 0
    for i in range(1, n + 1):
        rec = one_cycle(win)
        recs.append(rec)
        mark = "OK " if not rec["err"] else "ERR"
        fails += 1 if rec["err"] else 0
        print(f"[{i}/{n}] {mark} got_ip={rec['got_ip']} "
              f"abort={rec['abort']} assert={rec['assert']} "
              f"guru={rec['guru']} hang={rec['hang']}", flush=True)
        if rec["err"]:
            print("  tail:", rec.get("tail", "")[-300:].replace("\n", " | "))
    report = {"cycles": n, "fails": fails,
              "note": "T45 重启浸泡(分批); 开机电台关(冻结规避)",
              "records": recs}
    with open("/tmp/soak_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    print(f"=== soak: {n - fails}/{n} clean, {fails} 异常; "
          f"报告 /tmp/soak_report.json ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
