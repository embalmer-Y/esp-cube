#!/usr/bin/env python3
"""serial_log.py —— S31 调试串口日志抓取(ttyUSB0, RTS 复位)

用法:
  python3 tools/serial_log.py [秒数, 默认20] [端口, 默认 /dev/ttyUSB0]

作用: 打开串口后用 RTS 拉低复位板子, 从上电第一条日志开始抓取,
供无 JTAG 场景下验证启动序列/音频心跳/AVRCP 事件。仅操作 ttyUSB0,
严禁指向 /dev/ttyACM0(P4 板专用, 全程不得操作)。
"""
import sys
import time

from serial_excl import open_exclusive  # 独占打开(依赖 pyserial)

def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
    if "ttyACM" in port:
        raise SystemExit("refusing: ttyACM* is the P4 board, must not be touched")
    with open_exclusive(port, 115200, dtr=True, rts=True) as ser:   # dtr/rts 同改造前默认值
        ser.timeout = 0.5
        ser.dtr = False         # 自动复位电路(双三极管)要求 DTR 非有效
        ser.rts = True          # RTS 拉低进入复位
        time.sleep(0.5)         # CP2102N 需要较长的低电平才可靠触发 EN
        ser.rts = False         # 释放复位, 从上电开始抓
        time.sleep(0.2)
        ser.reset_input_buffer()
        deadline = time.time() + secs
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()

if __name__ == "__main__":
    main()
