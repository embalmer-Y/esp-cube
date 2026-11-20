#!/usr/bin/env python3
# soul_calib.py —— soul_calib 校准层(docs/24 §5.3, D3; S2.5b)
#
# per-model per-dimension 分位数校准器: LLM appraisal 原始观测 → 经验
# CDF → 引擎自身 delta 历史分布的逆 CDF(目标分布=引擎 1Hz 免费先验;
# Dirichlet 强度 50 平滑 10 分位箱; min_samples=300 前与均匀分布线性
# 混合)。**只收原始观测不收校准后值**(防自食, §5.3)。
#
# 用法:
#   python3 tools/soul_calib.py selftest          # 合成数据自检(host_tests 用)
#   python3 tools/soul_calib.py psi A.json B.json # 两批观测漂移检查
#
# 接入点(B-4/S6): LLM dose_hint 进引擎前先 transform(); 置信三档按
# 校准后分位 (<0.5 / 0.5-0.8 / >0.8); PSI≥0.10 调查 / ≥0.25 再校准。
import json
import math
import sys

MIN_SAMPLES = 300
DIRICHLET = 50.0     # 平滑强度(伪计数/箱)
BINS = 10
PSI_INVESTIGATE = 0.10
PSI_RECALIBRATE = 0.25


def _ecdf(samples):
    """经验 CDF: 返回排序样本表; transform 用 (i+0.5)/n 分位插值"""
    s = sorted(float(x) for x in samples)
    return s


def _ecdf_pmf(sorted_s, x):
    """P(X ≤ x) 线性插值(0,1 端饱和)"""
    n = len(sorted_s)
    lo, hi = 0, n
    # 二分找插入位
    while lo < hi:
        mid = (lo + hi) // 2
        if sorted_s[mid] < x:
            lo = mid + 1
        else:
            hi = mid
    # lo = 第一个 ≥x 的下标; 分位=lo/n 加插值
    if n == 0:
        return 0.5
    if lo == 0:
        q = 0.0 if x < sorted_s[0] else 1.0 / n
    elif lo == n:
        q = 1.0
    else:
        q = (lo + (x - sorted_s[lo - 1]) /
             (sorted_s[lo] - sorted_s[lo - 1] + 1e-12)) / n
    return min(1.0, max(0.0, q))


def _quantile_from_hist(hist, q):
    """Dirichlet 平滑直方图(伪计数)的逆 CDF: 返回箱中心加权分位"""
    total = sum(hist) + DIRICHLET
    acc = 0.0
    for i, h in enumerate(hist):
        p = (h + DIRICHLET / BINS) / total
        acc += p
        if acc >= q:
            return (i + 0.5) / BINS     # 归一化箱中心([0,1] 值域)
    return 1.0


class QuantileCalibrator:
    """raw 分布 → 目标分布(引擎 delta 历史)的分位数映射"""

    def __init__(self, target_hist):
        """target_hist: 长度 BINS 的箱计数(值域 [0,1] 10 等分箱)"""
        if len(target_hist) != BINS:
            raise ValueError(f"target_hist 须 {BINS} 箱")
        self.target_hist = [float(h) for h in target_hist]
        self.raw_sorted = None

    def fit(self, raw_samples):
        self.raw_sorted = _ecdf(raw_samples)
        return self

    def transform(self, x):
        """x(原始观测) → 校准值([0,1] 目标分布域)。
        min_samples 不足: 与均匀(=恒等)线性混合, 过渡平滑。"""
        if self.raw_sorted is None:
            return min(1.0, max(0.0, float(x)))
        q = _ecdf_pmf(self.raw_sorted, float(x))
        n = len(self.raw_sorted)
        target = _quantile_from_hist(self.target_hist, q)
        if n < MIN_SAMPLES:
            w = n / MIN_SAMPLES          # 线性混合: 样本越少越靠恒等
            return (1 - w) * min(1.0, max(0.0, float(x))) + w * target
        return target

    def confidence_tier(self, x_cal):
        """置信三档(§5.3): 校准后分位 <0.5 / 0.5-0.8 / >0.8"""
        if x_cal < 0.5:
            return "low"
        if x_cal < 0.8:
            return "mid"
        return "high"


def histogram(samples, lo=0.0, hi=1.0):
    """样本 → BINS 箱计数(值域 [lo,hi]; 越界钳入)"""
    hist = [0] * BINS
    for x in samples:
        x = min(hi - 1e-9, max(lo, float(x)))
        idx = int((x - lo) / (hi - lo) * BINS)
        hist[idx] += 1
    return hist


