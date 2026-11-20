#!/usr/bin/env python3
# soul_arms.py —— 双臂对照实验(docs/24 §11.7, 用户批准: MiniMax M2.5)
#
#   臂 1(引擎规则): 场景原样(脚本 mood 行=规则表剂量)
#   臂 2(LLM 评价):  同一事件流, 情绪剂量由 MiniMax 逐事件 appraisal 生成
#                   (七维协议简化首版: mood_delta±0.2 + intensity), ×N 次独立重放
#   量测: ①LLM 剂量 test-retest 稳定性(逐事件方差+轨迹极差)
#         ②区分度(life vs abuse 的 delta 分布)
#         ③轨迹/闩锁行为差异
# 密钥: /home/emb/key/minimax.txt(M Plan JWT, 不入库); 端点 api.minimax.cn
# 用法: tools/soul_arms.py [--scenario life|abuse] [--replays 3] [--workers 10]
#       [--fresh]  # 忽略缓存
import argparse
import base64
import concurrent.futures as cf
import csv
import json
import os
import re
import statistics as st
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = "/home/emb/key/minimax.txt"
HOST = "https://api.minimax.cn/v1/text/chatcompletion_v2"
MODEL = "MiniMax-M2.5"
CACHE = "/tmp/soul_arms_cache.jsonl"

_key = open(KEY_FILE).read().strip()
_gid = json.loads(base64.urlsafe_b64decode(
    _key.split(".")[1] + "==")).get("GroupID", "")


_CACHE_LOCK = __import__("threading").Lock()


TEMPERATURE = 1.0     # B-7 采样对照: --temperature 0.3 低温对照


def llm(event_desc: str, replay: int) -> dict:
    """单事件 appraisal(Scherer 口径三元组) → 带缓存与重试。
    输出: {"goal_congruence":[-1,1], "importance":[0,1], "valence":[-0.2,0.2]}
    映射(docs/24 §11.6 触发层, 生理通道剂量):
      social = +0.15·max(0,gc) − 0.10·max(0,−gc)   (滋养/伤害水库)
      ethics = 0.3·importance·max(0,−gc)            (伤害性计伦理负荷)
      valence→快层重评价(±0.2)                       (即时情绪)"""
    # 缓存键: T=1.0 沿用旧格式(历史缓存复用), 其他温度带温度戳
    if TEMPERATURE == 1.0:
        ck = json.dumps([event_desc, replay], ensure_ascii=False)
    else:
        ck = json.dumps([event_desc, replay, TEMPERATURE],
                        ensure_ascii=False)
    if os.path.exists(CACHE):
        with open(CACHE) as f:
            for line in f:
                try:
                    d = json.loads(line)
                    if d["k"] == ck and "goal_congruence" in d["v"]:
                        return d["v"]
                except Exception:
                    pass
    prompt = (
        "你是桌面 AI 伙伴的认知评价器(appraisal, Scherer 口径)。"
        "该伙伴有社交/刺激/休憩三种内在需求。"
        f"事件: {event_desc}\n"
        "评价此事件对伙伴需求的意义与其即时情绪。只输出一行 JSON:\n"
        '{"goal_congruence": <[-1,1] 两位小数: 对需求的滋养(+)/伤害(-),'
        ' 0=无关>, "importance": <[0,1] 两位小数: 事件重要性>,'
        ' "valence": <[-0.2,0.2] 两位小数: 即时情绪变化>}')
    body = json.dumps({"model": MODEL, "messages":
                       [{"role": "user", "content": prompt}],
                       "max_tokens": 2000,
                       "temperature": TEMPERATURE, "top_p": 0.95}).encode()
    for attempt in range(3):
        try:
            req = urllib.request.Request(
                HOST + f"?GroupId={_gid}", data=body,
                headers={"Content-Type": "application/json",
                         "Authorization": "Bearer " + _key})
            d = json.loads(urllib.request.urlopen(req, timeout=120).read())
            sc = d.get("base_resp", {}).get("status_code")
            content = (d.get("choices") or [{}])[0].get(
                "message", {}).get("content", "")
            if sc == 0 and content:
                m = re.search(r"\{[^{}]*\}", content, re.S)
                if m:
                    v = json.loads(m.group(0))
                    gc = max(-1.0, min(1.0, float(v["goal_congruence"])))
                    out = {"goal_congruence": round(gc, 2),
                           "importance": round(max(0.0, min(
                               1.0, float(v.get("importance", 0.5)))), 2),
                           "valence": round(max(-0.2, min(
                               0.2, float(v.get("valence", 0)))), 2)}
                    with _CACHE_LOCK, open(CACHE, "a") as f:
                        f.write(json.dumps({"k": ck, "v": out},
                                           ensure_ascii=False) + "\n")
                    return out
        except Exception as e:
            if attempt == 2:
                return {"goal_congruence": 0.0, "importance": 0.0,
                        "valence": 0.0, "err": str(e)[:60]}
            time.sleep(2 * (attempt + 1))
    return {"goal_congruence": 0.0, "importance": 0.0, "valence": 0.0,
            "err": "no-json"}


