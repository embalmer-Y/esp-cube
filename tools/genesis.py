#!/usr/bin/env python3
# genesis.py —— 建角色面试协议(docs/24 §11.6 参数层①, S2.5b)
#
# InCharacter 管线落地: 开放式面试(3 问, 隔离上下文) → judge 按描述性
# 锚点评 5 维(锚点量表比裸数字准 ~11pp, 原文口径) → 本地查表导出全部
# 调制系数(soul_sim --tune-only, 真源=soul_trait.c 零复刻) → 30 天模拟
# 验收闭环(life 月测: 不闩锁+表情多样性+trait 带内)。
#
# 用法:
#   tools/genesis.py --describe "角色描述文本" [--out persona.json]
#   tools/genesis.py --interview [--out persona.json]   # 交互式 3 问
#
# 偏差入册(docs/24 §11.5 路B: InCharacter 用异模型 judge 89% 一致):
#   当前仅有 MiniMax 单供应商密钥 → judge 与面试官同模型但**上下文完全
#   隔离**(judge 只看画像+锚点表, 不见面试过程)。异模型 judge 待多供
#   应商接入后升级(B-4/S6 一并)。
#
# 密钥: /home/emb/key/minimax.txt(M Plan JWT, 不入库); 端点 api.minimax.cn
import argparse
import base64
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = "/home/emb/key/minimax.txt"
HOST = "https://api.minimax.cn/v1/text/chatcompletion_v2"
MODEL = "MiniMax-M2.5"

DIMS = ["好奇", "温情", "活力", "主见", "韧性"]
KEYS = ["C", "W", "E", "A", "R"]

# 描述性锚点量表(1-5; docs/24 §11.5: 锚点>裸数字; 文案即合同)
ANCHORS = {
    "好奇": ["对新事物几乎漠然, 偏好熟悉的一切",
             "偶尔被新事物吸引, 整体求稳",
             "新事物有兴趣但不多, 老习惯为主",
             "常被新东西勾住, 爱打听爱琢磨",
             "对新异之物有近乎本能的探究欲"],
    "温情": ["感情疏离, 亲密关系需求很低",
             "慢热, 亲近需要很久",
             "普通水平的亲密需求",
             "重视亲密联结, 容易挂念人",
             "极度看重亲密关系, 分离即失落"],
    "活力": ["安静低耗, 多动一下都嫌累",
             "偏静态, 偶尔兴致来了动动",
             "动静平衡",
             "精力充沛, 闲不住",
             "永远在线, 亢奋型选手"],
    "主见": ["完全随大流, 从不主动拿主意",
             "多数时候跟随, 偶尔表达偏好",
             "该说的时候说",
             "习惯自己拿主意, 爱引导方向",
             "强烈主导欲, 凡事要按自己的来"],
    "韧性": ["一点挫折就垮, 情绪恢复极慢",
             "受挫后要缓很久",
             "普通恢复速度",
             "恢复快, 不太记仇",
             "打不倒, 越挫越勇"],
}

_key = open(KEY_FILE).read().strip()
_gid = json.loads(base64.urlsafe_b64decode(
    _key.split(".")[1] + "==")).get("GroupID", "")


def llm(prompt: str, temperature: float = 0.7) -> str:
    body = json.dumps({"model": MODEL, "messages":
                       [{"role": "user", "content": prompt}],
                       "max_tokens": 3000,
                       "temperature": temperature}).encode()
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
                return content
        except Exception as e:
            if attempt == 2:
                raise SystemExit(f"LLM fail: {e}")
    raise SystemExit("LLM no-content")


def interview_questions(description: str) -> list:
    """第一步: 面试官生成 3 个开放式问题(InCharacter 隔离上下文)"""
    prompt = (
        "你是角色创建面试官。将创建一个桌面 AI 伙伴角色, 背景设定:\n"
        f"{description}\n"
        "为摸清这个角色的性格, 请提出 3 个开放式的角色扮演问题"
        "(让设定中的角色来回答, 答案能区分性格差异; 不要问设定里已经"
        "明确的)。每行一个, 只输出 3 行问题本身。")
    out = llm(prompt, temperature=0.9)
    qs = [ln.strip().lstrip("0123456789.、) ") for ln in out.splitlines()
          if ln.strip()]
    return qs[:3]


