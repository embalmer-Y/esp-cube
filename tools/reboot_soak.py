#!/usr/bin/env python3
"""reboot_soak.py —— 冷热重启浸泡观察(T45 首段; docs/18 T45)

用法: python3 tools/reboot_soak.py [次数, 默认 30] [单次窗口秒, 默认 45] [--face]
      python3 tools/reboot_soak.py --selftest      (离线解析自检, 不开串口)
每次: RTS 复位 → 抓启动日志 → 判定(got ip / abort / assert / panic / 挂死)。
产出: /tmp/soak_report.json(逐次记录 + 汇总) + 终端汇总表。
T45 目标 ≥100 次; 本脚本可分批执行(报告文件累加由调用方处理)。

--face(freeze-and-face-blackscreen-fix 任务 4.4): 每次启动额外抓取表情
观测行(faceperf 的 `splash:`、`face first refresh`、`face screen check`,
ui_face 的 `face clip=` 行, uiport 的 `face reject` 行), 解析后写入该次记录的
"face" 键(结构见 face_parse())。--face 时每次收满窗口秒数(不在 got ip
后 6s 提前结束), 以覆盖 SD 就绪后的自动热载(约 17s); abort/assert/Guru
仍提前结束。原有字段(got_ip/abort/assert/guru/hang/err/tail)与判定不变,
"face" 内容不参与判定与退出码。
"""
import json
import re
import sys
import time

try:
    import serial
except ImportError:          # --selftest 不需要 pyserial
    serial = None
else:
    from serial_excl import open_exclusive  # 独占打开串口(任务 13.1), 依赖 pyserial

PORT = "/dev/ttyACM0"
BAUD = 115200


def judge(rec: dict, text: str) -> dict:
    """原有判定(逐字保持): 由启动日志文本填 got_ip/abort/assert/guru/hang/err/tail"""
    rec["got_ip"] = "got ip" in text
    rec["abort"] = "abort()" in text
    rec["assert"] = "assert failed" in text
    rec["guru"] = "Guru Meditation" in text
    rec["hang"] = not rec["got_ip"] and not rec["abort"] \
        and not rec["assert"] and not rec["guru"]
    rec["err"] = rec["abort"] or rec["assert"] or rec["guru"] or rec["hang"]
    if rec["err"]:
        rec["tail"] = text[-800:]
    return rec


# ---- 表情观测行解析(--face) ----
# 格式以固件源码的 format 串为准, 逐行来源见 _FIXTURE 的注释:
#   faceperf  firmware/main/face_perf.c  boot_print() 与开机窗挂接
#   ui_face   firmware/components/app_ui/ui/ui_widget.c  ui_face_log_loaded()
#   uiport    firmware/main/ui_port_esp.c  face_hot_load() 的 face reject
#   storage   firmware/components/app_storage/app_storage.c  sd up
# 固件已不输出的旧形式(face src 行、clip 行的 mood=、screen check 的 nonbg=、
# clip 的 load=fail)不再解析; 对应事件的现行输出见 face_parse() 的说明。
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_LOG = re.compile(r"([EWIDV]) \((\d+)\) ([A-Za-z0-9_]+): (.*)$")
_SPLASH = re.compile(r"^splash: n=(\d+) p95=(\d+\.\d)ms max=(\d+\.\d)ms "
                     r"gt100=(\d+) d=(-?\d+)\s*$")
_SPLASH_TO = re.compile(r"^splash: timeout n=(\d+) d=(-?\d+)\s*$")
_SPLASH_NOMEM = re.compile(r"^splash: summary alloc failed n=(\d+)\s*$")
_FIRST = re.compile(r"^face first refresh \+(\d+\.\d)ms\s*$")
_FIRST_TO = re.compile(r"^face first refresh: timeout\s*$")
_CHECK = re.compile(r"^face screen check: lit=(\d+\.\d)% pts=(\d+) "
                    r"frame=(-?\d+)\s*$")
