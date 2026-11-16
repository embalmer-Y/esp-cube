#!/usr/bin/env python3
"""freeze_watch.py —— P4 串口冻结现场监听器（freeze-and-face-blackscreen-fix 任务 13.3）。

运行期间只打开一次串口。打开端口会复位板子（见 evidence/1.5），因此：
  - 只在 SerialException（设备消失）时关闭并重开，记 USB_REENUM；启动时设备不存在则轮询等待其出现。
  - 本工具从不切换 DTR。RTS 只在操作者发送 !rst 时脉冲一次，等同 RST 键。
  - 打开后的第一条 ROM 复位横幅记为 MONITOR_OPEN_RESET：由本工具的打开造成，不计为冻结或下载模式。

事件类别（classify_stream 与实时监听共用同一分类器）：
  PANIC               panic、Guru Meditation、Backtrace 等，判定与任务 12 预言机一致
  ROM_RESET           ROM 复位行，细节为 rst 文本，如 rst:0x7 (TG0WDT_SYS_RST)
  HOST_DOWNLOAD_MODE  boot:0x3..(DOWNLOAD 或 waiting for download)，主机诱发；所在窗口作废、不计时长
  SILENT_STOP         15 s 以上无输出，且静默开始前的最后一行不在 ROM 或下载模式段内
  AUTO_RECOVERED      冻结、panic 或故障复位之后出现 sysdiag: 启动报告
  MONITOR_OPEN_RESET  打开端口后的第一条复位横幅（本工具造成）
  SAVED_PC            ROM 横幅或 sysdiag 中的 Saved PC，附 riscv32-esp-elf-addr2line 结果
  CMD                 中继命令，记发送时刻
  其他                USB_REENUM、OPEN、DEVICE_WAIT、PORT_BUSY、OPEN_FAIL、RUN_START、RUN_END 等运行记录

中继协议（任务 13.2 的 p4_cli.py 依赖）：向 /tmp/freeze_watch.cmd 写入一行 UTF-8 文本并以换行结束，
本工具把该行原样加换行发往串口；!rst 为 RTS 脉冲。窗口日志 /tmp/freeze_watch_<时间戳>.log 实时追加，
p4_cli.py 从中读回命令之后的输出。--out 指向其他目录时，窗口日志写入该目录，并在 /tmp 放一个指向它的
符号链接，供中继读回。

用法:
  python3 tools/freeze_watch.py [--port /dev/ttyACM0] [--out DIR] [--usb-events] [--max-s S]
  python3 tools/freeze_watch.py --replay <log> [--out DIR]
"""
import argparse
import errno
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
from collections import Counter
from datetime import datetime
from pathlib import Path

import serial

from serial_excl import open_exclusive

REPO = Path(__file__).resolve().parent.parent
PORT = "/dev/ttyACM0"
BAUD = 115200
RELAY_FIFO = "/tmp/freeze_watch.cmd"
LINK_DIR = Path("/tmp")
ELF = REPO / "firmware" / "build-p4c5" / "esp_cube.elf"

SILENCE_S = 15.0          # 无输出判为静默的时长
OPEN_RESET_S = 5.0        # 打开端口后此时长内的第一条复位横幅记为 MONITOR_OPEN_RESET
OPERATOR_RESET_S = 10.0   # 发出 !rst 后此时长内的复位不计为故障
PANIC_MERGE_S = 5.0       # 同一次 panic 的多行合并为一条 PANIC
REPLAY_TAIL_S = 20.0      # 回放时假定末行之后静默 20 s（与任务 11 的重放用例一致）
RST_PULSE_S = 0.1         # !rst 的 RTS 脉冲宽度
SUMMARY_EVERY_S = 30.0
RELAY_MAX_PENDING = 65536

PANIC_PAT = ("panic", "abort()", "assert failed", "Guru Meditation",
             "Backtrace", "Task watchdog", "Interrupt wdt")
