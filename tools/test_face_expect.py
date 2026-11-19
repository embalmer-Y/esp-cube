#!/usr/bin/env python3
"""表情内置片段期望值的宿主检查(任务 31.2 B2; host_tests.sh [18])。

核对 firmware/components/app_ui/ui/ui_face_expect.h 与下列事实源一致:
  - assets/face/MANIFEST.json 中 embedded=true 的 4 段: 名称集合、第 0 帧键占比、第 0 帧 lit 占比、
    文件字节数与 sha256;
  - firmware/components/app_ui/CMakeLists.txt 的 FACE_EMBED 列表;
  - firmware/components/app_ui/ui/ui_widget.c 的 ui_face_builtin_lookup() 中 strcmp 的名称;
  - 容差常量: alpha0 0.5 个百分点(500), lit 5 个百分点(500);
  - MOSTLY_KEYED 阈值(990‰)不拒绝任何内置片段。
只用标准库; 不访问设备与网络。退出码 0 = 全部通过。
"""
import hashlib
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FACE_DIR = os.path.join(REPO, "firmware", "components", "app_ui", "ui", "assets", "face")
MANIFEST = os.path.join(FACE_DIR, "MANIFEST.json")
EXPECT_H = os.path.join(REPO, "firmware", "components", "app_ui", "ui", "ui_face_expect.h")
APP_UI_CMAKE = os.path.join(REPO, "firmware", "components", "app_ui", "CMakeLists.txt")
WIDGET_C = os.path.join(REPO, "firmware", "components", "app_ui", "ui", "ui_widget.c")
MOSTLY_KEYED_PERMILLE = 990  # 设计 B3 步骤 7: 第 0 帧键占比 ≥ 990‰ 拒绝

FAILS = []
CHECKS = 0


def check(cond, message):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(message)
        print(f"  FAIL {message}")


def read_text(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def main():
    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    embedded = [c for c in manifest["clips"] if c.get("embedded")]
    names = {c["name"] for c in embedded}
    check(len(embedded) == 4 and len(names) == 4,
          f"MANIFEST embedded clips must be 4 distinct names, got {sorted(names)}")

    header = read_text(EXPECT_H)
    rows = re.findall(r'\{\s*"([a-z0-9_]+)",\s*(\d+)u,\s*(\d+)u\s*\}', header)
    row_names = [r[0] for r in rows]
    check(len(rows) == len(set(row_names)) == 4,
          f"ui_face_expect.h must hold 4 distinct rows, got {row_names}")
    check(set(row_names) == names,
          f"ui_face_expect.h names {sorted(row_names)} != MANIFEST embedded {sorted(names)}")

    by_name = {c["name"]: c for c in embedded}
    for name, key_x100, lit_x100 in rows:
        if name not in by_name:
            continue
        c = by_name[name]
        exp_key = round(c["frame0_key_permille"] * 100)
        exp_lit = round(c["frame0_lit_share_pct"] * 100)
        check(int(key_x100) == exp_key,
              f"{name}: key_permille_x100 {key_x100} != MANIFEST frame0_key_permille×100 {exp_key}")
        check(int(lit_x100) == exp_lit,
              f"{name}: lit_pct_x100 {lit_x100} != MANIFEST frame0_lit_share_pct×100 {exp_lit}")
        check(c["frame0_key_permille"] < MOSTLY_KEYED_PERMILLE,
              f"{name}: frame0_key_permille {c['frame0_key_permille']} would trip MOSTLY_KEYED")
        path = os.path.join(FACE_DIR, f"{name}.efc")
        check(os.path.isfile(path), f"{name}.efc missing")
        if os.path.isfile(path):
            data = open(path, "rb").read()
            check(len(data) == c["efc_bytes"],
                  f"{name}.efc size {len(data)} != MANIFEST efc_bytes {c['efc_bytes']}")
            check(hashlib.sha256(data).hexdigest() == c["efc_sha256"],
                  f"{name}.efc sha256 differs from MANIFEST efc_sha256")
            check(data[:4] == b"EFC1", f"{name}.efc magic is not EFC1")

    for macro, want in (("UI_FACE_ALPHA0_TOL_X100", 500), ("UI_FACE_LIT_TOL_X100", 500)):
        m = re.search(macro + r"\s+(\d+)u", header)
        check(m is not None and int(m.group(1)) == want,
              f"{macro} must be {want} (0.01 units)")

    cmake = read_text(APP_UI_CMAKE)
    # §55.20 素材出库后 FACE_EMBED 为 foreach 条件嵌入(不再是字面量列表):
    # 解析 foreach 名单 × 实际在场 .efc
    m_for = re.search(r"foreach\(f ([a-z_ ]+)\)", cmake)
    if m_for is None:
        raise SystemExit("无法解析 app_ui CMakeLists foreach 名单")
    present = {f for f in os.listdir(FACE_DIR) if f.endswith(".efc")}
    cmake_names = {n.strip() for n in m_for.group(1).split()
                   if f"{n.strip()}.efc" in present}
    check(cmake_names == names,
          f"CMakeLists FACE_EMBED {sorted(cmake_names)} != MANIFEST embedded {sorted(names)}")

    widget = read_text(WIDGET_C)
    lookup = re.search(r"\bbool ui_face_builtin_lookup\(.*?\n\}", widget, re.S)
    lookup_names = set(re.findall(r'strcmp\(name, "([a-z0-9_]+)"\)', lookup.group(0))) \
        if lookup else set()
    check(lookup_names == names,
          f"ui_widget.c ui_face_builtin_lookup names {sorted(lookup_names)} != MANIFEST {sorted(names)}")

    print(f"test_face_expect: {CHECKS - len(FAILS)}/{CHECKS} checks passed")
    if FAILS:
        print("test_face_expect: FAIL")
        return 1
    print("test_face_expect: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