_CHECK_NA = re.compile(r"^face screen check: n/a reason=(\d+) cf=(\d+)\s*$")
_CHECK_TO = re.compile(r"^face screen check: timeout loaded=(\d+)\s*$")
_BOOTWIN = re.compile(r"^boot window (on|off)\b")
_CLIP = re.compile(r"^face clip=(\S+) frames=(-?\d+) delay=(\d+) cf=(\S+) "
                   r"alpha0=(\S+) lit=(\S+)\s*$")
_REJECT = re.compile(r"^face reject name=(\S+) reason=(\S+) keep=(\S+)\s*$")


def face_parse(text: str) -> dict:
    """从一次启动的串口文本提取表情观测行。

    返回 {
      "boot_window": "on"|"off"|None,             # faceperf 开机窗是否挂上
      "splash": {"n","p95_ms","max_ms","gt100","d","ts_ms"}
              | {"timeout": True, "n", "d", "ts_ms"}
              | {"alloc_failed": True, "n", "ts_ms"} | None,
      "first_refresh": {"ms","ts_ms"} | {"timeout": True, "ts_ms"} | None,
      "screen_check": {"lit_pct","pts","frame","ts_ms"}
                    | {"na": True, "reason", "cf", "ts_ms"}
                    | {"timeout": True, "loaded", "ts_ms"} | None,
      "clips": [{"clip","frames","delay","cf","alpha0","lit","ts_ms"}
                | {"clip": "sd:<名>", "load": "fail", "reason", "keep",
                   "ts_ms"}, ...],
      "sd_up_ms": int | None,                     # storage: sd up 的时间戳(只取时间)
      "lines": [匹配到的原始行(已去 ANSI 颜色)],   # 不含 sd up 行
    }
    clips 按出现顺序: "clip" 行是每次加载成功后的 `face clip=`(开机内置片段在前,
    SD 热载在后); "load": "fail" 项来自 `face reject` 行, 其 clip 为 sd:<名>,
    名为 invalid 时固件未回显原名。alpha0 与 lit 保留固件输出的字符串
    ("NN.N%" 或 "n/a")。
    lit_pct 为 812 个采样点中亮度 ≥128 的占比(%, 一位小数), 口径见 ui_face_fmt.h。
    ts_ms 为 ESP 日志时间戳(启动以来毫秒)。splash/first_refresh/screen_check
    取第一条。`face clip=sd:<名>` 即 SD/CLI 热载完成时刻; 其 ts_ms 大于 `splash:`
    行的 ts_ms 即说明热载晚于 splash 结束(splash 行在开机窗结束后才输出)。
    """
    out = {"boot_window": None, "splash": None, "first_refresh": None,
           "screen_check": None, "clips": [], "sd_up_ms": None, "lines": []}
    # 只解析完整行: 窗口末尾被截断的半行(无换行)丢弃, 免得把 d=23 截成 d=2
    rows = _ANSI.sub("", text).replace("\r", "").split("\n")[:-1]
    for raw in rows:
        m = _LOG.search(raw)
        if m is None:
            continue
        ts, tag, msg = int(m.group(2)), m.group(3), m.group(4)
        line = raw[m.start():].rstrip()
        hit = True
        if tag == "faceperf":
            if (b := _BOOTWIN.match(msg)) and out["boot_window"] is None:
                out["boot_window"] = b.group(1)
            elif (s := _SPLASH.match(msg)) and out["splash"] is None:
                out["splash"] = {"n": int(s.group(1)),
                                 "p95_ms": float(s.group(2)),
                                 "max_ms": float(s.group(3)),
                                 "gt100": int(s.group(4)),
                                 "d": int(s.group(5)), "ts_ms": ts}
            elif (s := _SPLASH_TO.match(msg)) and out["splash"] is None:
                out["splash"] = {"timeout": True, "n": int(s.group(1)),
                                 "d": int(s.group(2)), "ts_ms": ts}
            elif (s := _SPLASH_NOMEM.match(msg)) and out["splash"] is None:
                out["splash"] = {"alloc_failed": True, "n": int(s.group(1)),
                                 "ts_ms": ts}
            elif (f := _FIRST.match(msg)) and out["first_refresh"] is None:
                out["first_refresh"] = {"ms": float(f.group(1)), "ts_ms": ts}
            elif _FIRST_TO.match(msg) and out["first_refresh"] is None:
                out["first_refresh"] = {"timeout": True, "ts_ms": ts}
            elif (c := _CHECK.match(msg)) and out["screen_check"] is None:
                out["screen_check"] = {"lit_pct": float(c.group(1)),
                                       "pts": int(c.group(2)),
                                       "frame": int(c.group(3)), "ts_ms": ts}
            elif (c := _CHECK_NA.match(msg)) and out["screen_check"] is None:
                out["screen_check"] = {"na": True, "reason": int(c.group(1)),
                                       "cf": int(c.group(2)), "ts_ms": ts}
            elif (c := _CHECK_TO.match(msg)) and out["screen_check"] is None:
                out["screen_check"] = {"timeout": True,
                                       "loaded": int(c.group(1)), "ts_ms": ts}
            else:
                hit = False
        elif tag == "ui_face":
            if c := _CLIP.match(msg):
                out["clips"].append({"clip": c.group(1),
                                     "frames": int(c.group(2)),
                                     "delay": int(c.group(3)),
                                     "cf": c.group(4), "alpha0": c.group(5),
                                     "lit": c.group(6), "ts_ms": ts})
            else:
                hit = False
        elif tag == "uiport":
            if c := _REJECT.match(msg):
                out["clips"].append({"clip": f"sd:{c.group(1)}", "load": "fail",
                                     "reason": c.group(2), "keep": c.group(3),
                                     "ts_ms": ts})
            else:
                hit = False
        elif tag == "storage" and msg.startswith("sd up"):
            if out["sd_up_ms"] is None:
                out["sd_up_ms"] = ts
            hit = False                     # 只取时间, 不存原文
        else:
            hit = False
        if hit:
            out["lines"].append(line)
    return out


