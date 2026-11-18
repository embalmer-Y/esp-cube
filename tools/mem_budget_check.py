#!/usr/bin/env python3
"""mem_budget_check.py —— 内存预算 map 校验(docs/22 §7, 零设备)

解析双目标 GNU map, 校验:
  1. 预算静态栈符号在位且尺寸正确(docs/22 §3B 内部静态表);
  2. .dram0.bss 总量不越上限(失控静态增长哨兵);
  3. 列出 >16KB 的 .bss 单体(白名单外供人工审: 入表或移 PSRAM)。

用法: python3 tools/mem_budget_check.py [firmware_dir]
退出码 0=通过。依赖: 仅标准库(纯 map 文本解析, 不需要工具链)。
"""
import re
import sys
from pathlib import Path

# (符号前缀, 期望字节数, 说明) —— 与 docs/22 §3B / cube_tasks.h 一致
STACKS = [
    ("s_mixer_stack",    0x2000, "mixer 8192"),
    ("s_voice_stack",    0x2000, "voice 8192"),
    ("s_sess_stk",       0x3800, "aisess 14336"),
    ("s_tts_stk",        0x1800, "aitts 6144"),
    ("s_mic_stk",        0x1800, "aimic 6144"),
    ("s_cli_stack",      0x2000, "cli 8192"),
    ("s_soul_stack",     0x0C00, "soul 3072"),
    ("s_aremind_stack",  0x0C00, "aremind 3072"),
    ("s_thk_stack",      0x4000, "think 16384(既有)"),
]
# bss 总量上限(docs/22 §7; 超限=有失控静态增长, 需审表)
BSS_LIMITS = {"build-p4c5": 256 * 1024, "build-s31": 192 * 1024}
# 各目标必查集合: S31 关闭 VOICE/AI/SOUL(对应任务不编译), P4 全开
REQUIRED = {
    "build-p4c5": {s[0] for s in STACKS},
    "build-s31": {"s_mixer_stack", "s_cli_stack", "s_aremind_stack"},
}
BIG_BSS = 16 * 1024          # 单体超此值即列出供审
BIG_WHITELIST = {"s_thk_stack", "s_sess_stk"}   # 已入表的大栈

SEC_RE = re.compile(
    r"^\s+\.bss\.([A-Za-z_][\w]*)[0-9.]*\s+0x[0-9a-f]+\s+0x([0-9a-f]+)")
# 长符号名换行: 名占一行, 地址/尺寸在下一行
SEC_WRAP_RE = re.compile(r"^\s+\.bss\.([A-Za-z_][\w]*)[0-9.\s]*$")
NEXT_SZ_RE = re.compile(r"^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)")
TOT_RE = re.compile(r"^\.dram0\.bss\s+0x[0-9a-f]+\s+0x([0-9a-f]+)")


def check_map(map_path: Path) -> int:
    fails = 0
    syms: dict[str, int] = {}
    bss_total = 0
    lines = map_path.read_text(errors="replace").splitlines()
    for i, line in enumerate(lines):
        m = SEC_RE.match(line)
        if m:
            sym, size = m.group(1), int(m.group(2), 16)
            syms[sym] = syms.get(sym, 0) + size   # 同名多段(罕见)合并
        else:
            w = SEC_WRAP_RE.match(line)
            if w and i + 1 < len(lines):
                n = NEXT_SZ_RE.match(lines[i + 1])
                if n:
                    syms[w.group(1)] = syms.get(w.group(1), 0) + int(n.group(1), 16)
        t = TOT_RE.match(line)
        if t:
            bss_total = int(t.group(1), 16)

    required = REQUIRED.get(map_path.parent.name, set())
    for pref, want, desc in STACKS:
        got = syms.pop(pref, 0)
        if got == 0:
            if pref in required:
                print(f"  FAIL {map_path.parent.name}: 栈符号缺失 .bss.{pref} ({desc})")
                fails += 1
        elif got != want:
            print(f"  FAIL {map_path.parent.name}: {pref} 尺寸 {got} != {want} ({desc})")
            fails += 1

    limit = BSS_LIMITS.get(map_path.parent.name)
    print(f"  {map_path.parent.name}: dram0.bss={bss_total} (上限 {limit})")
    if limit and bss_total > limit:
        print(f"  FAIL {map_path.parent.name}: bss 超上限")
        fails += 1

    for name, size in sorted(syms.items(), key=lambda kv: -kv[1]):
        if size > BIG_BSS and name not in BIG_WHITELIST:
            print(f"  审: {map_path.parent.name} .bss.{name} = {size} (>16KB, 请入 docs/22 或移 PSRAM)")
    return fails


def main() -> int:
    fw = Path(sys.argv[1] if len(sys.argv) > 1 else "firmware")
    fails = 0
    for build in ("build-p4c5", "build-s31"):
        mp = fw / build / "esp_cube.map"
        if not mp.exists():
            print(f"  FAIL 缺 {mp}(先构建)")
            fails += 1
            continue
        fails += check_map(mp)
    print("mem_budget_check:", "PASS" if fails == 0 else f"{fails} 项 FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
