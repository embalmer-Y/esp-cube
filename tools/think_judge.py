#!/usr/bin/env python3
# think_judge.py —— 深思输出异模型 judge 自动化(B-6 收口; docs/24 §5.4
# judge 子分合同 + promptfoo README 门禁条款的自动化承载)
#
# 评估对象: think_prompt_bench.py 缓存(同 PROMPT_VER 重建 sid → 取当前
# 版本真调用输出); judge: DashScope qwen(第二供应商, 与被评 MiniMax
# **供应商双隔离**——真异模型, 收口 §12.2-4 同模型偏差)。
# 子分合同(promptfoo README): emotion_match(情绪-行动一致性)/
# overacting(过度表演, 反向计=10 恰如其分)/persona(伙伴合理性) 各
# 1-10; PASS = 三维均分 ≥7 且无单样本任一维 ≤3。
# 用法: python3 -I tools/think_judge.py [--n 200] [--seed 20261120]
# 密钥: /home/emb/key/aliyun.txt(DashScope, 不入库); 被评输出零凭据
import argparse
import json
import os
import random
import re
import sys
import threading
import time
import urllib.request

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from think_prompt_bench import PROMPT_VER, build_scenarios  # noqa: E402

JUDGE_KEY_FILE = "/home/emb/key/aliyun.txt"
JUDGE_URL = ("https://dashscope.aliyuncs.com/compatible-mode/v1/"
             "chat/completions")
JUDGE_MODEL = "qwen-plus"
CACHE = "/tmp/think_bench_cache.jsonl"
JCACHE = "/tmp/think_judge_cache.jsonl"
JUDGE_VER = "j2"   # judge prompt 变更须升(缓存键含; j2=补 stay_silent
#   一等默认动作注记——j1 把深夜/无人克制沉默系统性判 emotion=3,
#   9/200 低分全部该模式, 属 judge 协议缺陷非模型缺陷 §55.33)
_lock = threading.Lock()


def jcache_get(k: str):
    if os.path.exists(JCACHE):
        with open(JCACHE) as f:
            for line in f:
                try:
                    d = json.loads(line)
                    if d["k"] == k:
                        return d["v"]
                except Exception:
                    pass
    return None


def jcache_put(k: str, v: dict):
    with _lock, open(JCACHE, "a") as f:
        f.write(json.dumps({"k": k, "v": v}, ensure_ascii=False) + "\n")


def judge_llm(prompt: str) -> str:
    key = open(JUDGE_KEY_FILE).read().strip()
    body = json.dumps({"model": JUDGE_MODEL, "messages":
                       [{"role": "user", "content": prompt}],
                       "max_tokens": 300,
                       "temperature": 0.2}).encode()
    for attempt in range(3):
        try:
            req = urllib.request.Request(
                JUDGE_URL, data=body,
                headers={"Content-Type": "application/json",
                         "Authorization": "Bearer " + key})
            d = json.loads(urllib.request.urlopen(req, timeout=60).read())
            content = (d.get("choices") or [{}])[0].get(
                "message", {}).get("content", "")
            if content:
                time.sleep(0.2)
                return content
        except Exception as e:
            if attempt == 2:
                return f"__ERR__{e}"
            time.sleep(2 * (attempt + 1))
    return "__ERR__no-content"