def face_brief(fc: dict) -> str:
    """终端一行摘要(--face)"""
    s = fc.get("splash")
    if s is None:
        sp = "splash=none"
    elif s.get("timeout"):
        sp = f"splash=timeout n={s['n']} d={s['d']}"
    elif s.get("alloc_failed"):
        sp = f"splash=alloc_failed n={s['n']}"
    else:
        sp = (f"splash n={s['n']} p95={s['p95_ms']}ms max={s['max_ms']}ms "
              f"gt100={s['gt100']} d={s['d']}")
    f = fc.get("first_refresh")
    fr = "first=none" if f is None else (
        "first=timeout" if f.get("timeout") else f"first=+{f['ms']}ms")
    c = fc.get("screen_check")
    if c is None:
        ck = "check=none"
    elif c.get("timeout"):
        ck = f"check=timeout loaded={c['loaded']}"
    elif c.get("na"):
        ck = f"check=n/a reason={c['reason']}"
    else:
        ck = f"lit={c['lit_pct']}%"
    clips = fc.get("clips", [])
    ok = [k for k in clips if "load" not in k]           # 加载成功的 face clip= 行
    bad = [k for k in clips if k.get("load") == "fail"]  # face reject 行
    # 首个成功加载的片段是开机内置片段, 其 cf/alpha0 即开机时的素材格式
    cf = "cf=none" if not ok else f"cf={ok[0]['cf']} alpha0={ok[0]['alpha0']}"
    sd = [k["clip"] for k in ok if k["clip"].startswith("sd:")]
    out = f"{sp} | {fr} | {ck} | {cf} | clips={len(clips)}"
    if sd:
        out += f" sd={','.join(sd)}"
    if bad:
        out += " reject=" + ",".join(f"{k['clip']}({k['reason']})" for k in bad)
    return out