_PANIC_LOW = tuple(p.lower() for p in PANIC_PAT)
APP_LINE = re.compile(r"^\s*[IWEDV] \(\d+\) ")
RST = re.compile(r"\brst:0x[0-9a-f]+\s*\([^)]*\)", re.I)
DOWNLOAD = re.compile(r"\bboot:0x3[0-9a-f]{2}\s*\(\s*download", re.I)
WAIT_DOWNLOAD = re.compile(r"waiting for download", re.I)
ESP_ROM = re.compile(r"\besp-rom:", re.I)
SAVED_PC = re.compile(r"\bsaved[ _]?pc\s*[:=]?\s*(0x[0-9a-f]+)", re.I)
# CH343 (1a86:，内核日志中为 idVendor=1a86,)、Espressif USB 串口 (303a:，即 ttyACM1)、ttyACM 设备行
USB_FILTER = re.compile(r"\b(?:1a86|303a)\b|ttyACM")
HOST_RST_KEYS = ("POWERON", "USB_UART", "USB_JTAG")


def is_fault_reset(rst_text):
    """故障复位（看门狗、panic 等）。POWERON 与 USB 串口复位由人工或主机造成，不算故障。"""
    up = rst_text.upper()
    return not any(k in up for k in HOST_RST_KEYS)


class StreamClassifier:
    """逐行分类器。时间单位为秒，同一次运行内须使用同一时钟。classify_stream 与实时监听共用。"""

    def __init__(self, symbolize=None):
        self._symbolize = symbolize
        self.last_out = None        # 最近一次有字节到达的时刻；None 表示尚无输出或设备已消失
        self.reported = False       # 本段静默是否已报 SILENT_STOP
        self.rom = False            # 处于 ROM 或下载模式段内（直到出现首条应用日志行）
        self.dl_reported = False    # 本下载模式段是否已报 HOST_DOWNLOAD_MODE
        self.anomaly = False        # 自上次启动报告以来出现过冻结、panic 或故障复位
        self.armed = False          # 打开后的第一条复位横幅尚未出现
        self.open_t = None
        self.operator_until = None  # 操作者 !rst 之后，此时刻前的复位不计为故障
        self.panic_t = None

    def arm_open(self, t):
        """端口刚被打开。OPEN_RESET_S 内的第一条复位横幅记为 MONITOR_OPEN_RESET；静默计时从此刻起。"""
        self.open_t = t
        self.armed = True
        self.last_out = t
        self.reported = False
        self.rom = False
        self.dl_reported = False

    def operator_reset(self, t):
        """操作者发出 !rst。之后的复位由操作者造成，不计为故障，也不使随后的启动报告判为 AUTO_RECOVERED。"""
        self.operator_until = t + OPERATOR_RESET_S
        self.anomaly = False

    def lost(self):
        """设备消失。消失期间不计静默。"""
        self.last_out = None
        self.reported = False
        self.rom = False
        self.armed = False

    def tick(self, t):
        """判定截至 t 时刻的静默。ROM 或下载模式段内不判 SILENT_STOP。"""
        if self.last_out is None or self.reported or self.rom:
            return []
        gap = t - self.last_out
        if gap < SILENCE_S:
            return []
        self.reported = True
        self.anomaly = True
        return [("SILENT_STOP", self.last_out, f"no output for {gap:.1f}s")]

    def on_activity(self, t):
        """t 时刻有字节到达。先判定此前的静默，再开始新的计时。"""
        events = self.tick(t)
        self.last_out = t
        self.reported = False
        return events

    def finish(self, end_t):
        return [] if end_t is None else self.tick(end_t)

    def on_line(self, t, text):
        events = self.on_activity(t)
        low = text.lower()
        if self.armed and t - self.open_t > OPEN_RESET_S:
            self.armed = False
        if any(p in low for p in _PANIC_LOW):
            if self.panic_t is None or t - self.panic_t > PANIC_MERGE_S:
                events.append(("PANIC", t, text.strip()[:200]))
            self.panic_t = t
            self.anomaly = True
        saved = SAVED_PC.search(text)
        if saved:
            pc = saved.group(1)
            detail = pc if self._symbolize is None else f"{pc} {self._symbolize(pc)}"
            events.append(("SAVED_PC", t, detail))
        if APP_LINE.match(text) or "sysdiag:" in low:
            self.rom = False
            self.dl_reported = False
            if "sysdiag:" in low and self.anomaly:
                events.append(("AUTO_RECOVERED", t, text.strip()[:200]))
                self.anomaly = False
        elif RST.search(text):
            events.extend(self._reset_line(t, text))
        elif WAIT_DOWNLOAD.search(text):
            if not self.dl_reported:
                events.append(("HOST_DOWNLOAD_MODE", t, text.strip()[:200]))
                self.dl_reported = True
            self.rom = True
        elif ESP_ROM.search(text):
            self.rom = True
        return events

    def _reset_line(self, t, text):
        rst_text = RST.search(text).group(0)
        in_open = self.armed
        self.armed = False
        if in_open:
            self.dl_reported = False
            events = [("MONITOR_OPEN_RESET", t, rst_text)]
        elif DOWNLOAD.search(text):
            self.dl_reported = True
            events = [("HOST_DOWNLOAD_MODE", t, text.strip()[:200])]
        else:
            self.dl_reported = False
            events = [("ROM_RESET", t, rst_text)]
            if self.operator_until is not None and t <= self.operator_until:
                self.operator_until = None
            elif is_fault_reset(rst_text):
                self.anomaly = True
        self.rom = True
        return events


