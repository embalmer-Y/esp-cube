#!/usr/bin/env python3
"""p4_cli.py —— P4(ttyACM0) CLI 命令发送(T15 续 证书灌入/配对链验证用)

用法:
  python3 tools/p4_cli.py <命令1> [命令2 ...]        # 逐条发送, 每条等回显
  python3 tools/p4_cli.py --file cmds.txt           # 按行发送(# 注释跳过)
  python3 tools/p4_cli.py --wait 0.5 <命令 ...>     # 每条后等待秒数

仅允许 ttyACM0(P4 专用口); 只写不复位。
新增选项(任务 13.7, 写在命令之前, 可与 --wait 组合):
  --ready-wait S   直连路径: 发送命令前每秒发一次 help, 直到 CLI 回复; 默认 30。超时则报错退出, 不发送命令。
  --settle S       发送命令后, 连续 S 秒没有新字节即结束读取; 默认 3.0。
  --max S          发送命令后读取上限(秒); 默认 15。
"""
import codecs
import errno
import glob
import os
import stat
import sys
import time

from serial_excl import open_exclusive

BAUD = 115200
RELAY = "/tmp/freeze_watch.cmd"            # freeze_watch 命令中继: 命名管道(任务 13.3 建立)
LIVE_LOG_GLOB = "/tmp/freeze_watch_*.log"  # freeze_watch 窗口日志(任务 13.3 的命名)
WAIT_DEFAULT = 0.8          # --wait: 每条命令的最短读取时间(实际下限 0.4 s)
READY_WAIT_DEFAULT = 30.0   # --ready-wait: 直连路径等待 CLI 回复的秒数
SETTLE_DEFAULT = 3.0        # --settle: 连续无新字节的秒数, 达到即结束读取(baseline 表在回显后约 2.5 s 才出现)
MAX_DEFAULT = 15.0          # --max: 发送命令后读取的上限秒数
READY_PROBE = b"help\n"     # 就绪探测命令
READY_DRAIN_SETTLE = 0.3    # 就绪后丢弃 help 回复其余部分: 静默秒数
READY_DRAIN_MAX = 3.0       # 同上: 上限秒数