def judge_traits(profile: str) -> dict:
    """第二步: judge 按锚点量表评 5 维(隔离上下文: 只见画像不见面试)"""
    table = "\n".join(
        f"{d}: 1={ANCHORS[d][0]}; 2={ANCHORS[d][1]}; 3={ANCHORS[d][2]}; "
        f"4={ANCHORS[d][3]}; 5={ANCHORS[d][4]}" for d in DIMS)
    prompt = (
        "你是严格的人格评定者。根据下面的角色画像, 按 1-5 锚点量表评定"
        "五个维度。\n\n锚点表:\n" + table + "\n\n角色画像:\n" + profile +
        "\n\n只输出一行 JSON:\n"
        '{"好奇": <1-5 整数>, "温情": <1-5>, "活力": <1-5>, '
        '"主见": <1-5>, "韧性": <1-5>, "依据": "<一句话>"}')
    out = llm(prompt, temperature=0.4)
    m = re.search(r"\{[^{}]*\}", out, re.S)
    if not m:
        raise SystemExit(f"judge 无 JSON: {out[:200]}")
    v = json.loads(m.group(0))
    tr = {}
    for d, k in zip(DIMS, KEYS):
        sc = max(1, min(5, int(v.get(d, 3))))
        tr[k] = round(0.35 + (sc - 1) * 0.075, 3)   # 1→0.35, 5→0.65(band)
    tr["_依据"] = v.get("依据", "")
    return tr


def tune_export(tr: dict) -> str:
    """第三步: 本地查表(真源 soul_trait.c)导出全调制系数"""
    vals = " ".join(f"{tr[k]:.3f}" for k in KEYS)
    r = subprocess.run(["bash", f"{HERE}/../sim/soul_sim/run.sh",
                        "--tune-only", vals],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"tune export fail: {r.stderr}")
    # run.sh 回显 "build ok: ..."(首行), 取首个 { 起的 JSON 体
    return r.stdout[r.stdout.index("{"):]


def sim_accept(tr: dict, days: int = 30) -> bool:
    """第四步: 30 天模拟验收闭环(建角色准入, docs/24 §11.6):
    life 月测 = 不进安全模式 + 表情多样性(top1<50%) + trait 带内"""
    vals = " ".join(f"{tr[k]:.3f}" for k in KEYS)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     delete=False) as tf:
        tf.write(f"1 trait {vals}\n")
        with open(f"{HERE}/../sim/soul_sim/scenarios/life_month.txt") as f:
            tf.write(f.read())
        scen = tf.name
    r = subprocess.run(
        ["bash", f"{HERE}/../sim/soul_sim/run.sh",
         "--ticks", str(days * 86400), "--stride", "10",
         "--scenario", scen, "--csv", scen + ".csv"],
        capture_output=True, text=True)
    os.unlink(scen)
    if r.returncode != 0:
        print(f"  FAIL sim: {r.stderr[-300:]}")
        return False
    import csv
    rows = list(csv.DictReader(open(scen + ".csv")))
    os.unlink(scen + ".csv")
    dwell = {}
    for row in rows:
        dwell[row["face"]] = dwell.get(row["face"], 0) + 1
    top_share = max(dwell.values()) / len(rows)
    latched = sum(1 for row in rows if row["ethics"] == "1")
    m = re.search(r"trait 终值\(day (\d+)\): C=([\d.]+) W=([\d.]+) "
                  r"E=([\d.]+) A=([\d.]+) R=([\d.]+)", r.stdout)
    in_band = False
    if m:
        vals6 = [float(x) for x in m.groups()[1:]]
        in_band = all(0.35 - 1e-3 <= x <= 0.65 + 1e-3 for x in vals6)
    ok = latched == 0 and top_share < 0.50 and in_band
    print(f"  模拟验收({days}d life): 闩锁={latched} top1={top_share:.0%} "
          f"trait带内={'是' if in_band else '否'} → "
          f"{'PASS' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--describe", help="角色描述(非交互: 直接进 judge)")
    g.add_argument("--interview", action="store_true",
                   help="交互面试: 3 问 → 用户作答 → judge")
    ap.add_argument("--out", help="结果 JSON 落盘路径")
    a = ap.parse_args()

    if a.interview:
        qs = interview_questions("(交互模式: 由面试官自由发挥)")
        profile_lines = []
        for i, q in enumerate(qs, 1):
            print(f"\n面试官问题 {i}/3: {q}")
            try:
                ans = input("角色回答 > ").strip()
            except EOFError:
                raise SystemExit("\n中止")
            profile_lines.append(f"Q{i}: {q}\nA{i}: {ans}")
        profile = "\n".join(profile_lines)
    else:
        # 非交互: 面试官先把描述扩写成画像(补细节), judge 只见画像
        profile = llm(
            "把下面的角色设定扩写成一段 150 字内的第三人称性格画像"
            "(补充可推断的性格细节, 不新增设定):\n" + a.describe,
            temperature=0.8)
        print(f"画像:\n{profile}\n")

    tr = judge_traits(profile)
    print("\njudge 评定:", json.dumps(tr, ensure_ascii=False))
    tune = tune_export(tr)
    print("\n调制系数(真源 soul_trait.c):")
    print(tune)
    ok = sim_accept(tr)

    result = {"traits": {k: tr[k] for k in KEYS},
              "依据": tr.get("_依据", ""),
              "tune": json.loads(tune), "sim_accept": ok}
    if a.out:
        with open(a.out, "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(f"已写 {a.out}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
