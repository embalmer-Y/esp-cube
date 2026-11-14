#!/usr/bin/env python3
"""freeze_watch.py —— 冻结现场捕获器(T45 现场保留; 批次 34 WDT panic 模式)

长时监听 P4 串口:
  - panic/abort/assert/Guru → 抓全现场(含 Backtrace) 落盘并退出 → 符号化
  - 持续静默(≥15s 无输出且此前在运行态) → 判定"静默冻结"(WDT 不可捕获
    =硬件级嫌疑) 落盘退出
用法: python3 tools/freeze_watch.py [最长观察秒, 默认 2700]
输出: /tmp/freeze_watch.log + /tmp/freeze_watch_report.json
"""
import json
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200
PANIC_PAT = ("panic", "abort()", "assert failed", "Guru Meditation",
             "Backtrace", "Task watchdog", "Interrupt wdt")


def main() -> None:
    total = float(sys.argv[1]) if len(sys.argv) > 1 else 2700
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    ser.dtr = False
    ser.rts = False                      # 不复位
    time.sleep(1.0)
    ser.reset_input_buffer()
    start = time.time()
    buf = []
    last_rx = time.time()
    running_seen = False                 # 见过运行态日志(区分启动前静默)
    caught = None
    while time.time() - start < total:
        try:
            d = ser.read(4096)
        except serial.SerialException:
            time.sleep(2)
            try:
                ser.close()
            except Exception:
                pass
            ser = serial.Serial(PORT, BAUD, timeout=0.5)
            ser.dtr = False
            ser.rts = False
            ser.reset_input_buffer()
            continue
        if d:
            chunk = d.decode("utf-8", "replace")
            buf.append(chunk)
            last_rx = time.time()
            low = chunk.lower()
            if any(p.lower() in low for p in
                   ("alive", "mixer: health", "lv_efc", "got ip")):
                running_seen = True
            text = "".join(buf).lower()
            if any(p.lower() in text[-4000:] for p in PANIC_PAT):
                # 抓现场: 再收 5s 后退出
                t2 = time.time() + 5
                while time.time() < t2:
                    d = ser.read(8192)
                    if d:
                        buf.append(d.decode("utf-8", "replace"))
                caught = "panic-captured"
                break
        else:
            if running_seen and time.time() - last_rx > 15:
                caught = "silence-freeze"
                break
    try:
        ser.close()
    except Exception:
        pass
    text = "".join(buf).replace("\r", "")
    with open("/tmp/freeze_watch.log", "w", encoding="utf-8") as f:
        f.write(text)
    rep = {"observed_s": round(time.time() - start),
           "running_seen": running_seen,
           "caught": caught,
           "panic_markers": sum(p.lower() in text.lower() for p in PANIC_PAT),
           "silence_detected": caught == "silence-freeze"}
    with open("/tmp/freeze_watch_report.json", "w", encoding="utf-8") as f:
        json.dump(rep, f, ensure_ascii=False, indent=1)
    print(f"[freeze_watch] {rep}")
    if caught == "panic-captured":
        i = max(0, text.lower().find("panic"))
        print("=== 现场前段 ===")
        print(text[max(0, i - 300):i + 2200])
    elif caught == "silence-freeze":
        print("=== 静默冻结: WDT 不可捕获 → 硬件级嫌疑 ===")
        print(text[-500:])
    else:
        print(f"[freeze_watch] 观察窗结束未见冻结({total}s)")


if __name__ == "__main__":
    # 循环模式: 每窗结束(捕获/超时)后自动开下一窗, 持续累积证据
    while True:
        main()
        print("[freeze_watch] 窗口结束, 3s 后开下一窗...\n", flush=True)
        time.sleep(3)
