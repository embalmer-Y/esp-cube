#!/usr/bin/env python3
# soul_accept.py —— R3 表达区分度验收(月测三场景; docs/24 §0 R3 / S1)
#
# 判据(目标态, v2 实现的验收合同):
#   C1 无单底色 >50% 驻留(三场景各查)
#   C2 三场景可区分: 底色驻留分布两两总变差距离 TV ≥ 0.20
#   C3 ethics 可达且可恢复: abuse 场景至少一次 0→1 闩锁, 且其后有 1→0 解锁
# 用法: tools/soul_accept.py [--days N]
# 退出码 = 未达标判据数(0=全部达标)。当前为 v2 前基线(预期红),
# host_tests.sh 按 KNOWN 判据数放行; 判据由绿转红即回归。
import subprocess
import sys
import tempfile
import os
import itertools

HERE = os.path.dirname(os.path.abspath(__file__))
SCEN = ["life", "neglect", "abuse"]
DAYS = 30
TV_MIN = 0.20
FACE_SHARE_MAX = 0.50

for i, a in enumerate(sys.argv):
    if a == "--days" and i + 1 < len(sys.argv):
        DAYS = int(sys.argv[i + 1])


def run_sim(name: str, csv_path: str) -> None:
    r = subprocess.run(
        ["bash", f"{HERE}/../sim/soul_sim/run.sh",
         "--ticks", str(DAYS * 86400), "--stride", "10",
         "--scenario", f"{HERE}/../sim/soul_sim/scenarios/{name}_month.txt",
         "--csv", csv_path],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL sim run: {name}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
        raise SystemExit(9)


def parse(csv_path: str):
    """返回 (face 驻留计数 dict, ethics 闩锁/解锁事件数)"""
    dwell, prev_eth, latch_on, latch_off = {}, 0, 0, 0
    with open(csv_path) as f:
        header = f.readline().strip().split(",")
        if "ethics" not in header:
            raise SystemExit("sim CSV 缺 ethics 列(需 soul_sim >= 2026-11-19 S1)")
        i_face, i_eth = header.index("face"), header.index("ethics")
        for line in f:
            parts = line.split(",")
            face = parts[i_face]
            dwell[face] = dwell.get(face, 0) + 1
            eth = int(parts[i_eth])
            if eth == 1 and prev_eth == 0:
                latch_on += 1
            elif eth == 0 and prev_eth == 1:
                latch_off += 1
            prev_eth = eth
    return dwell, latch_on, latch_off


def main():
    fails = 0
    dists = {}
    with tempfile.TemporaryDirectory(prefix="soul_acc_") as td:
        for name in SCEN:
            p = os.path.join(td, f"{name}.csv")
            run_sim(name, p)
            dists[name] = parse(p)

    total = {n: sum(d[0].values()) for n, d in dists.items()}
    print(f"== R3 验收({DAYS} 天 × {len(SCEN)} 场景) ==")

    # C1: 无单底色 >50%
    for n in SCEN:
        top = max(dists[n][0].items(), key=lambda kv: kv[1])
        share = top[1] / total[n]
        ok = share < FACE_SHARE_MAX
        print(f"C1 {n:8s} 最大驻留 {top[0]}={share:.0%} (<{FACE_SHARE_MAX:.0%}) "
              f"{'PASS' if ok else 'FAIL'}")
        if not ok:
            fails += 1

    # C2: 两两 TV 距离
    for a, b in itertools.combinations(SCEN, 2):
        da, db = dists[a][0], dists[b][0]
        keys = set(da) | set(db)
        tv = sum(abs(da.get(k, 0) / total[a] - db.get(k, 0) / total[b])
                 for k in keys) / 2
        ok = tv >= TV_MIN
        print(f"C2 {a:8s} vs {b:8s} TV={tv:.2f} (≥{TV_MIN}) {'PASS' if ok else 'FAIL'}")
        if not ok:
            fails += 1

    # C3: abuse ethics 可达+可恢复
    _, on, off = dists["abuse"]
    ok = on >= 1 and off >= 1
    print(f"C3 abuse   ethics 闩锁×{on} 解锁×{off} (须 ≥1/≥1) {'PASS' if ok else 'FAIL'}")
    if not ok:
        fails += 1

    print(f"soul_accept: {'ALL PASS' if fails == 0 else f'{fails} 判据未达标'}")
    return fails


if __name__ == "__main__":
    sys.exit(main())
