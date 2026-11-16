#!/usr/bin/env python3
"""freeze_watch 分类器与串口独占打开的宿主测试（freeze-and-face-blackscreen-fix 任务 11）。

本文件先于实现编写。未修复代码上 freeze_watch.classify_stream 与 serial_excl 尚不存在，
三个测试应当失败，失败即证实主机侧假象存在。任务 13.5 原样复跑本文件，全部通过才算修复成立。
PreserveTest（preserve 组，任务 12）只依赖预言机，未修复代码上应通过。

接口契约（任务 13.1、13.3 须实现）：
  freeze_watch.classify_stream(records, end_t=None) -> list[(类别, 触发时刻, 细节)]
      records 为 (t 秒, 行文本) 的可迭代对象，t 单调不减。
      相邻两条记录的间隔 >= 15 s 记为静默；给出 end_t 时，末条记录到 end_t 的间隔同样记为静默。
      类别取 PANIC、ROM_RESET、HOST_DOWNLOAD_MODE、SILENT_STOP、AUTO_RECOVERED；
      ROM_RESET 的细节含 rst 码，例如 "rst:0x7 (TG0WDT_SYS_RST)"。
  serial_excl.open_exclusive(port, baud, dtr=False, rts=False) -> serial.Serial
      先设好 dtr/rts 再 open()，随后 fcntl.ioctl(fd, TIOCEXCL)。端口已被独占时抛出 errno 为 EBUSY 的异常。
"""

import errno
import os
import random
import unittest
from pathlib import Path

import freeze_watch

FIXTURE = Path(__file__).resolve().parent / "testdata" / "freeze_win3_tail.log"
PBT_SEED = 20261115
PBT_CASES = 1200

NORMAL_MSGS = (
    "mixer: health ok",
    "app: tick",
    "lv_efc: perf fps=9 changed=412x194",
    "wifi: got ip 192.168.1.20",
    "sntp: synced",
)
DOWNLOAD_LINES = (
    "rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))",
    "waiting for download",
)
RESET_LINES = (  # (ROM 行, rst 码)
    ("rst:0x7 (TG0WDT_SYS_RST),boot:0x13 (SPI_FAST_FLASH_BOOT)", "0x7"),
    ("rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)", "0xc"),
)
PANIC_LINES = (
    "Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.",
    "Backtrace: 0x4080a1b2:0x3ffb1c20 0x40801f3e:0x3ffb1c40",
)
SYSDIAG_LINES = (
    "sysdiag: fw=b5a3382-dirty elf=d973d258 built=Nov 14 2026 06:52:21",
    "sysdiag: boot#124 reset=LP_WDT raw0=0x0 raw1=0x0 abn_total=3",
    "sysdiag: last run 1407.2s trigger=NONE inject=none sealed@1407.0s",
)


def _gen_case(rng):
    """生成一段随机日志流，返回 (用例类型, 记录列表, end_t, 附加信息)。

    注入内容前后都有普通行。静默不少于 16 s，因此静默段两侧的记录间隔一定超过 15 s。
    """
    kind = rng.choice(("none", "download", "reset", "panic", "silence", "sysdiag"))
    records = []
    now = 0.0
    info = None

    def put(text):
        nonlocal now
        records.append((round(now, 3), text))
        now += rng.uniform(0.05, 2.0)

    def normal(lo, hi):
        for _ in range(rng.randint(lo, hi)):
            put(f"I ({rng.randrange(1000, 2_000_000)}) {rng.choice(NORMAL_MSGS)}")

    def gap_or_tail():
        """静默放在两条记录之间（返回 None），或放在末条记录之后作为 end_t（返回截止时刻）。"""
        nonlocal now
        if rng.random() < 0.5:
            now += rng.uniform(16.0, 30.0)
            normal(1, 3)
            return None
        return now + rng.uniform(16.0, 30.0)

    normal(1, 4)
    end_t = None
    if kind == "none":
        normal(1, 6)
    elif kind == "download":
        for _ in range(rng.randint(1, 3)):
            put(rng.choice(DOWNLOAD_LINES))
        end_t = gap_or_tail()
    elif kind == "reset":
        line, info = rng.choice(RESET_LINES)
        put(line)
        end_t = gap_or_tail()
    elif kind == "panic":
        for line in PANIC_LINES:
            put(line)
        normal(1, 3)
    elif kind == "silence":
        end_t = gap_or_tail()
    elif kind == "sysdiag":
        now += rng.uniform(16.0, 30.0)
        for msg in SYSDIAG_LINES:
            put(f"I ({rng.randrange(1000, 2_000_000)}) {msg}")
        normal(1, 3)
    return kind, records, end_t, info


