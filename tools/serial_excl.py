"""独占打开串口（freeze-and-face-blackscreen-fix 任务 13.1）。

open_exclusive(port, baud, dtr=False, rts=False) -> serial.Serial
  先构造未打开的 serial.Serial 并设好 dtr/rts，再 open()，随后 fcntl.ioctl(fd, TIOCEXCL)。
  dtr/rts 由 pyserial 在 open() 时下发，故须先于 open() 设置。
  TIOCEXCL 使非 root 进程的后续 open() 得到 EBUSY。pyserial 自带的 exclusive=True 是 flock 建议锁，
  不阻止其他进程 open()，所以这里不用它。
  端口被占用时抛出 errno 为 EBUSY 的 OSError，消息提示用 `fuser -v <port>` 查看占用者。
"""

import errno
import fcntl
import termios

import serial


def _busy_message(port):
    return f"串口 {port} 已被其他进程占用（EBUSY），请运行 `fuser -v {port}` 查看占用者。"


def open_exclusive(port, baud, dtr=False, rts=False):
    """打开 port 并设为独占，返回已打开的 serial.Serial。端口被占用时抛出 errno 为 EBUSY 的 OSError。"""
    ser = serial.Serial()  # 构造时不打开端口
    ser.port = port
    ser.baudrate = baud
    ser.dtr = dtr
    ser.rts = rts
    try:
        ser.open()
    except OSError as exc:  # serial.SerialException 继承自 OSError，errno 由 pyserial 填入
        if exc.errno == errno.EBUSY:
            raise OSError(errno.EBUSY, _busy_message(port)) from exc
        raise
    try:
        fcntl.ioctl(ser.fileno(), termios.TIOCEXCL)
    except OSError:
        ser.close()
        raise
    return ser