def one_cycle(win_s: float, face: bool = False) -> dict:
    rec = {"t": time.strftime("%H:%M:%S")}
    ser = open_exclusive(PORT, BAUD, dtr=True, rts=True)   # dtr/rts 同改造前默认值
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = True
    time.sleep(0.5)
    ser.rts = False
    ser.reset_input_buffer()
    deadline = time.time() + win_s
    buf = []
    while time.time() < deadline:
        d = ser.read(8192)
        if d:
            buf.append(d.decode("utf-8", "replace"))
            text = "".join(buf)
            if any(k in text for k in ("abort()", "assert failed",
                                       "Guru Meditation")):
                # 捕获现场后再收 2s
                t2 = time.time() + 2
                while time.time() < t2:
                    d = ser.read(8192)
                    if d:
                        buf.append(d.decode("utf-8", "replace"))
                break
            if "got ip" in text and not face:
                # 网络就绪后再收 6s 观察早期稳定性
                t2 = time.time() + 6
                while time.time() < t2:
                    d = ser.read(4096)
                    if d:
                        buf.append(d.decode("utf-8", "replace"))
                break
    ser.close()
    text = "".join(buf).replace("\r", "")
    judge(rec, text)
    if face:
        rec["face"] = face_parse(text)
    return rec


def parse_args(argv: list) -> tuple:
    """位置参数语义不变([次数] [窗口秒]); 标志 --face/--selftest 可在任意位置"""
    flags = {a for a in argv if a.startswith("--")}
    unknown = flags - {"--face", "--selftest"}
    if unknown:
        raise SystemExit(f"未知参数: {' '.join(sorted(unknown))}")
    pos = [a for a in argv if not a.startswith("--")]
    n = int(pos[0]) if len(pos) > 0 else 30
    win = float(pos[1]) if len(pos) > 1 else 45.0
    return n, win, "--face" in flags, "--selftest" in flags


# ---- 离线自检(--selftest; host_tests.sh [18]) ----
# 每条样例行按固件源码的 format 串手工展开(PRIu32→u、PRId32→d、
# FP_MSF→"%u.%ums"), 行注释给出源码位置。
_FIXTURE = (
    "ESP-ROM:esp32p4-eco2-20240710\r\n"
    # ui_widget.c:197 ui_face_log_loaded(): 开机内置片段的加载行
    "\x1b[0;32mI (1182) ui_face: face clip=smile_static frames=46 delay=100 "
    "cf=RGB565A8 alpha0=100.0% lit=12.3%\x1b[0m\r\n"
    # face_perf.c 开机窗挂接成功
    "\x1b[0;32mI (1300) faceperf: boot window on (lv_efc timer hooked)\x1b[0m\r\n"
    # face_perf.c boot_print(): splash 摘要 (d 为挂钩后的解码次数)
    "\x1b[0;32mI (4012) faceperf: splash: n=72 p95=118.4ms max=231.0ms "
    "gt100=4 d=23\x1b[0m\r\n"
    "\x1b[0;32mI (4013) faceperf: splash cputop (since boot: task abs pct):"
    "\x1b[0m\r\n"
    "\x1b[0;32mI (4013) faceperf:   IDLE1\t123\t\t40%\x1b[0m\r\n"
    # face_perf.c boot_print(): 首刷 与 屏幕自检(pts 为 UI_FACE_LIT_POINTS)
    "\x1b[0;32mI (4014) faceperf: face first refresh +12.3ms\x1b[0m\r\n"
    "\x1b[0;32mI (4015) faceperf: face screen check: lit=12.3% pts=812 "
    "frame=3\x1b[0m\r\n"
    "\x1b[0;32mI (8000) wifi: got ip:192.0.2.10\x1b[0m\r\n"
    # app_storage.c:202 sd up
    "\x1b[0;32mI (17100) storage: sd up: SD32G 30436MB sd\x1b[0m\r\n"
    # ui_widget.c:197 经 ui_port_esp.c face_hot_load() 的 SD 热载成功
    "\x1b[0;32mI (17401) ui_face: face clip=sd:idle frames=46 delay=50 "
    "cf=RGB565 alpha0=n/a lit=n/a\x1b[0m\r\n"
    # ui_port_esp.c:507 SD 热载被拒, 回退内置片段
    "\x1b[0;33mW (17402) uiport: face reject name=happy reason=BAD_FORMAT "
    "keep=smile_static\x1b[0m\r\n"
    # 同源但不解析的行: ui_widget.c:210 自检不符; ui_widget.c:239 mood 回退
    "\x1b[0;33mW (17403) ui_face: face check clip=smile_static MISMATCH "
    "alpha0=99.5% expect=100.00% lit=12.3% expect=12.50% (tol 0.5/5 pts)"
    "\x1b[0m\r\n"
    "\x1b[0;33mW (17404) ui_face: face clip fallback: mood=happy clip=happy "
    "not built in, using smile_static\x1b[0m\r\n"
)