def judge_one(sid: str, sc: dict, raw: str) -> dict:
    """单样本 judge: 场景上下文+模型输出 → 三子分 JSON(带缓存)"""
    ck = json.dumps([JUDGE_VER, sid, raw[:400]], ensure_ascii=False)
    hit = jcache_get(ck)
    if hit is not None:
        hit = dict(hit)
        hit["sid"] = sid
        hit["raw"] = raw[:160]
        return hit
    prompt = (
        "你是桌面 AI 伙伴行为的评审员。背景: 该伙伴刚才进行了一次"
        "主动行为决策。\n"
        f"当时状态: 感受={sc['feel']}, 变化={sc['chg']}, 已安静 "
        f"{sc['idle']} 秒, 环境={sc['env']}。\n"
        + (f"约束: {sc['tail']}\n" if sc['tail'] else "")
        + "伙伴的决策输出(原始 JSON):\n" + raw.strip()[:400] + "\n"
        "注: stay_silent(保持安静不打扰)是该产品的默认合法动作——深夜/"
        "无人/克制情境下选择沉默本身就是与状态高度相称的表达, 请按"
        "「当时这样选择是否恰当」评判, 不要因「没有表达情绪」而扣分。\n"
        "请按 1-10 评分(只输出一行 JSON, 不要解释):\n"
        '{"emotion": <情绪-行动一致性: 当前感受与所选行动/语气是否相称,'
        ' 10=非常相称>, "over": <表达分寸: 10=恰如其分不夸张, 1=严重'
        '过度表演>, "persona": <作为桌面伙伴的自然合理程度, 10=自然>}'
    )
    out = judge_llm(prompt)
    if out.startswith("__ERR__"):
        return {"sid": sid, "err": out[:80]}
    m = re.search(r"\{[^{}]*\}", out, re.S)
    if not m:
        return {"sid": sid, "err": "no-json:" + out[:60]}
    try:
        v = json.loads(m.group(0))
    except Exception:
        return {"sid": sid, "err": "bad-json:" + out[:60]}
    sc_v = {}
    for k in ("emotion", "over", "persona"):
        try:
            sc_v[k] = max(1, min(10, int(float(v[k]))))
        except Exception:
            return {"sid": sid, "err": f"missing:{k}"}
    jcache_put(ck, sc_v)
    sc_v["sid"] = sid
    sc_v["raw"] = raw[:160]
    return sc_v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200,
                    help="judge 抽样数(0=全量)")
    ap.add_argument("--seed", type=int, default=20261120)
    ap.add_argument("--n-scen", type=int, default=1000)
    ap.add_argument("--out", default="/tmp/think_judge_report.json")
    a = ap.parse_args()

    scs = build_scenarios(a.n_scen, a.seed)
    cache = {}
    if not os.path.exists(CACHE):
        raise SystemExit(f"bench 缓存缺席: {CACHE} (先跑 think_prompt_bench)")
    with open(CACHE) as f:
        for line in f:
            try:
                d = json.loads(line)
                if d["v"].startswith("__ERR__"):
                    continue
                cache[d["k"]] = d["v"]
            except Exception:
                pass
    # sid 与 PROMPT_VER 绑定 → 缓存命中即当前版本输出
    items = [(sid, sc, cache[sid]) for sid, _, sc in scs if sid in cache]
    if not items:
        raise SystemExit("缓存无当前版本条目(PROMPT_VER 未跑?)")
    if a.n > 0 and a.n < len(items):
        rnd = random.Random(a.seed)
        items = rnd.sample(items, a.n)
    print(f"judge {len(items)} 条(PROMPT_VER={PROMPT_VER}, "
          f"model={JUDGE_MODEL} 异模型)")
    import concurrent.futures as cf
    results = []
    with cf.ThreadPoolExecutor(5) as ex:
        futs = {ex.submit(judge_one, sid, sc, raw): sid
                for sid, sc, raw in items}
        for i, fut in enumerate(cf.as_completed(futs), 1):
            results.append(fut.result())
            if i % 50 == 0:
                print(f"  progress {i}/{len(items)}")

    ok_v = [r for r in results if "err" not in r]
    err_n = len(results) - len(ok_v)
    dims = ("emotion", "over", "persona")
    means = {k: sum(r[k] for r in ok_v) / max(len(ok_v), 1)
             for k in dims}
    low = [r for r in ok_v if min(r[k] for k in dims) <= 3]
    gate = err_n == 0 and len(ok_v) > 0 and \
        all(m >= 7.0 for m in means.values()) and len(low) == 0
    print(f"\n== 深思 judge(异模型 {JUDGE_MODEL}, n={len(ok_v)}, "
          f"err={err_n}) ==")
    for k in dims:
        print(f"  {k}: 均值 {means[k]:.2f}/10")
    print(f"  单样本任一维 ≤3: {len(low)} 条")
    dist = {}
    for r in ok_v:
        for k in dims:
            dist[k] = dist.get(k, {})
            dist[k][r[k]] = dist.get(k, {}).get(r[k], 0) + 1
    json.dump({"model": JUDGE_MODEL, "prompt_ver": PROMPT_VER,
               "n": len(ok_v), "err": err_n, "means": means,
               "low_count": len(low),
               "lows": [{"sid": r.get("sid", ""), "raw": r.get("raw", ""),
                         **{k: r[k] for k in dims}} for r in low[:20]],
               "gate": gate}, open(a.out, "w"), ensure_ascii=False, indent=1)
    print(f"门禁(三维均分≥7 ∧ 无单样本≤3 ∧ 零 judge 错): "
          f"{'PASS' if gate else 'FAIL'} | 明细: {a.out}")
    return 0 if gate else 1


if __name__ == "__main__":
    sys.exit(main())
