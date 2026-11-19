#!/usr/bin/env python3
"""soak_watch.py —— 8h 长稳浸泡串口侧监视(P4, 2026-11-19)

职责(单进程独占 ACM0):
  - 连续日志 → /tmp/soak8h_serial.log(带 [SOAK HH:MM] 分钟标记)
  - 每 10min 注入 `mem`(水位采样, 提取 dma_largest/internal_free)
  - 每 60min 注入 stop→radio(电台周期压测, 累积 A1a 路径循环次数)
  - 崩溃模式实时检测(rst:/panic/Guru/assert/backtrace/Stack protection/
    Load resp/TLSF)→ 计数+时间点记 /tmp/soak8h_summary.txt
8h(28800s)后自动收尾并输出总摘要。与 liveness_watch.py(网络旁证)并行。
"""
import re
import serial
import time
from datetime import datetime

DUR_S = 8 * 3600
LOG = '/tmp/soak8h_serial.log'
SUMMARY = '/tmp/soak8h_summary.txt'

CRASH_PAT = re.compile(
    r'rst:0x|Guru Meditation|panic|assert|Stack protection|'
    r'Load access fault|Store access fault|TLSF|abort was called|backtrace')
MEM_PAT = re.compile(
    r'audit\[cli\] internal free=(\d+) largest=\d+ \| dma largest=(\d+) '
    r'\| psram free=(\d+)')
THINK_PAT = re.compile(r'soulthink|think:|deliberat', re.I)
MOOD_PAT = re.compile(r'face bg|mood|uimood|emotion', re.I)

crashes = []
mems = []
think_lines = 0
mood_lines = 0
t0 = time.time()
next_mem = 60
next_cycle = 600
next_mark = 0

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
lf = open(LOG, 'ab', buffering=0)
sf = open(SUMMARY, 'w', buffering=1)
sf.write(f"soak start {datetime.now().isoformat()}\n")


def note(s):
    sf.write(s + '\n')


while time.time() - t0 < DUR_S:
    el = time.time() - t0
    if el >= next_mark:
        stamp = datetime.now().strftime('%H:%M')
        lf.write(f"\n[SOAK {stamp} +{int(el)}s]\n".encode())
        next_mark += 60
    if el >= next_mem:
        ser.write(b"mem\r\n")
        note(f"[{int(el)}s] mem injected")
        next_mem += 600
    if el >= next_cycle and el > 300:            # 首个周期在 10min 处
        ser.write(b"stop\r\n")
        time.sleep(2)
        ser.write(b"radio\r\n")
        note(f"[{int(el)}s] radio cycle injected")
        next_cycle += 3600
    d = ser.read(2048)
    if not d:
        continue
    lf.write(d)
    try:
        text = d.decode('utf-8', 'replace')
    except Exception:
        continue
    for ln in text.splitlines():
        if CRASH_PAT.search(ln):
            crashes.append((int(el), ln.strip()[:160]))
            note(f"[{int(el)}s] CRASH? {ln.strip()[:160]}")
        m = MEM_PAT.search(ln)
        if m:
            mems.append((int(el), int(m.group(1)), int(m.group(2))))
        if THINK_PAT.search(ln):
            think_lines += 1
        if MOOD_PAT.search(ln):
            mood_lines += 1

ser.close()
dma_min = min((m[2] for m in mems), default=None)
int_min = min((m[1] for m in mems), default=None)
note("=== SOAK 8h SUMMARY ===")
note(f"duration_s={int(time.time() - t0)}")
note(f"crash_events={len(crashes)}")
for c in crashes[:20]:
    note(f"  {c}")
note(f"mem_samples={len(mems)} dma_min={dma_min} internal_min={int_min}")
note(f"radio_cycles~={sum(1 for l in open(SUMMARY) if 'radio cycle' in l)}")
note(f"think_lines={think_lines} mood_lines={mood_lines}")
sf.close()
lf.close()