def classify_stream(records, end_t=None, open_t=None, symbolize=None):
    """对 (t 秒, 行文本) 序列分类，返回 [(类别, 时刻, 细节)]。end_t 给出时，末行到 end_t 的静默同样计入。"""
    clf = StreamClassifier(symbolize=symbolize)
    if open_t is not None:
        clf.arm_open(open_t)
    events = []
    for t, text in records:
        events.extend(clf.on_line(t, text))
    events.extend(clf.finish(end_t))
    return events


def find_addr2line():
    exe = shutil.which("riscv32-esp-elf-addr2line")
    if exe:
        return exe
    found = sorted(glob.glob(os.path.expanduser(
        "~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line")))
    return found[-1] if found else None


class Symbolizer:
    """Saved PC 的 riscv32-esp-elf-addr2line -pfiaC -e <elf> <pc> 结果，按 pc 缓存。"""

    def __init__(self, elf=ELF):
        self.elf = Path(elf)
        self.exe = find_addr2line()
        self._cache = {}

    def __call__(self, pc):
        if pc not in self._cache:
            self._cache[pc] = self._run(pc)
        return self._cache[pc]

    def _run(self, pc):
        if self.exe is None:
            return "(riscv32-esp-elf-addr2line 未找到)"
        if not self.elf.exists():
            return f"(ELF 不存在: {self.elf})"
        try:
            res = subprocess.run([self.exe, "-pfiaC", "-e", str(self.elf), pc],
                                 capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return f"(addr2line 失败: {exc})"
        return (res.stdout or res.stderr).strip() or f"(无输出, 退出码 {res.returncode})"


class UsbEvents:
    """旁路运行 journalctl -k -f，把 USB_FILTER 命中的行追加写入文件。"""

    def __init__(self, path):
        self.error = None
        self._stopping = False
        self.proc = subprocess.Popen(
            ["journalctl", "-k", "-f", "-o", "short-iso", "--no-pager"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", bufsize=1)
        self._thread = threading.Thread(target=self._pump, args=(Path(path),), daemon=True)
        self._thread.start()

    def _pump(self, path):
        with open(path, "a", encoding="utf-8") as fh:
            for line in self.proc.stdout:
                if USB_FILTER.search(line):
                    fh.write(line)
                    fh.flush()
        rc = self.proc.wait()
        err = self.proc.stderr.read().strip()
        if not self._stopping and (rc != 0 or err):
            self.error = f"journalctl 退出码 {rc}: {err}"

    def stop(self):
        self._stopping = True
        if self.proc.poll() is None:
            self.proc.terminate()
        self._thread.join(timeout=3)


def write_json_atomic(path, doc):
    path = Path(path)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _unlink_quiet(path):
    try:
        Path(path).unlink()
    except OSError:
        pass


def _window_doc(w, t, reason):
    dur = 0.0 if w["t0"] is None else max(0.0, t - w["t0"])
    return {"log": w["path"], "link": w["link"], "dur_s": round(dur, 3), "void": w["void"],
            "end": reason, "events": dict(w["counts"])}


class Monitor:
    """实时监听：一次打开，常驻运行，事件与窗口日志落盘。"""

    def __init__(self, port, out_dir, usb_events):
        self.port = port
        self.out = Path(out_dir)
        self.out.mkdir(parents=True, exist_ok=True)
        self.m0 = time.monotonic()
        self.w0 = time.time()
        self.run_id = datetime.now().strftime("%Y%m%dT%H%M%S")
        self.symbolize = Symbolizer()
        self.clf = StreamClassifier(symbolize=self.symbolize)
        self.events_path = self.out / "freeze_watch_events.jsonl"
        self.summary_path = self.out / "freeze_watch_summary.json"
        self.usb_path = (self.out / "usb_events.log") if usb_events else None
        self.events_fh = open(self.events_path, "a", encoding="utf-8")
        self.counts = Counter()
        self.closed = []            # 已结束窗口的记录
        self.win = None             # 当前窗口
        self.ser = None
        self.line_buf = b""
        self.at_line_start = True
        self.relay_fd = None
        self.relay_created = False
        self.relay_buf = b""
        self.links = []
        self.usb = None
        self.usb_reported = False
        self.operator_resets = 0
        self.commands = 0
        self.wait_noted = False
        self.open_err = None

    # ---- 时间与事件 ----
    def now(self):
        return time.monotonic() - self.m0

    def wall_iso(self, t):
        return datetime.fromtimestamp(self.w0 + t).astimezone().isoformat(timespec="milliseconds")

    def emit(self, kind, t, detail, **extra):
        rec = {"run": self.run_id, "mode": "live", "t": round(t, 3), "wall": self.wall_iso(t),
               "kind": kind, "detail": detail, "window": self.win["name"] if self.win else None}
        rec.update(extra)
        self.counts[kind] += 1
        if self.win is not None:
            self.win["counts"][kind] += 1
            if kind == "HOST_DOWNLOAD_MODE":
                self.win["void"] = True
        self.events_fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
        self.events_fh.flush()
        print(f"[freeze_watch] {rec['wall']} {kind} {detail}", flush=True)

    def _emit_all(self, events):
        for kind, t, detail in events:
            self.emit(kind, t, detail)

    # ---- 窗口日志 ----
    def _new_window(self, t0):
        base = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
        path = fh = None
        for i in range(100):
            name = f"freeze_watch_{base}.log" if i == 0 else f"freeze_watch_{base}-{i}.log"
            cand = self.out / name
            try:
                fh = open(cand, "xb")       # 排他创建，不覆盖已有文件
            except FileExistsError:
                continue
            path = cand
            break
        if path is None:
            raise SystemExit(f"无法在 {self.out} 创建窗口日志")
        link = None
        if self.out.resolve() != LINK_DIR.resolve():
            link = LINK_DIR / path.name
            try:
                os.symlink(path, link)
                self.links.append(link)
            except FileExistsError:
                link = None
                print(f"[freeze_watch] 警告: {LINK_DIR / path.name} 已存在，中继读不到此窗口", flush=True)
        self.win = {"name": path.name, "path": str(path), "link": str(link) if link else None,
                    "fh": fh, "t0": t0, "void": False, "has_rst": False, "counts": Counter()}

    def _close_window(self, t, reason):
        if self.win is None:
            return
        self.win["fh"].close()
        self.closed.append(_window_doc(self.win, t, reason))
        self.win = None

    # ---- 串口 ----
    def _try_open(self, t):
        if not os.path.exists(self.port):
            if not self.wait_noted:
                self.emit("DEVICE_WAIT", t, f"{self.port} 不存在，等待其出现")
                self.wait_noted = True
            time.sleep(0.5)
            return
        try:
            ser = open_exclusive(self.port, BAUD, dtr=False, rts=False)
        except OSError as exc:
            if exc.errno != self.open_err:
                self.open_err = exc.errno
                kind = "PORT_BUSY" if exc.errno == errno.EBUSY else "OPEN_FAIL"
                self.emit(kind, t, str(exc)[:200])
            time.sleep(1.0 if exc.errno == errno.EBUSY else 0.5)
            return
        ser.timeout = 0.2
        ser.write_timeout = 2.0
        self.ser = ser
        self.wait_noted = False
        self.open_err = None
        if self.win is None:
            self._new_window(t)
        elif self.win["t0"] is None:
            self.win["t0"] = t
        self.line_buf = b""
        self.at_line_start = True
        self.clf.arm_open(t)
        self.emit("OPEN", t, f"{self.port} 已独占打开（DTR/RTS 置低；打开本身会复位板子）")

    def _close_port(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except (OSError, serial.SerialException):
                pass
            self.ser = None

    def _device_lost(self, t, exc):
        self.emit("USB_REENUM", t, f"{self.port} 消失或读写失败，关闭并等待重开: {exc}"[:240])
        self._close_port()
        self._close_window(t, "USB_REENUM")
        self.clf.lost()
        self.line_buf = b""
        self.at_line_start = True

    def _read(self):
        try:
            data = self.ser.read(4096)
        except serial.SerialException as exc:
            self._device_lost(self.now(), exc)
            return
        if data:
            self._on_bytes(self.now(), data)

    def _on_bytes(self, t, data):
        self._emit_all(self.clf.on_activity(t))
        pos = 0
        while pos < len(data):
            nl = data.find(b"\n", pos)
            end = len(data) if nl < 0 else nl + 1
            seg = data[pos:end]
            if (self.at_line_start and self.win["has_rst"]
                    and (seg.startswith(b"ESP-ROM") or seg.startswith(b"rst:0x"))):
                self._close_window(t, "ROM_BANNER")
                self._new_window(t)
            self.win["fh"].write(seg)
            self.line_buf += seg
            self.at_line_start = seg.endswith(b"\n")
            if self.at_line_start:
                line = self.line_buf.rstrip(b"\r\n").decode("utf-8", "replace")
                self.line_buf = b""
                self._on_line(t, line)
            pos = end
        self.win["fh"].flush()

    def _on_line(self, t, text):
        if RST.search(text):
            self.win["has_rst"] = True
        self._emit_all(self.clf.on_line(t, text))

    # ---- 中继 ----
    def _open_relay(self):
        try:
            os.mkfifo(RELAY_FIFO, 0o600)
            self.relay_created = True
        except FileExistsError:
            pass
        st = os.lstat(RELAY_FIFO)
        if not stat.S_ISFIFO(st.st_mode) or st.st_uid != os.getuid():
            raise SystemExit(f"{RELAY_FIFO} 已存在且不是本用户的命名管道，拒绝使用")
        self.relay_fd = os.open(RELAY_FIFO, os.O_RDWR | os.O_NONBLOCK)

    def _poll_relay(self):
        if self.relay_fd is None:
            return
        try:
            self.relay_buf += os.read(self.relay_fd, 4096)
        except BlockingIOError:
            return
        while b"\n" in self.relay_buf:
            line, self.relay_buf = self.relay_buf.split(b"\n", 1)
            self._command(line.rstrip(b"\r").decode("utf-8", "replace"))
        if len(self.relay_buf) > RELAY_MAX_PENDING:
            self.relay_buf = b""
            self.emit("CMD", self.now(), "(丢弃超长且无换行的中继输入)", sent=False, reason="too long")

    def _command(self, text):
        t = self.now()
        self.commands += 1
        if self.ser is None:
            self.emit("CMD", t, text, sent=False, reason="port not open")
            return
        if text == "!rst":
            try:
                self.ser.rts = True
                try:
                    time.sleep(RST_PULSE_S)
                finally:
                    self.ser.rts = False
            except (OSError, serial.SerialException) as exc:
                self.emit("CMD", t, text, sent=False, reason=f"RTS 脉冲失败: {exc}"[:160])
                return
            self.operator_resets += 1
            self.clf.operator_reset(t)
            self.emit("CMD", t, text, sent=True, rts_pulse_s=RST_PULSE_S)
            return
        try:
            self.ser.write(text.encode("utf-8") + b"\n")
        except serial.SerialTimeoutException as exc:
            self.emit("CMD", t, text, sent=False, reason=f"写超时: {exc}"[:160])
            return
        except serial.SerialException as exc:
            self.emit("CMD", t, text, sent=False, reason=f"设备已消失: {exc}"[:160])
            self._device_lost(t, exc)
            return
        self.emit("CMD", t, text, sent=True)

    # ---- USB 旁路 ----
    def _start_usb(self):
        try:
            self.usb = UsbEvents(self.usb_path)
        except OSError as exc:
            self.emit("USB_EVENTS_ERROR", self.now(), f"journalctl 无法启动: {exc}"[:200])

    def _check_usb(self):
        if self.usb is not None and self.usb.error and not self.usb_reported:
            self.usb_reported = True
            self.emit("USB_EVENTS_ERROR", self.now(), self.usb.error[:200])

    # ---- 主循环与收尾 ----
    def run(self, max_s):
        self._open_relay()
        self._new_window(None)
        if self.usb_path is not None:
            self._start_usb()
        self.emit("RUN_START", self.now(), f"port={self.port} out={self.out} relay={RELAY_FIFO}")
        next_save = 0.0
        while True:
            t = self.now()
            if max_s and t >= max_s:
                break
            if self.ser is None:
                self._try_open(t)
            else:
                self._read()
            self._poll_relay()
            if self.ser is not None:
                self._emit_all(self.clf.tick(self.now()))
            self._check_usb()
            if t >= next_save:
                self._save_summary(t, final=False)
                next_save = t + SUMMARY_EVERY_S

    def _summary(self, t, final):
        wins = list(self.closed)
        if self.win is not None:
            wins.append(_window_doc(self.win, t, "open"))
        valid = sum(w["dur_s"] for w in wins if not w["void"])
        return {
            "run": self.run_id, "mode": "live", "final": final, "port": self.port,
            "started": self.wall_iso(0.0), "updated": self.wall_iso(t),
            "valid_observation_s": round(valid, 1),
            "total_window_s": round(sum(w["dur_s"] for w in wins), 1),
            "windows": len(wins),
            "void_windows": sum(1 for w in wins if w["void"]),
            "host_download_mode_events": self.counts["HOST_DOWNLOAD_MODE"],
            "operator_resets": self.operator_resets,
            "commands": self.commands,
            "events": dict(sorted(self.counts.items())),
            "window_logs": [w["log"] for w in wins],
            "events_file": str(self.events_path),
            "usb_events_log": str(self.usb_path) if self.usb_path else None,
            "relay": RELAY_FIFO,
            "addr2line": self.symbolize.exe,
            "elf": str(ELF),
        }

    def _save_summary(self, t, final):
        write_json_atomic(self.summary_path, self._summary(t, final))

    def close(self):
        t = self.now()
        self.emit("RUN_END", t, "监听结束")
        self._close_port()
        self._close_window(t, "EXIT")
        if self.usb is not None:
            self.usb.stop()
        if self.relay_fd is not None:
            os.close(self.relay_fd)
            self.relay_fd = None
        if self.relay_created:
            _unlink_quiet(RELAY_FIFO)
        for link in self.links:
            _unlink_quiet(link)
        self._save_summary(t, final=True)
        self.events_fh.close()


def run_replay(log_path, out_dir):
    raw = Path(log_path).read_bytes()
    records = [(0.0, line) for line in raw.decode("utf-8", "replace").splitlines()]
    events = classify_stream(records, end_t=REPLAY_TAIL_S, symbolize=Symbolizer())
    counts = Counter(kind for kind, _, _ in events)
    void = counts["HOST_DOWNLOAD_MODE"] > 0
    run_id = datetime.now().strftime("%Y%m%dT%H%M%S")
    summary = {
        "run": run_id, "mode": "replay", "source": str(log_path),
        "source_sha256": hashlib.sha256(raw).hexdigest(), "records": len(records),
        "time_base": f"日志无时间戳：全部记为 t=0，末行之后按 {REPLAY_TAIL_S:g} s 静默计算",
        "events": dict(sorted(counts.items())),
        "host_download_mode_events": counts["HOST_DOWNLOAD_MODE"],
        "void_windows": 1 if void else 0,
        "valid_observation_s": 0.0 if void else None,
    }
    for kind, t, detail in events:
        print(f"{kind} t={t:.3f} {detail}")
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    if out_dir:
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        with open(out / "freeze_watch_events.jsonl", "a", encoding="utf-8") as fh:
            for kind, t, detail in events:
                rec = {"run": run_id, "mode": "replay", "t": round(t, 3), "kind": kind, "detail": detail}
                fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
        write_json_atomic(out / "freeze_watch_summary.json", summary)
    return 0


def _on_term(signum, frame):
    raise SystemExit(128 + signum)


def main(argv=None):
    ap = argparse.ArgumentParser(description="P4 串口冻结现场监听器（任务 13.3）")
    ap.add_argument("--port", default=PORT, help="串口设备（默认 %(default)s）")
    ap.add_argument("--out", help="输出目录（默认 /tmp）")
    ap.add_argument("--replay", metavar="LOG", help="离线分类一个日志文件后退出")
    ap.add_argument("--usb-events", action="store_true",
                    help="旁路运行 journalctl -k -f，过滤写入 usb_events.log")
    ap.add_argument("--max-s", type=float, default=0.0,
                    help="最长运行秒数（0 为直到 SIGTERM 或 Ctrl-C）")
    args = ap.parse_args(argv)
    if args.replay:
        return run_replay(args.replay, args.out)
    signal.signal(signal.SIGTERM, _on_term)
    mon = Monitor(args.port, args.out or str(LINK_DIR), args.usb_events)
    try:
        mon.run(args.max_s)
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
