#!/usr/bin/env python3
"""
ai_selftest.py —— EspCube AI 点歌链自动化验证 harness(替代人工对板说话)

原理: 板上 `selftest <文本>` = 云端 TTS 合成"人声" → 注入麦克服务(实时
节奏) → 唤醒→VAD→ASR→LLM(function calling)→NCM→播放器→TTS 回应全链
照常运转。本脚本驱动串口, 按日志打点断言 + 分段测量延迟。

用例:
  0. radio 起播(基线)
  1. selftest ×N 轮 —— 必备断言: 聆听挂起/ASR 文本/技能执行/切歌/回 IDLE
     可选打点(缺了只 WARN): tool hint / filler / prefetch HIT / handoff
  2. stwake —— 合成"你好小智"注入, 期望 wn9 检出(TTS 嗓音对 wn9 属边界
     用例, 未检出记 SKIP 不算 FAIL)

用法: python3 tools/ai_selftest.py [--port /dev/ttyACM0] [--rounds 2]
退出码 0=PASS 1=FAIL
"""
import argparse
import re
import sys
import time

import serial

# 硬故障签名(出现即 FAIL)
FAULTS = [
    ("task-failed", re.compile(r"tts task-failed|asr task-failed")),
    ("drop-round", re.compile(r"drop round")),
    ("llm-err", re.compile(r"v2 http \d\d\d|llm v2 rc=-")),
    ("reboot", re.compile(r"EspCube cube-speaker prototype")),
]

# (key, 正则, 超时s, 必备)
STEPS = [
    ("synth",    re.compile(r"ST: synth ok \(([\d.]+)s\)"),    30,  True),
    ("inject",   re.compile(r"mic inject armed"),               5,  True),
    ("listen",   re.compile(r"state=2 -> stream suspend"),     20,  True),
    ("asr",      re.compile(r"asr: (.+?)\s*$"),                40,  True),
    ("hint",     re.compile(r"tool hint: music"),              25, False),
    ("filler",   re.compile(r"filler play"),                   25, False),
    ("pfhit",    re.compile(r"prefetch HIT"),                   5, False),
    ("handoff",  re.compile(r"ai handoff: new stream released"), 5, False),
    ("exec",     re.compile(r"skill music\.search_play"),      30,  True),
    # reply 与 music 毫秒级竞态(fast_ack 即时喂 vs 播放器建连), 锚定
    # 在 exec 之后各自独立匹配(见 round 内 FREE_AFTER_EXEC)
    ("music",    re.compile(r"playing (http\S*music\.126\.net)"), 40, True),
    ("reply",    re.compile(r"llm sentence.*: (.+?)\s*$"),      30, True),
    ("idle",     re.compile(r"round complete -> IDLE|ai state=0 -> stream resume"), 30, True),
]

STEPS_ORDER = [k for k, _, _, _ in STEPS]