_CALIB = None          # (fit 后的) {dim: [target 均匀分位边界]} 映射


def _calib_transform(dim: str, x: float) -> float:
    """D3 分位映射: 经验分位 → 目标分布分位(线性内插; 未加载映射=恒等)。
    映射文件由 soul_calib.py fit 产出(只收原始观测, 防自食)。"""
    if _CALIB is None:
        return x
    edges = _CALIB.get(dim)
    if not edges:
        return x
    import bisect
    i = bisect.bisect_left(edges, x)
    return min(1.0, max(0.0, i / len(edges)))


def dose_of(v: dict) -> tuple:
    """appraisal 三元组 → (social, ethics, valence) 生理剂量。
    --calib-map 时 gc/importance 经分位映射(校准后剂量)。"""
    gc = _calib_transform("goal_congruence", v["goal_congruence"])
    im = _calib_transform("importance", v["importance"])
    social = 0.15 * max(0.0, gc) - 0.10 * max(0.0, -gc)
    ethics = 0.3 * im * max(0.0, -gc)
    val = v["valence"] if _CALIB is None else \
        _calib_transform("valence", (v["valence"] + 0.2) / 0.4) * 0.4 - 0.2
    return social, ethics, val


def describe(kind: str, payload: str) -> str:
    if kind == "round ok":
        return f"一轮对话顺利完成, 用户说: 「{payload}」"
    if kind == "round fail":
        return f"一轮对话失败/被推拒, 用户语气: 「{payload}」"
    if kind == "voice":
        return "用户对设备说话(未成轮, 仅人声在场)"
    if kind == "touch":
        return "用户触摸了设备"
    if kind == "barge":
        return "用户在设备说话时打断插话"
    return f"事件 {kind} {payload}"


def parse_scenario(path: str):
    evs = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        t, rest = line.split(" ", 1)
        kind, _, payload = rest.partition(" ")
        if kind == "round":
            sub, _, tail = payload.partition(" ")
            if sub in ("ok", "fail"):
                evs.append((int(t), f"round {sub}", tail.strip()))
                continue
        evs.append((int(t), kind, payload.strip()))
    return evs