def _open_relay():
    """打开中继写端(非阻塞)。中继 = 本用户拥有的命名管道 RELAY 且有读端(freeze_watch 运行中)。否则返回 None。"""
    try:
        st = os.stat(RELAY)
    except OSError:
        return None
    if not stat.S_ISFIFO(st.st_mode) or st.st_uid != os.getuid():
        return None
    try:
        return os.open(RELAY, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as exc:
        if exc.errno == errno.ENXIO:        # 没有读端
            return None
        raise


def _live_log():
    """freeze_watch 当前写入的窗口日志: LIVE_LOG_GLOB 中本用户拥有、修改时间最新者。没有则返回 None。"""
    best, best_t = None, -1.0
    for path in glob.glob(LIVE_LOG_GLOB):
        try:
            st = os.stat(path)
        except OSError:
            continue
        if st.st_uid == os.getuid() and st.st_mtime > best_t:
            best, best_t = path, st.st_mtime
    return best


def _log_size(path):
    try:
        return os.path.getsize(path) if path else 0
    except OSError:
        return 0


def _log_tail(path, pos):
    """日志 path 自字节偏移 pos 起的新增内容; path 为 None 或已消失时返回空。"""
    if path is None:
        return b""
    try:
        with open(path, "rb") as f:
            f.seek(pos)
            return f.read()
    except OSError:
        return b""


def _settled_chunks(read_once, settle, max_s, min_s=0.0):
    """逐块产出 read_once() 读到的新字节, 直到下列条件之一成立:
    距最后一批新字节已过 settle 秒且距开始已过 min_s 秒; 或距开始已满 max_s 秒。
    read_once() 返回本次读到的字节, 无数据时返回 b""(应在约 0.1 s 内返回)。"""
    start = last = time.time()
    while True:
        data = read_once()
        now = time.time()
        if data:
            last = now
            yield data
        if now - start >= max_s:
            return
        if now - last >= settle and now - start >= min_s:
            return


def _print_chunks(chunks):
    """按 UTF-8 增量解码后输出字节块; 块边界切开多字节字符时不会产生乱码。"""
    dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
    for chunk in chunks:
        sys.stdout.write(dec.decode(chunk))
        sys.stdout.flush()
    sys.stdout.write(dec.decode(b"", final=True))
    sys.stdout.flush()


def _has_help_echo(buf):
    """buf 中是否有独占一行的 help(CLI 对 help 的回显行)。"""
    return any(line.strip() == b"help" for line in buf.splitlines())


def _wait_ready(ser, ready_wait):
    """直连路径: 在 ready_wait 秒内每秒发一次 help, 直到 CLI 回显 help 行, 返回 True。
    回显之后丢弃 help 回复的其余部分, 使其不混入后续命令的输出。超时返回 False。调用方须在返回 False 时不发送命令。"""
    deadline = time.time() + ready_wait
    next_probe = 0.0
    tail = b""
    while time.time() < deadline:
        now = time.time()
        if now >= next_probe:
            ser.write(READY_PROBE)
            next_probe = now + 1.0
        tail = (tail + ser.read(2048))[-4096:]
        if _has_help_echo(tail):
            for _ in _settled_chunks(lambda: ser.read(2048), READY_DRAIN_SETTLE, READY_DRAIN_MAX):
                pass
            return True
    return False


def _direct_one(ser, cmd, wait, settle, max_s):
    """直连路径: 发送一条命令, 按 _settled_chunks 规则读取并输出回显与结果, 最后换行。"""
    ser.write(cmd.encode() + b"\n")
    _print_chunks(_settled_chunks(lambda: ser.read(2048), settle, max_s, max(wait, 0.4)))
    print(flush=True)


def _relay_one(fd, cmd, wait, settle, max_s):
    """经中继发送一条命令, 打印 freeze_watch 日志中该命令之后的新增输出。读取规则与直连路径相同(_settled_chunks)。"""
    path = _live_log()
    pos = _log_size(path)
    os.write(fd, cmd.encode() + b"\n")

    def read_once():
        nonlocal path, pos
        newest = _live_log()
        if newest != path:                  # 窗口日志切换: 新文件整份都在本命令之后
            path, pos = newest, 0
        data = _log_tail(path, pos)
        if data:
            pos += len(data)
        else:
            time.sleep(0.05)
        return data

    _print_chunks(_settled_chunks(read_once, settle, max_s, max(wait, 0.4)))
    print(flush=True)


def _relay_session(cmds, wait, settle, max_s):
    """中继可用时经中继发送全部命令并回显输出, 返回 True。中继不可用时返回 False, 且不发送任何命令。"""
    fd = _open_relay()
    if fd is None:
        return False
    try:
        if _live_log() is None:
            raise SystemExit(f"中继 {RELAY} 存在, 但找不到 freeze_watch 日志 {LIVE_LOG_GLOB}, 无法读回输出; 命令未发送。")
        print(f"串口被占用(EBUSY), 改走 freeze_watch 中继 {RELAY}", file=sys.stderr, flush=True)
        for cmd in cmds:
            _relay_one(fd, cmd, wait, settle, max_s)
    except OSError as exc:
        raise SystemExit(f"经中继写入失败(freeze_watch 可能已退出): {exc}") from exc
    finally:
        os.close(fd)
    return True


def main() -> None:
    args = sys.argv[1:]
    opts = {"--wait": WAIT_DEFAULT, "--ready-wait": READY_WAIT_DEFAULT,
            "--settle": SETTLE_DEFAULT, "--max": MAX_DEFAULT}
    while args and args[0] in opts:
        opts[args[0]] = float(args[1])
        args = args[2:]
    wait, ready_wait = opts["--wait"], opts["--ready-wait"]
    settle, max_s = opts["--settle"], opts["--max"]
    if args and args[0] == "--file":
        with open(args[1], encoding="utf-8") as f:
            cmds = [l.rstrip("\n") for l in f
                    if l.strip() and not l.startswith("#")]
        args = cmds
    if not args:
        raise SystemExit(__doc__)
    try:
        ser = open_exclusive("/dev/ttyACM0", BAUD, dtr=True, rts=True)  # dtr/rts 同改造前 pyserial 默认值(open 时置位)
    except OSError as exc:
        if exc.errno != errno.EBUSY:
            raise
        if _relay_session(args, wait, settle, max_s):
            return
        raise SystemExit(f"{exc.strerror}\n未找到 freeze_watch 命令中继 {RELAY}; 命令未发送。")
    with ser:
        ser.timeout = 0.1              # 读取粒度 0.1 s, settle 计时以此为准
        time.sleep(0.3)
        ser.reset_input_buffer()
        if not _wait_ready(ser, ready_wait):
            raise SystemExit(f"CLI 在 {ready_wait:g} s 内未回复 help(板子可能仍在启动); 命令未发送。")
        for cmd in args:
            _direct_one(ser, cmd, wait, settle, max_s)


if __name__ == "__main__":
    main()
