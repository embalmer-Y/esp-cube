#!/usr/bin/env python3
"""p4_cli.py 直连读取路径的 pty 测试(任务 13.7, 仅标准库)。

读写都走真实内核 pty, 计时用真实时钟, 不做 mock:
- 模拟板子(FakeBoard)在 pty 主端运行, 回显命令、回复 help, 并在 baseline 5 的回显 2.5 s 后打印 3 行统计表。
- 读取端是 pty 从端, 经 PtySerial 适配为 p4_cli 直连路径使用的 read/write/timeout 接口。

运行: python3 tools/test_p4_cli_read.py
"""
import contextlib
import io
import os
import pty
import select
import sys
import threading
import time
import tty
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p4_cli  # noqa: E402

# 与 firmware/main/cli.c 中 baseline 的 3 行输出格式相同(见 evidence/12/baseline_5_full_2.txt)
TABLE_ROWS = [
    "  internal free : min=20539 p50=20539 p95=20539 p99=20539",
    "  largest block : min=16384 p50=16384 p95=16384 p99=16384",
    "  psram free    : min=30968716 p50=30968716 p95=31012804 p99=31012804",
]
HELP_LINES = ["help", "  ncm <关键词>  网易云搜索+播放 | ncmn <n> 播第n条"]


class PtySerial:
    """pty 从端的最小 serial 适配器: read(size) 最多等 timeout 秒, 返回已到达的字节。"""

    def __init__(self, fd):
        self.fd = fd
        self.timeout = 0.1

    def read(self, size=1):
        ready, _, _ = select.select([self.fd], [], [], self.timeout)
        return os.read(self.fd, size) if ready else b""

    def write(self, data):
        return os.write(self.fd, data)


class FakeBoard:
    """pty 主端上的模拟板子。boot_delay 秒内不回应任何输入。
    回应 help: 回显 help 行并列出一条命令。
    收到 baseline 5: 立即回显, table_delay 秒后打印 3 行统计表。
    received 记录收到的每一行(不含换行)。"""

    def __init__(self, master_fd, boot_delay=0.0, table_delay=2.5):
        self.fd = master_fd
        self.boot_end = time.time() + boot_delay
        self.table_delay = table_delay
        self.received = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=10)

    def _write_line(self, text):
        os.write(self.fd, text.encode() + b"\r\n")

    def _run(self):
        buf = b""
        while not self._stop.is_set():
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            buf += os.read(self.fd, 1024)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._handle(line.rstrip(b"\r").decode("utf-8"))

    def _handle(self, line):
        self.received.append(line)
        if time.time() < self.boot_end:
            return
        if line == "help":
            for text in HELP_LINES:
                self._write_line(text)
        elif line == "baseline 5":
            self._write_line("baseline 5")
            time.sleep(self.table_delay)
            for text in TABLE_ROWS:
                self._write_line(text)


class ReadPathTest(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.addCleanup(os.close, self.master)
        self.addCleanup(os.close, self.slave)
        tty.setraw(self.slave)  # 关闭回显与行缓冲, 与串口字节流一致
        self.ser = PtySerial(self.slave)

    def _board(self, **kwargs):
        board = FakeBoard(self.master, **kwargs).start()
        self.addCleanup(board.stop)  # LIFO: 先停板子线程, 再关 pty
        return board

    def test_table_printed_2_5_s_after_command_is_fully_captured(self):
        board = self._board(table_delay=2.5)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            p4_cli._direct_one(self.ser, "baseline 5", p4_cli.WAIT_DEFAULT,
                               p4_cli.SETTLE_DEFAULT, p4_cli.MAX_DEFAULT)
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0], "baseline 5")
        self.assertEqual(lines[1:4], TABLE_ROWS)
        self.assertEqual(board.received, ["baseline 5"])

    def test_ready_wait_returns_after_cli_answers_help(self):
        board = self._board(boot_delay=2.2)
        t0 = time.time()
        self.assertTrue(p4_cli._wait_ready(self.ser, ready_wait=10))
        elapsed = time.time() - t0
        self.assertGreaterEqual(elapsed, 2.1)
        self.assertLess(elapsed, 6.0)
        self.assertTrue(all(line == "help" for line in board.received))

    def test_ready_wait_times_out_and_sends_only_help_probes(self):
        board = self._board(boot_delay=float("inf"))
        t0 = time.time()
        self.assertFalse(p4_cli._wait_ready(self.ser, ready_wait=2.5))
        elapsed = time.time() - t0
        self.assertGreaterEqual(elapsed, 2.4)
        self.assertLess(elapsed, 3.5)
        self.assertTrue(board.received and all(line == "help" for line in board.received))

    def test_settle_and_max_bound_the_read(self):
        def quiet():
            time.sleep(0.05)
            return b""

        t0 = time.time()
        self.assertEqual(list(p4_cli._settled_chunks(quiet, settle=0.5, max_s=5.0)), [])
        self.assertAlmostEqual(time.time() - t0, 0.5, delta=0.3)

        def chatty():
            time.sleep(0.1)
            return b"x"

        t0 = time.time()
        chunks = list(p4_cli._settled_chunks(chatty, settle=5.0, max_s=1.0))
        self.assertAlmostEqual(time.time() - t0, 1.0, delta=0.3)
        self.assertGreater(len(chunks), 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