def _expect(kind, info, events):
    """返回 None 表示该用例的分类符合期望，否则返回失败原因。"""
    kinds = [ev[0] for ev in events]
    if kind == "none":
        return None if not events else "普通日志流不应产生事件"
    if kind == "download":
        if "HOST_DOWNLOAD_MODE" not in kinds:
            return "ROM 下载模式行未判为 HOST_DOWNLOAD_MODE"
        return "ROM 下载模式被判为 SILENT_STOP" if "SILENT_STOP" in kinds else None
    if kind == "reset":
        if not any(k == "ROM_RESET" and info in str(detail) for k, _, detail in events):
            return f"ROM 复位行（rst 码 {info}）未判为 ROM_RESET"
        return "ROM 复位之后的静默被判为 SILENT_STOP" if "SILENT_STOP" in kinds else None
    if kind == "panic":
        return None if "PANIC" in kinds else "panic + Backtrace 未判为 PANIC"
    if kind == "silence":
        if "SILENT_STOP" not in kinds:
            return "无 ROM 行的 15 s 以上静默未判为 SILENT_STOP"
        if "HOST_DOWNLOAD_MODE" in kinds or "ROM_RESET" in kinds:
            return "无 ROM 行的静默被判为 ROM 类事件"
        return None
    if kind == "sysdiag":
        return None if "AUTO_RECOVERED" in kinds else "sysdiag 启动报告未判为 AUTO_RECOVERED"
    return f"未知用例类型 {kind}"


class FreezeWatchClassifyTest(unittest.TestCase):

    def test_replay_fixture_is_host_download_mode(self):
        """**Validates: Requirements 1.1, 1.4**"""
        lines = FIXTURE.read_bytes().decode("utf-8", errors="replace").splitlines()
        # 夹具没有到达时刻，全部记为同一时刻。末行之后 20 s 无输出，对应旧报告 caught=silence-freeze 的条件。
        events = freeze_watch.classify_stream([(0.0, line) for line in lines], end_t=20.0)
        kinds = {ev[0] for ev in events}
        self.assertIn("HOST_DOWNLOAD_MODE", kinds, f"events={events}")
        self.assertNotIn("SILENT_STOP", kinds, f"events={events}")

    def test_classification_pbt_seeded(self):
        """Property 1 的工具部分：固定种子随机日志流，至少 1000 例。**Validates: Requirements 1.1, 1.4**"""
        classify = freeze_watch.classify_stream
        rng = random.Random(PBT_SEED)
        for case in range(PBT_CASES):
            kind, records, end_t, info = _gen_case(rng)
            events = classify(records, end_t=end_t)
            problem = _expect(kind, info, events)
            if problem:
                self.fail(f"seed={PBT_SEED} case={case} kind={kind} info={info}: {problem}\n"
                          f"end_t={end_t} last_records={records[-4:]}\nevents={events}")


class SerialExclusiveOpenTest(unittest.TestCase):

    def test_second_open_of_slave_gets_ebusy(self):
        """**Validates: Requirements 2.5**"""
        import serial_excl  # 任务 13.1 新建；未修复代码上此处抛出 ModuleNotFoundError
        master, slave = os.openpty()
        port = None
        try:
            path = os.ttyname(slave)
            port = serial_excl.open_exclusive(path, 115200)
            # 不经 serial_excl 的第二次打开（如旧工具的 serial.Serial）须得到 EBUSY
            with self.assertRaises(OSError) as plain:
                os.close(os.open(path, os.O_RDWR | os.O_NOCTTY))
            self.assertEqual(plain.exception.errno, errno.EBUSY)
            # 经 serial_excl 的第二次打开同样须得到 EBUSY
            with self.assertRaises(OSError) as again:
                serial_excl.open_exclusive(path, 115200)
            self.assertEqual(again.exception.errno, errno.EBUSY)
        finally:
            if port is not None:
                port.close()
            os.close(slave)
            os.close(master)


# ---- preserve 组（任务 12）：panic 判定的保持性 ----
# 预言机是 freeze_watch.main() 中 panic 判定的原样复制。这里不引用 freeze_watch.PANIC_PAT，
# 使预言机在任务 13 改动 freeze_watch.py 之后仍代表旧规则。
_ORACLE_PANIC_PAT = ("panic", "abort()", "assert failed", "Guru Meditation",
                     "Backtrace", "Task watchdog", "Interrupt wdt")