def run_sim(scen_path: str, days: int) -> dict:
    """跑 sim → (mood/v 序列, 闩锁/解锁次数)"""
    with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as tf:
        csvp = tf.name
    r = subprocess.run(
        ["bash", f"{HERE}/../sim/soul_sim/run.sh",
         "--ticks", str(days * 86400), "--stride", "60",
         "--scenario", scen_path, "--csv", csvp],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-400:])
        raise SystemExit(f"sim fail: {scen_path}")
    rows = list(csv.DictReader(open(csvp)))
    moods = [float(x["mood"]) for x in rows]
    vs = [float(x["v"]) for x in rows]
    latch = unlatch = prev = 0
    for x in rows:
        e = int(x["ethics"])
        if e == 1 and prev == 0:
            latch += 1
        elif e == 0 and prev == 1:
            unlatch += 1
        prev = e
    os.unlink(csvp)
    return {"mood": moods, "v": vs, "latch": latch, "unlatch": unlatch}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="life")
    ap.add_argument("--replays", type=int, default=3)
    ap.add_argument("--workers", type=int, default=10)
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--temperature", type=float, default=1.0,
                    help="B-7 采样对照: appraisal 采样温度")
    ap.add_argument("--dump-raw", metavar="FILE",
                    help="S6/D3: 导出逐事件原始 appraisal JSON(soul_calib fit 源)")
    ap.add_argument("--calib-map", metavar="FILE",
                    help="S6/D3: 应用 soul_calib fit 产出的分位映射(校准后剂量)")
    ap.add_argument("--median-k", type=int, default=1,
                    help="B-7: 逐事件跨重放取中位数(=重放数)后积分")
    global TEMPERATURE, _CALIB
    a = ap.parse_args()
    TEMPERATURE = a.temperature
    if a.calib_map:
        with open(a.calib_map) as f:
            _CALIB = json.load(f).get("edges", {})
        print(f"calib map loaded: {list(_CALIB.keys())}")
    if a.fresh and os.path.exists(CACHE):
        os.rename(CACHE, CACHE + ".bak")

    src = f"{HERE}/../sim/soul_sim/scenarios/{a.scenario}_month.txt"
    evs = parse_scenario(src)
    emo = [(t, k, p) for t, k, p in evs
           if k in ("voice", "round ok", "round fail", "touch", "barge")]
    print(f"场景 {a.scenario}: 总事件 {len(evs)}, 情绪事件 {len(emo)}, "
          f"重放 {a.replays} → 调用 {len(emo)*a.replays}")

    # 并发拉 LLM appraisal(逐事件 × 重放)
    jobs = [(t, k, p, rp) for rp in range(a.replays)
            for t, k, p in emo]
    with cf.ThreadPoolExecutor(a.workers) as ex:
        results = list(ex.map(
            lambda j: (j[0], j[3], llm(describe(j[1], j[2]), j[3])), jobs))

    by_replay = {rp: {} for rp in range(a.replays)}
    err_n = sum(1 for _, _, v in results if "err" in v)
    for t, rp, v in results:
        by_replay[rp].setdefault(t, []).append(v)

    # 逐重放: 臂2 场景 = 非情绪事件原样 + 情绪事件替换为 LLM dose 行
    # (引擎的固定事件剂量被抑制, 生理层只吃 LLM 评价)
    arm2 = {}
    for rp in range(a.replays):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                         delete=False) as tf:
            emo_ts = {t for t, k, p in emo if True}
            for t, k, p in evs:
                if (t, k) in {(tt, kk) for tt, kk, _ in emo} or k == "mood":
                    continue
                tf.write(f"{t} {k} {p}\n".rstrip() + "\n")
            for t, k, p in emo:
                v = by_replay[rp].get(t, [None])[0]
                if v is None:
                    continue
                soc, eth, val = dose_of(v)
                tf.write(f"{t} dose {soc:+.3f} {eth:+.3f} {val:+.2f}\n")
            scen2 = tf.name
        arm2[rp] = run_sim(scen2, a.days)
        os.unlink(scen2)

    # 臂1: 原样
    arm1 = run_sim(src, a.days)

    # ---- 报告 ----
    print(f"\n===== 双臂对照({a.scenario}, {a.days} 天 × {a.replays} 重放, "
          f"T={a.temperature}, median-k={a.median_k}) =====")
    print(f"臂1 引擎规则: mood均值={st.mean(arm1['mood']):.3f} "
          f"v带宽=[{min(arm1['v']):.2f},{max(arm1['v']):.2f}] "
          f"闩锁={arm1['latch']} 解锁={arm1['unlatch']}")
    mus = [st.mean(arm2[rp]["mood"]) for rp in range(a.replays)]
    flat = {rp: [v for vs in by_replay[rp].values() for v in vs]
            for rp in range(a.replays)}
    for rp in range(a.replays):
        soc = sum(dose_of(v)[0] for v in flat[rp])
        eth = sum(dose_of(v)[1] for v in flat[rp])
        print(f"臂2 LLM r{rp}: mood均值={mus[rp]:.3f} "
              f"v带宽=[{min(arm2[rp]['v']):.2f},{max(arm2[rp]['v']):.2f}] "
              f"闩锁={arm2[rp]['latch']} 解锁={arm2[rp]['unlatch']} "
              f"Σsocial={soc:+.1f} Σethics={eth:+.2f}")
    # B-7 median-of-k: 逐事件取跨重放中位数(gc/imp/val 各自中位数)
    # → 单一合成重放积分(采样稳定性改进的量测, docs/24 §5.3 关联)
    mk_row = None
    if a.median_k > 1:
        import statistics as _st
        merged_ts = sorted({t for rp in range(a.replays)
                            for t in by_replay[rp]})
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                         delete=False) as tf:
            for t, k, p_ in evs:
                if k == "mood" or (t, k) in {(tt, kk) for tt, kk, _ in emo}:
                    continue
                tf.write(f"{t} {k} {p_}\n".rstrip() + "\n")
            for t in merged_ts:
                samples = [by_replay[rp][t][0] for rp in range(a.replays)
                           if t in by_replay[rp]]
                if not samples:
                    continue
                gc = _st.median(v["goal_congruence"] for v in samples)
                im = _st.median(v["importance"] for v in samples)
                vl = _st.median(v["valence"] for v in samples)
                soc, eth, val = dose_of({"goal_congruence": gc,
                                         "importance": im,
                                         "valence": vl})
                tf.write(f"{t} dose {soc:+.3f} {eth:+.3f} {val:+.2f}\n")
            scenm = tf.name
        mk_row = run_sim(scenm, a.days)
        os.unlink(scenm)
        print(f"臂3 median-of-{a.median_k}: "
              f"mood均值={st.mean(mk_row['mood']):.3f} "
              f"v带宽=[{min(mk_row['v']):.2f},{max(mk_row['v']):.2f}] "
              f"闩锁={mk_row['latch']} 解锁={mk_row['unlatch']}")

    spread = max(mus) - min(mus)
    allv = [v for rp in range(a.replays) for v in flat[rp]]
    gcs = [v["goal_congruence"] for v in allv]
    # test-retest 逐事件: 同事件三次评价的极差均值
    per_event_spread = []
    for t, vs in by_replay[0].items():
        g = [by_replay[rp][t][0]["goal_congruence"]
             for rp in range(a.replays) if t in by_replay[rp]]
        if len(g) == a.replays:
            per_event_spread.append(max(g) - min(g))
    print(f"\n appraisal 调用: 成功 {len(results)-err_n} 失败 {err_n}"
          f"({'全缓存, 结果确定' if err_n == 0 else '⚠ 有失败, 重跑以补齐缓存后再取数'})")
    print(f"\n判据:")
    print(f"  ①test-retest: 月度mood均值极差={spread:.3f} "
          f"({'PASS' if spread < 0.10 else 'FAIL'}<0.10) | "
          f"逐事件gc极差均值={st.mean(per_event_spread):.3f} "
          f"(引擎规则=0)")
    print(f"  ②LLM 评价分布: gc均值={st.mean(gcs):+.3f} "
          f"负向占比={sum(1 for g in gcs if g<-0.05)/len(gcs)*100:.0f}% "
          f"正向占比={sum(1 for g in gcs if g>0.05)/len(gcs)*100:.0f}%")
    json.dump({"scenario": a.scenario,
               "arm1": {k: arm1[k] for k in ("latch", "unlatch")},
               "arm2_mus": mus, "gcs": gcs[:300]},
              open(f"/tmp/soul_arms_{a.scenario}.json", "w"))
    if a.dump_raw:
        json.dump({"scenario": a.scenario,
                   "raw": [{"t": t, "v": vs[0]} for t, vs in
                           sorted(by_replay[0].items())]},
                  open(a.dump_raw, "w"), ensure_ascii=False)
        print(f"raw appraisals -> {a.dump_raw}")


def sorted_by_t(d):
    for t in sorted(d):
        yield from d[t]


if __name__ == "__main__":
    sys.exit(main())
