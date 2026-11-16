#!/usr/bin/env python3
"""p4_boot_capture.py —— P4(ttyACM0) 启动日志抓取(T12/T06 板级验收用)

用法: python3 tools/p4_boot_capture.py [秒数, 默认45] [端口, 默认 /dev/ttyACM0]
仅允许 ttyACM0(P4 专用口); 先尝试 RTS 复位从上电抓全, 再持续读取。
输出落 /tmp/p4_boot.log, 终端回显关键行。
"""
import sys
import time

from serial_excl import open_exclusive  # 独占打开(依赖 pyserial)

BAUD = 115200

def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
    if "ttyACM" not in port:
        raise SystemExit("refusing: 本脚本专用 P4 ttyACM0, 其他口用 serial_log.py")
    with open_exclusive(port, BAUD, dtr=True, rts=True) as ser:   # dtr/rts 同改造前默认值
        ser.timeout = 0.5
        ser.dtr = False
        ser.rts = True          # 拉低复位(同 serial_log.py 双三极管口径)
        time.sleep(0.5)
        ser.rts = False         # 释放复位, 从上电抓
        ser.reset_input_buffer()
        deadline = time.time() + secs
        chunks = []
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                chunks.append(data)
        blob = b"".join(chunks)
    text = blob.decode("utf-8", errors="replace")
    with open("/tmp/p4_boot.log", "w", encoding="utf-8") as f:
        f.write(text)
    keys = ["mem boot", "mem backend", "anomalous", "soul", "wifi", "ip",
            "station", "got ip", "Guru Meditation", "abort", "assert",
            "panic", "Backtrace", "radio", "sntp", "time", "E (", "W ("]
    low = text.lower()
    for line in text.splitlines():
        if any(k in line.lower() for k in keys):
            print(line[:200])
    print(f"--- captured {len(text)} chars -> /tmp/p4_boot.log")

if __name__ == "__main__":
    main()