class Runner:
    def __init__(self, port):
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.faults = []
        self.fails = []
        self.info = {"asr": [], "music": [], "reply": []}
        self._rbuf = b""   # 跨 wait 持久行缓冲

    def _lines(self, deadline):
        """产出完整行。半行留在 self._rbuf 跨窗口保留 —— wait 在匹配
        瞬间返回时生成器被弃, 局部缓冲会把已读的半行丢掉, 该行就永远
        错过(R1 丢 inject / R2 丢 synth 的元凶, 2026-11-12)"""
        while time.time() < deadline:
            b = self.ser.read(64)
            if b:
                self._rbuf += b
            while b"\n" in self._rbuf:
                line, self._rbuf = self._rbuf.split(b"\n", 1)
                yield line.decode("utf-8", "replace").rstrip("\r")

    def wait(self, pat, tmo, fault=True):
        """等待单条正则; 返回 (match, t) 或 (None, t)"""
        t_end = time.time() + tmo
        for line in self._lines(t_end):
            if line:
                print(f"    | {line[:145]}")
            if fault:
                for fname, fp in FAULTS:
                    if fp.search(line):
                        self.faults.append(f"{fname}: {line[:80]}")
                        return ("FAULT", time.time())
            m = pat.search(line)
            if m:
                return (m, time.time())
        return (None, time.time())

    def drain(self, secs):
        for line in self._lines(time.time() + secs):
            if line:
                print(f"    | {line[:145]}")

    def baseline_radio(self):
        print("== 基线: 电台起播 ==")
        self.ser.reset_input_buffer()
        self.ser.write(b"radio\n")
        m, t = self.wait(re.compile(r"playing http\S*m3u8"), 30, fault=False)
        if m is None:
            self.fails.append("baseline:radio-timeout")
            print("  [FAIL] 电台未起播")
            return False
        print("  [ok] 电台播放中")
        self.drain(2)
        return True

    def round(self, idx, phrase):
        print(f"\n== ROUND {idx}: selftest {phrase} ==")
        # busy 重发: 上一轮板上可能尚未回 IDLE, 最多等 60s
        for _ in range(12):
            self.ser.reset_input_buffer()
            self.ser.write(f"selftest {phrase}\n".encode())
            m, t = self.wait(re.compile(r"ST: submit|ST: busy|ST: synth failed"), 12, fault=False)
            if m is None or m == "FAULT":
                self.fails.append(f"R{idx}:ack:{m}")
                return {}
            txt = m.group(0)
            if "busy" in txt:
                print("  [wait] 上一轮未收尾, 5s 后重试")
                self.drain(5)
                continue
            if "failed" in txt:
                self.fails.append(f"R{idx}:synth-failed")
                return {}
            break
        # ---- 整轮全量录制直到回 IDLE(或总超时), 后置按序匹配 ----
        # (逐步等待会"吃掉"下一步的行: 可选打点等待窗把后续必备行消费
        #  掉导致误报超时, 2026-11-12 三轮 harness 迭代教训)
        print("  [rec] 录制整轮(至回 IDLE)...")
        events = []                     # (t, line)
        idle_pat = re.compile(
            r"round complete -> IDLE|ai state=0 -> stream resume")
        t_end = time.time() + 100
        got_idle = False
        buf = self._rbuf
        last_rx = time.time()
        while time.time() < t_end:
            b = self.ser.read(256)
            if b:
                last_rx = time.time()
                buf += b
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if not line:
                        continue
                    now = time.time()
                    print(f"    | {line[:145]}")
                    events.append((now, line))
                    if idle_pat.search(line):
                        got_idle = True
                        t_end = min(t_end, now + 4)
            elif time.time() - last_rx > 20:
                # 串口静默看门狗: 板上电台/混音日志 ≥每 10s 一条,
                # 20s 全静默 = USB ACM 掉线(实测整轮黑屏的元凶) → 重开
                print("  [warn] serial silence 20s, reopen port")
                try:
                    self.ser.close()
                except Exception:
                    pass
                time.sleep(1.0)
                self.ser = serial.Serial(self.port, 115200, timeout=0.05)
                last_rx = time.time()
        self._rbuf = buf
        # ---- 后置按序匹配 ----
        marks, extras = {}, {}
        idx0 = 0
        exec_idx = 0
        for key, pat, tmo, must in STEPS:
            hit = None
            # reply 与 music 毫秒级竞态: reply 的搜索起点回到 exec 锚
            start = (exec_idx + 1) if key == "reply" else idx0
            for i in range(start, len(events)):
                m = pat.search(events[i][1])
                if m:
                    hit = (i, m, events[i][0])
                    break
            if hit is None:
                if must:
                    self.fails.append(f"R{idx}:{key}:missing")
                    print(f"  [FAIL] {key} 未出现")
                else:
                    print(f"  [WARN] {key} 未出现(可选)")
                continue
            i, m, t = hit
            idx0 = max(idx0, i + 1)          # reply 早于 music 时勿回拉
            if key == "exec":
                exec_idx = i
            marks[key] = t
            if key in self.info and m.groups():
                self.info[key].append(m.group(1))
            if key == "synth":
                extras["synth_len"] = float(m.group(1))
        for key in STEPS_ORDER:
            if key in marks:
                dt = marks[key] - marks[STEPS_ORDER[0]]
                print(f"  [ok] {key} (+{dt:.2f}s)")
        # 故障扫描(整轮)
        for _, line in events:
            for fname, fp in FAULTS:
                if fp.search(line):
                    self.faults.append(f"R{idx} {fname}: {line[:70]}")
        if not got_idle and "idle" not in marks:
            self.fails.append(f"R{idx}:no-idle")
        self.ser.write(b"radio\n")
        self.drain(4)
        return marks, extras

    def stwake(self):
        print("\n== STWAKE: 合成'你好小智'注入, 期望 wn9 检出 ==")
        self.ser.reset_input_buffer()
        self.ser.write(b"stwake\n")
        m, t = self.wait(re.compile(r"★ WAKE #"), 25, fault=False)
        if m:
            print("  [ok] ★ WAKE(TTS 嗓音可触发 wn9)")
            self.drain(6)   # 让这轮聆听走完(注入音频耗尽→no speech→drop 属预期)
            return "PASS"
        print("  [SKIP] wn9 未检出 TTS 嗓音(边界用例, 不算 FAIL)")
        self.drain(3)
        return "SKIP"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--phrase", default="我想听一首伤感雨季")
    args = ap.parse_args()

    r = Runner(args.port)
    if not r.baseline_radio():
        print("\n总判定: FAIL")
        sys.exit(1)

    results = []
    for i in range(args.rounds):
        results.append(r.round(i + 1, args.phrase))
    r.stwake()

    print("\n" + "=" * 62)
    print("断言")
    ok = True
    def check(cond, msg):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
        if not cond:
            ok = False

    n_asr = len(r.info["asr"])
    n_music = len(r.info["music"])
    check(n_asr >= max(1, args.rounds - 1),
          f"ASR 识别({n_asr}/{args.rounds}): {r.info['asr']}")
    check(n_music >= max(1, args.rounds - 1),
          f"NCM 切歌({n_music}/{args.rounds})")
    check(not r.faults, "无云端/协议故障" + (f": {r.faults}" if r.faults else ""))
    check(not r.fails, "流程完整" + (f": {r.fails}" if r.fails else ""))

    print("\n延迟分段(每轮, 秒)")
    segs = [("inject→listen", "inject", "listen"),   # 唤醒处理+ASR 开
            ("listen→asr",    "listen", "asr"),      # 注入语音+VAD 尾+定稿
            ("asr→hint",      "asr", "hint"),        # LLM 决策+流到关键词
            ("asr→exec",      "asr", "exec"),        # LLM 完整轮
            ("asr→music",     "asr", "music"),       # 认出指令→歌响(核心)
            ("asr→reply",     "asr", "reply")]
    for i, res in enumerate(results):
        if not isinstance(res, tuple):
            print(f"  R{i+1}: (轮未完成)")
            continue
        marks = res[0]
        row = []
        for label, a, b in segs:
            if a in marks and b in marks:
                row.append(f"{label}={marks[b] - marks[a]:.2f}")
        opt = [k for k in ("hint", "filler", "pfhit", "handoff") if k in marks]
        print(f"  R{i+1}: {' '.join(row)}")
        print(f"       可选特性命中: {opt or '无'}")

    verdict = "PASS" if ok else "FAIL"
    print(f"\n总判定: {verdict}")
    sys.exit(0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    main()
