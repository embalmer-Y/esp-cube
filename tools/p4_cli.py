#!/usr/bin/env python3
"""p4_cli.py —— P4(ttyACM0) CLI 命令发送(T15 续 证书灌入/配对链验证用)

用法:
  python3 tools/p4_cli.py <命令1> [命令2 ...]        # 逐条发送, 每条等回显
  python3 tools/p4_cli.py --file cmds.txt           # 按行发送(# 注释跳过)
  python3 tools/p4_cli.py --wait 0.5 <命令 ...>     # 每条后等待秒数

仅允许 ttyACM0(P4 专用口); 只写不复位。
"""
import sys
import time

import serial

BAUD = 115200


def main() -> None:
    args = sys.argv[1:]
    wait = 0.8
    if args and args[0] == "--wait":
        wait = float(args[1])
        args = args[2:]
    if args and args[0] == "--file":
        with open(args[1], encoding="utf-8") as f:
            cmds = [l.rstrip("\n") for l in f
                    if l.strip() and not l.startswith("#")]
        args = cmds
    if not args:
        raise SystemExit(__doc__)
    with serial.Serial("/dev/ttyACM0", BAUD, timeout=0.6) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        for cmd in args:
            ser.write(cmd.encode() + b"\n")
            deadline = time.time() + max(wait, 0.4)
            while time.time() < deadline:
                data = ser.read(2048)
                if data:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    deadline = time.time() + 0.25   # 持续有输出则续窗
            print(flush=True)


if __name__ == "__main__":
    main()
