#!/usr/bin/env python3
"""check_layers.py —— 分层依赖校验器(docs/06 §2 规则的机器执行)

扫描 firmware/components/* 与 firmware/main 的源文件 #include, 对照层表:
  - L0(cubecore/boardcfg) 全层可用;
  - 其余只允许向下紧邻一层(经对方 include/ 公开头);
  - 同层横向 include 禁止(app 层控制面走事件总线);
  - 越层/横向 → 报错退出码 1。
例外: L5 装配层 main 可 include 一切; vendored mp3_helix 豁免;
      sim/ 模拟器(后端=mock, 属 L5 同类装配)不在扫描范围。

用法: python3 tools/check_layers.py [repo_root]
"""
import re
import sys
from pathlib import Path

# ---- 层表(与 docs/06 §1 一致; 改层先改文档再改这里) ----
LAYERS = {
    "cubecore": 0, "boardcfg": 0,
    "drv_audio": 1, "drv_display": 1,
    "app_audio": 2, "app_net": 2, "speech_provider": 2,
    "app_bt": 3, "app_player": 3, "app_voice": 3, "app_ai_session": 3,
    "app_ui": 4,
}
VENDORED = {"mp3_helix"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def public_headers(components_root: Path) -> dict:
    """组件 → 公开头文件名集合(include/ 目录; app_ui 额外含 ui/ 契约头)。"""
    out = {}
    for comp in LAYERS:
        names = set()
        inc = components_root / comp / "include"
        if inc.is_dir():
            names |= {p.name for p in inc.glob("*.h")}
        if comp == "app_ui":
            ui = components_root / comp / "ui"
            if ui.is_dir():
                names |= {p.name for p in ui.glob("*.h")}
        out[comp] = names
    return out


def check(root: Path) -> list:
    errors = []
    comp_root = root / "firmware" / "components"
    pub = public_headers(comp_root)
    owner_of = {h: c for c, hs in pub.items() for h in hs}

    srcs = list(comp_root.rglob("*.c")) + list(comp_root.rglob("*.h"))
    srcs += list((root / "firmware" / "main").glob("*.c"))   # L5: 仅统计, 不校验
    for f in sorted(srcs):
        rel = f.relative_to(root)
        parts = rel.parts                    # ("firmware", "components", <comp>, ...)
        if parts[0] != "firmware" or len(parts) < 4:
            continue                         # main/*.c 走下面统一跳过
        comp = parts[2]
        if comp in VENDORED:
            continue
        my_layer = LAYERS.get(comp)
        if my_layer is None:
            continue
        for line in f.read_text(errors="ignore").splitlines():
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            owner = owner_of.get(Path(m.group(1)).name)
            if owner is None or owner == comp:
                continue                     # 自家/系统/第三方头
            if LAYERS[owner] == 0:
                continue                     # 规则 1: L0 全层可用
            if comp == "app_ui":
                # 规则 2.1: L4 只依赖 ui_model.h 契约, 禁止任何 L1-L3 固件头
                errors.append(f"{rel}:{line.strip()}: L4(app_ui) 依赖 "
                              f"L{LAYERS[owner]}({owner}) —— L4 只准用 ui_model.h 契约, "
                              f"违反 docs/06 §2.1")
                continue
            if LAYERS[owner] >= my_layer:
                errors.append(f"{rel}:{line.strip()}: L{my_layer}({comp}) → "
                              f"L{LAYERS[owner]}({owner}) 同层/向上依赖, 违反 docs/06 §2")
            elif LAYERS[owner] < my_layer - 1:
                errors.append(f"{rel}:{line.strip()}: L{my_layer}({comp}) 越层依赖 "
                              f"L{LAYERS[owner]}({owner}), 违反 docs/06 §2")
    return errors


if __name__ == "__main__":
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    errs = check(root)
    if errs:
        print(f"✗ 分层违例 {len(errs)} 处:")
        for e in errs:
            print("  " + e)
        sys.exit(1)
    print("✓ 分层依赖校验通过(include 图符合 docs/06 层表)")