_PRESERVE_NEAR_MISS = ("pan", "panel", "abort", "assert", "guru", "meditation",
                       "backtrack", "watchdog", "interrupt", "wdt")
_PRESERVE_NOISE = ("mixer: health ok", "app: tick", "lv_efc: perf fps=9 changed=412x194",
                   "wifi: got ip 192.168.1.20", "sntp: synced", "alive", "中文日志",
                   "İstanbul ünïcödé", "\ufffd")
_PRESERVE_PRINTABLE = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                       "0123456789 .,=-_/()[]{}'\"!?#%&*+@")


def _oracle_panic(log: str) -> bool:
    """freeze_watch.main() 中 panic 判定的原样复制：缓冲区小写后，任一 PANIC_PAT 在末 4000 字符内即判 panic。"""
    text = log.lower()
    return any(p.lower() in text[-4000:] for p in _ORACLE_PANIC_PAT)


def _is_rom_or_download_line(line: str) -> bool:
    """ROM 行（rst/boot 码、ESP-ROM 横幅）与下载模式行（DOWNLOAD(、waiting for download）。"""
    low = line.lower()
    return any(s in low for s in ("rst:0x", "boot:0x", "esp-rom:", "download(", "waiting for download"))


def _case_shuffle(rng, text):
    return "".join(c.upper() if rng.random() < 0.5 else c.lower() for c in text)


def _preserve_line(rng, frag_p):
    """一行随机文本。每个片段以 frag_p 概率取 PANIC_PAT 的大小写随机变体，其余为近似词、噪声与随机字符。"""
    while True:
        pieces = []
        for _ in range(rng.randint(1, 6)):
            r = rng.random()
            if r < frag_p:
                pieces.append(_case_shuffle(rng, rng.choice(_ORACLE_PANIC_PAT)))
            elif r < frag_p + 0.2:
                pieces.append(rng.choice(_PRESERVE_NEAR_MISS))
            elif r < frag_p + 0.3:
                pieces.append(rng.choice(_PRESERVE_NOISE))
            else:
                pieces.append("".join(rng.choice(_PRESERVE_PRINTABLE)
                                      for _ in range(rng.randint(0, 12))))
        line = " ".join(pieces)
        if not _is_rom_or_download_line(line):
            return line


def _preserve_log(rng):
    """一条随机日志流。约 40% 为短日志（0–30 行），其余为长日志（60–300 行，超过 4000 字符窗口）。
    约一半用例在末 4000 字符边界附近插入一段 PANIC 片段，覆盖窗口切边。"""
    frag_p = rng.choice((0.0, 0.0, 0.01, 0.05))
    n_lines = rng.randint(0, 30) if rng.random() < 0.4 else rng.randint(60, 300)
    log = "\n".join(_preserve_line(rng, frag_p) for _ in range(n_lines))
    if rng.random() < 0.5:
        frag = _case_shuffle(rng, rng.choice(_ORACLE_PANIC_PAT))
        cut = min(max(len(log) - 4000 + rng.randint(-len(frag), len(frag)), 0), len(log))
        log = log[:cut] + frag + log[cut:]
    return log


class PreserveTest(unittest.TestCase):

    def test_panic_rule_matches_oracle_seeded(self):
        """Property 2 的工具部分（preserve 组）：固定种子随机日志流 1200 例，不含 ROM 行与下载模式行。
        被测实现的 panic 判定须与预言机逐例一致。F 上被测实现取预言机自身以建立基线，F' 上换成新分类器。
        **Validates: Requirements 3.4, 3.17**"""
        implementation = _oracle_panic  # F 基线；F' 换成新分类器的 panic 判定
        rng = random.Random(PBT_SEED)
        verdicts = []
        for case in range(PBT_CASES):
            log = _preserve_log(rng)
            self.assertFalse(any(_is_rom_or_download_line(line) for line in log.split("\n")),
                             f"seed={PBT_SEED} case={case}: 随机日志含 ROM 行或下载模式行")
            expected = _oracle_panic(log)
            verdicts.append(expected)
            got = implementation(log)
            if got != expected:
                self.fail(f"seed={PBT_SEED} case={case}: 被测实现判 {got}，预言机判 {expected}\n"
                          f"log_tail={log[-200:]!r}")
        self.assertGreaterEqual(len(verdicts), 1000)
        self.assertTrue(any(verdicts) and not all(verdicts), "随机日志未同时覆盖 panic 与非 panic 两类")
        print(f"\npreserve: seed={PBT_SEED} cases={len(verdicts)} "
              f"panic={sum(verdicts)} no_panic={len(verdicts) - sum(verdicts)}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