def psi(samples_a, samples_b):
    """PSI(Population Stability Index, 10 分位箱)。
    ≥0.10 调查 / ≥0.25 再校准(§5.3; 零计数 Dirichlet 平滑防 inf)"""
    qs = [i / BINS for i in range(1, BINS)]
    def _cuts(samples):
        s = sorted(float(x) for x in samples)
        return [s[min(len(s) - 1, int(q * len(s)))] for q in qs]
    cuts = _cuts(samples_b)              # 参照分布分位切点
    def _binrate(samples, cuts):
        n = len(samples)
        rates = []
        prev = None
        for c in cuts + [None]:
            cnt = sum(1 for x in samples
                      if (prev is None or x > prev) and (c is None or x <= c))
            rates.append((cnt + DIRICHLET / BINS) / (n + DIRICHLET))
            prev = c
        return rates
    ra = _binrate([float(x) for x in samples_a], cuts)
    rb = _binrate([float(x) for x in samples_b], cuts)
    return sum((a - b) * math.log(a / b) for a, b in zip(ra, rb))


def selftest():
    """合成数据锚(确定性; host_tests 门禁)"""
    fails = 0
    ok = lambda c, msg: (print(f"  {'PASS' if c else 'FAIL'} {msg}"),
                         0 if c else fails + 1)[1]

    # 1) 恒等性: raw 与目标同分布 → transform≈恒等
    import random
    rnd = random.Random(7)
    raw = [rnd.random() for _ in range(500)]
    cal = QuantileCalibrator(histogram(raw)).fit(raw)
    errs = [abs(cal.transform(x) - x) for x in raw]
    fails = ok(max(errs) < 0.12, f"同分布恒等性 max|err|={max(errs):.3f}<0.12")

    # 2) 偏态压缩: raw 集中 [0,0.2], 目标均匀 → transform 拉开到全域
    raw2 = [rnd.random() * 0.2 for _ in range(500)]
    cal2 = QuantileCalibrator(histogram([rnd.random() for _ in range(500)]))\
        .fit(raw2)
    spread = cal2.transform(0.15) - cal2.transform(0.01)
    fails = ok(spread > 0.4, f"偏态映射展开 spread={spread:.2f}>0.4")

    # 3) min_samples 混合: 50 样本 → 输出靠向恒等
    cal3 = QuantileCalibrator(histogram(raw)).fit(raw2[:50])
    mid = cal3.transform(0.1)
    fails = ok(0.0 < mid < 0.5, f"少样本混合 mid={mid:.2f}∈(0,0.5)")

    # 4) 单调性
    mono = all(cal2.transform(a) <= cal2.transform(b) + 1e-9
               for a, b in [(0.01, 0.05), (0.05, 0.1), (0.1, 0.15)])
    fails = ok(mono, "单调性")

    # 5) 置信三档
    tiers = {cal.transform(0.9): None}
    t_low = cal.confidence_tier(cal.transform(0.1))
    t_high = cal.confidence_tier(cal.transform(0.95))
    fails = ok(t_low == "low" and t_high == "high",
               f"置信三档 low={t_low} high={t_high}")

    # 6) PSI: 同分布≈0, 漂移>0.10
    same = [rnd.random() for _ in range(2000)]
    drifted = [rnd.random() * 0.5 + 0.5 for _ in range(2000)]
    p_same = psi(same[1000:], same[:1000])
    p_drift = psi(drifted, same)
    fails = ok(p_same < 0.10, f"PSI 同分布={p_same:.3f}<0.10")
    fails = ok(p_drift > 0.25, f"PSI 漂移={p_drift:.3f}>0.25")

    print("soul_calib selftest:", "ALL PASS" if fails == 0 else f"{fails} FAIL")
    return 1 if fails else 0


def _load_list(path):
    with open(path) as f:
        d = json.load(f)
    return d if isinstance(d, list) else d.get("samples", [])


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        return selftest()
    if len(sys.argv) >= 4 and sys.argv[1] == "psi":
        a, b = _load_list(sys.argv[2]), _load_list(sys.argv[3])
        p = psi(a, b)
        lvl = ("OK" if p < PSI_INVESTIGATE else
               "调查" if p < PSI_RECALIBRATE else "再校准")
        print(f"PSI={p:.3f} → {lvl}")
        return 0 if p < PSI_RECALIBRATE else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