_FIXTURE_BAD = (
    # face_perf.c boot_print(): 开机窗超时的三种行
    "W (31300) faceperf: splash: timeout n=900 d=300\n"
    "W (31301) faceperf: face first refresh: timeout\n"
    "W (31302) faceperf: face screen check: timeout loaded=0\n"
    # ui_widget.c:197: 非 RGB565A8 素材没有 alpha 平面, alpha0 与 lit 为 n/a
    "I (900) ui_face: face clip=smile_static frames=12 delay=100 "
    "cf=RGB565 alpha0=n/a lit=n/a\n"
)


def selftest() -> int:
    fails = []
    total = [0]

    def check(name, cond):
        total[0] += 1
        if not cond:
            fails.append(name)

    # 1. 正常启动(固件格式样例整段): 各字段解析
    fc = face_parse(_FIXTURE)
    check("boot_window", fc["boot_window"] == "on")
    check("splash", fc["splash"] == {"n": 72, "p95_ms": 118.4,
                                     "max_ms": 231.0, "gt100": 4, "d": 23,
                                     "ts_ms": 4012})
    check("first", fc["first_refresh"] == {"ms": 12.3, "ts_ms": 4014})
    check("check", fc["screen_check"] == {"lit_pct": 12.3, "pts": 812,
                                          "frame": 3, "ts_ms": 4015})
    check("clips", fc["clips"] == [
        {"clip": "smile_static", "frames": 46, "delay": 100,
         "cf": "RGB565A8", "alpha0": "100.0%", "lit": "12.3%", "ts_ms": 1182},
        {"clip": "sd:idle", "frames": 46, "delay": 50, "cf": "RGB565",
         "alpha0": "n/a", "lit": "n/a", "ts_ms": 17401},
        {"clip": "sd:happy", "load": "fail", "reason": "BAD_FORMAT",
         "keep": "smile_static", "ts_ms": 17402}])
    check("sd_up", fc["sd_up_ms"] == 17100)
    check("lines_n", len(fc["lines"]) == 7)
    check("lines_no_ansi", all("\x1b" not in ln for ln in fc["lines"]))
    check("lines_no_sd", all("sd up" not in ln for ln in fc["lines"]))
    check("no_cputop", all("IDLE1" not in ln for ln in fc["lines"]))
    b = face_brief(fc)
    check("brief", "p95=118.4ms" in b and "lit=12.3%" in b
          and "cf=RGB565A8 alpha0=100.0%" in b and "sd=sd:idle" in b
          and "reject=sd:happy(BAD_FORMAT)" in b)
    check("json", json.loads(json.dumps(fc, ensure_ascii=False)) == fc)

    # 2. 开机窗超时, 非 RGB565A8 素材, n/a 与 timeout 变体
    fb = face_parse(_FIXTURE_BAD)
    check("splash_to", fb["splash"] == {"timeout": True, "n": 900, "d": 300,
                                        "ts_ms": 31300})
    check("first_to", fb["first_refresh"] == {"timeout": True,
                                              "ts_ms": 31301})
    check("check_to", fb["screen_check"] == {"timeout": True, "loaded": 0,
                                             "ts_ms": 31302})
    check("clip_no_alpha", fb["clips"] == [
        {"clip": "smile_static", "frames": 12, "delay": 100, "cf": "RGB565",
         "alpha0": "n/a", "lit": "n/a", "ts_ms": 900}])
    check("bw_none", fb["boot_window"] is None)
    na = face_parse("W (5) faceperf: face screen check: n/a reason=4 cf=0\n"
                    "W (6) faceperf: boot window off: ESP_ERR_TIMEOUT\n")
    check("check_na", na["screen_check"] == {"na": True, "reason": 4,
                                             "cf": 0, "ts_ms": 5})
    check("bw_off", na["boot_window"] == "off")
    check("brief_none", face_brief(face_parse("")).startswith("splash=none"))

    # 2b. 每条固件 format 各一行(参数取边界值), 只检查对应字段
    cases = [
        # face_perf.c boot_print(): d 未挂钩时为 -1
        ("I (1) faceperf: splash: n=75 p95=41.0ms max=99.9ms gt100=0 d=-1",
         "splash", {"n": 75, "p95_ms": 41.0, "max_ms": 99.9, "gt100": 0,
                    "d": -1, "ts_ms": 1}),
        ("W (2) faceperf: splash: summary alloc failed n=900",
         "splash", {"alloc_failed": True, "n": 900, "ts_ms": 2}),
        ("W (3) faceperf: splash: timeout n=12 d=-1",
         "splash", {"timeout": True, "n": 12, "d": -1, "ts_ms": 3}),
        ("W (4) faceperf: face first refresh: timeout",
         "first_refresh", {"timeout": True, "ts_ms": 4}),
        ("I (5) faceperf: face screen check: lit=0.0% pts=812 frame=-1",
         "screen_check", {"lit_pct": 0.0, "pts": 812, "frame": -1,
                          "ts_ms": 5}),
        ("W (6) faceperf: face screen check: n/a reason=3 cf=0",
         "screen_check", {"na": True, "reason": 3, "cf": 0, "ts_ms": 6}),
        ("W (7) faceperf: face screen check: timeout loaded=1",
         "screen_check", {"timeout": True, "loaded": 1, "ts_ms": 7}),
        ("I (8) faceperf: boot window on (lv_efc timer missing)",
         "boot_window", "on"),
        ("W (9) faceperf: boot window off: ESP_ERR_NO_MEM",
         "boot_window", "off"),
        # ui_widget.c:197 在 cf 为 none 时(未加载)
        ("I (10) ui_face: face clip=laugh frames=0 delay=100 cf=none "
         "alpha0=n/a lit=n/a", "clips",
         [{"clip": "laugh", "frames": 0, "delay": 100, "cf": "none",
           "alpha0": "n/a", "lit": "n/a", "ts_ms": 10}]),
        # ui_port_esp.c:462 与 :478
        ("W (11) uiport: face reject name=invalid reason=BAD_FILE "
         "keep=current", "clips",
         [{"clip": "sd:invalid", "load": "fail", "reason": "BAD_FILE",
           "keep": "current", "ts_ms": 11}]),
        ("W (12) uiport: face reject name=idle reason=LOCK_TIMEOUT "
         "keep=current", "clips",
         [{"clip": "sd:idle", "load": "fail", "reason": "LOCK_TIMEOUT",
           "keep": "current", "ts_ms": 12}]),
        # app_storage.c:202, sdio 卡
        ("I (13) storage: sd up: SD32G 30436MB sdio", "sd_up_ms", 13),
    ]
    for line, field, want in cases:
        check(f"real:{field}:{line[:30]}",
              face_parse(line + "\n")[field] == want)

    # 3. 同类行取第一条; 空文本全空
    dup = face_parse("I (1) faceperf: face first refresh +1.0ms\n"
                     "I (2) faceperf: face first refresh +9.0ms\n")
    check("first_dup", dup["first_refresh"]["ms"] == 1.0)
    e = face_parse("")
    check("empty", e["splash"] is None and e["screen_check"] is None and
          e["clips"] == [] and e["lines"] == [] and e["sd_up_ms"] is None)

    # 4. 截断在任意字节处(窗口末尾半行): 不抛异常, 字段只会缺失不会错
    #    (半行丢弃; 如 "d=23" 截成 "d=2" 不得被接受)
    full = face_parse(_FIXTURE)
    cut_bad = []
    for cut in range(len(_FIXTURE) + 1):
        try:
            p = face_parse(_FIXTURE[:cut])
        except Exception as ex:  # noqa: BLE001
            cut_bad.append(f"cut{cut}:{ex!r}")
            continue
        for k in ("boot_window", "splash", "first_refresh", "screen_check",
                  "sd_up_ms"):
            if p[k] is not None and p[k] != full[k]:
                cut_bad.append(f"cut{cut}:{k}")
        if p["clips"] != full["clips"][:len(p["clips"])] or \
                p["lines"] != full["lines"][:len(p["lines"])]:
            cut_bad.append(f"cut{cut}:clips/lines")
    check("truncation" + (f"({cut_bad[0]})" if cut_bad else ""), not cut_bad)
    check("truncation_full", face_parse(_FIXTURE) == full)

    # 5. 原有判定逐字保持
    r = judge({}, "x got ip y")
    check("judge_ok", r["got_ip"] and not r["err"] and "tail" not in r)
    r = judge({}, "Guru Meditation Error")
    check("judge_guru", r["guru"] and r["err"] and not r["hang"]
          and r["tail"] == "Guru Meditation Error")
    r = judge({}, "got ip abort() was called")
    check("judge_abort", r["abort"] and r["err"] and not r["hang"])
    r = judge({}, "abort() was called")
    check("judge_abort_noip", r["abort"] and not r["got_ip"]
          and not r["hang"] and r["err"])
    r = judge({}, "nothing")
    check("judge_hang", r["hang"] and r["err"])
    r = judge({}, "a" * 900 + "assert failed")
    check("judge_tail", r["assert"] and len(r["tail"]) == 800)

    # 6. 参数: 位置语义不变, 标志任意位置
    check("args0", parse_args([]) == (30, 45.0, False, False))
    check("args1", parse_args(["3", "45", "--face"]) == (3, 45.0, True, False))
    check("args2", parse_args(["--face", "10"]) == (10, 45.0, True, False))
    check("args3", parse_args(["5", "20"]) == (5, 20.0, False, False))
    try:
        parse_args(["--bogus"])
        rejected = False
    except SystemExit:
        rejected = True
    check("args_unknown", rejected)

    print(f"reboot_soak selftest: {total[0] - len(fails)}/{total[0]} checks pass"
          + (f"; FAIL: {', '.join(fails[:10])}" if fails else ""))
    return 1 if fails else 0


def main() -> None:
    n, win, face, st = parse_args(sys.argv[1:])
    if st:
        sys.exit(selftest())
    if serial is None:
        raise SystemExit("需要 pyserial(pip install pyserial)")
    recs = []
    fails = 0
    for i in range(1, n + 1):
        rec = one_cycle(win, face)
        recs.append(rec)
        mark = "OK " if not rec["err"] else "ERR"
        fails += 1 if rec["err"] else 0
        print(f"[{i}/{n}] {mark} got_ip={rec['got_ip']} "
              f"abort={rec['abort']} assert={rec['assert']} "
              f"guru={rec['guru']} hang={rec['hang']}", flush=True)
        if face:
            print("  face:", face_brief(rec["face"]), flush=True)
        if rec["err"]:
            print("  tail:", rec.get("tail", "")[-300:].replace("\n", " | "))
    report = {"cycles": n, "fails": fails,
              "note": "T45 重启浸泡(分批); 开机电台关(冻结规避)",
              "records": recs}
    if face:
        report["face"] = True
    with open("/tmp/soak_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    print(f"=== soak: {n - fails}/{n} clean, {fails} 异常; "
          f"报告 /tmp/soak_report.json ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
