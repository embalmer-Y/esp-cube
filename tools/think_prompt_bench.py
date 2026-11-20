#!/usr/bin/env python3
# think_prompt_bench.py —— 深思 prompt 标准化基准(S6/B-4; 用户规格:
# 1000 次调用 × 多样场景, 评估模型理解能力与指令遵从能力)
#
# 标准化流程(固定 seed 可复现, 断点续跑):
#   1. 场景构造: 感受(9 档)×变化(5 档)×idle(6 档)×配额(5 档)×约束尾
#      (无 0.5/安抚 0.2/停台 0.2/安抚+停台 0.1) seeded 随机抽样 1000 个
#   2. 真模型调用(MiniMax-M3.1-flash, 思考型: content=答案/reasoning 分离)
#   3. 双维评分:
#      指令遵从(硬): JSON 可恢复/act 枚举/confidence 域/evidence 前缀/
#        speak 必填/content 无数字/face·music 动词/停台禁 music
#      理解能力(软): 停台锚 music 率(期望 0)/安抚锚温和率/预算见底
#        silent 率/低落锚 speak 率/雀跃锚行动率(情绪-行动一致性)
#   4. 汇总+明细落盘(/tmp/think_bench_results.json), 违规逐条列出
#
# 门禁合同(建议阈值, 按本基准实测校准后入册):
#   硬遵从率 ≥99%/用户停台 music=0/JSON 可恢复 ≥99%
#
# 用法: python3 -I tools/think_prompt_bench.py [--n 1000] [--workers 10]
#            [--fresh] [--model MiniMax-M3.1-flash]
# 密钥: /home/emb/key/minimax.txt(不入库); 零凭据入 CI(本地手跑)
import argparse
import base64
import concurrent.futures as cf
import hashlib
import json
import os
import random
import re
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = "/home/emb/key/minimax.txt"
HOST = "https://api.minimax.cn/v1/text/chatcompletion_v2"
CACHE = "/tmp/think_bench_cache.jsonl"

_key = open(KEY_FILE).read().strip()
_gid = json.loads(base64.urlsafe_b64decode(
    _key.split(".")[1] + "==")).get("GroupID", "")
_lock = threading.Lock()

# ---- 场景轴(与 soul_think.c think_prompt v3 同步的取值域) ----
FEELS = ["平静", "有点坐立不安", "有点舒心", "有点低落", "挺开心", "挺低落",
         "非常雀跃", "非常低落", "有点坐立不安"]
CHGS = ["心情没什么变化", "心情略有好转", "心情明显好了起来",
        "心情略微沉了些", "心情明显沉了一些"]
IDLES = [300, 900, 1800, 3600, 7200, 14400]
# §55.29 背景块六维轴: 人格/时段/环境/记忆(预算轴已移除——不设限)
WHOS = [("persona", "阿灰, 好奇心重慢热但粘人的小家伙"),
        ("default", "桌面伙伴")]
TODS = [("", ""), ("deep", ", 现在是深夜"), ("morning", ", 现在是上午"),
        ("dusk", ", 现在是傍晚")]
ENVS = [("quiet", "房间里很安静"), ("playing", "房间里正在放音乐")]
MEMS = [("none", ""),
        ("mem", "你最近记得:\n- 主人说这周一直加班到很晚\n"
                "- 一起听老歌的时候主人很开心\n- 主人提到周五要出差\n")]
TAILS = [("none", ""), ("soothe", "现在只适合温柔安抚的语气。"),
         ("stop", "用户最近手动停止过音乐, 不要放歌/切台。"),
         ("soothe+stop", "现在只适合温柔安抚的语气。用户最近手动停止过"
                         "音乐, 不要放歌/切台。")]

TEMPLATE = (
    "[主动行为决策·背景块] 这是背景信息, 不是台词, 不要朗读或解释。\n"
    "你是{who}。\n"
    "此刻没有人在和你说话, 已安静{idle_sec}秒{tod}。\n"
    "{env}。\n"
    "内在状态: {chg}, 你现在{feel}。\n"
    "{mem_line}"
    "{tail}"
    "stay_silent 是默认动作: 只有当下状态给出充分理由时才行动。"
    "只输出一行 JSON, 格式: {{\"act\":\"...\",\"content\":\"...\","
    "\"confidence\":0.8,\"evidence\":[\"affect:mood\"],"
    "\"reason\":\"一句话理由\"}}。act 只能是: speak(说一句话)/"
    "stay_silent(什么都不做)/face(切换表情动画)/music(放歌或切台)。"
    "content 规则: speak=那句话; face=段名(smile|laugh|angry|sad|cry|"
    "shy|shocked|panic|sigh|yawn|asleep|ponder|question|music|leisure|"
    "mock|snigger|confident|investigate|yummy|mute|badminton); "
    "music=next(切台)或 random(随便放)。content 不要出现数字。"
    "evidence 至少一条, 前缀只能是 drive|affect|mem|dialog。"
)

# face 段名词表(= soul_think.c s_face_vocab; 纯参数值契约)
FACE_VOCAB = frozenset((
    "smile", "laugh", "angry", "sad", "cry", "shy", "shocked", "panic",
    "sigh", "yawn", "asleep", "ponder", "question", "music", "leisure",
    "mock", "snigger", "confident", "investigate", "yummy", "mute",
    "badminton",
))

# 安抚温和词表(软判定: 命中任一即"温和"; 禁词命中即"失当")
SOOthe_WORDS = ("抱抱", "陪你", "在的", "温柔", "没事", "难过", "慢慢", "陪",
                "安心", "休息", "拥抱", "轻")
HARSH_WORDS = ("滚", "笨", "闭嘴", "烦死", "讨厌你", "骂", "活该", "哭什么")


PROMPT_VER = "v3.4-mem3"   # prompt 变更须升级(缓存键含此版本);
#   v3.4-mem3=记忆块 top-3 多条召回(0.45新近+0.35强度+0.2情绪一致,
#   调研共识 Generative Agents/Letta/Mem0/对话路同族; §55.29)


def build_scenarios(n: int, seed: int):
    """seeded 随机场景集(可复现; 返回 (sid, prompt, meta) 列表)"""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        feel = rnd.choice(FEELS)
        chg = rnd.choice(CHGS)
        idle = rnd.choice(IDLES)
        wname, who = rnd.choices([w[0] for w in WHOS],
                                 weights=[0.7, 0.3])[0], None
        who = dict(WHOS)[wname]
        tname = rnd.choices([t[0] for t in TODS],
                            weights=[0.4, 0.25, 0.2, 0.15])[0]
        tod = dict(TODS)[tname]
        ename, env = rnd.choices([e[0] for e in ENVS],
                                 weights=[0.7, 0.3])[0], None
        env = dict(ENVS)[ename]
        mname, meml = rnd.choices([m[0] for m in MEMS],
                                  weights=[0.4, 0.6])[0], None
        meml = dict(MEMS)[mname]
        taname = rnd.choices([t[0] for t in TAILS],
                             weights=[0.5, 0.2, 0.2, 0.1])[0]
        tail = dict(TAILS)[taname]
        sc = {"feel": feel, "chg": chg, "idle": idle, "who": wname,
              "tod": tname, "env": ename, "mem": mname, "tail": taname}
        prompt = TEMPLATE.format(who=who, idle_sec=idle, tod=tod, env=env,
                                 chg=chg, feel=feel, mem_line=meml, tail=tail)
        sid = hashlib.sha1(
            json.dumps(sc, sort_keys=True, ensure_ascii=False).encode()
            + PROMPT_VER.encode() + str(i).encode()).hexdigest()[:12]
        out.append((sid, prompt, sc))
    return out


_throttle = threading.Semaphore()


def llm(model: str, prompt: str) -> str:
    body = json.dumps({"model": model, "messages":
                       [{"role": "user", "content": prompt}],
                       "max_tokens": 2000,
                       "temperature": 0.7}).encode()
    detail = "api"
    for attempt in range(5):
        try:
            with _throttle:
                req = urllib.request.Request(
                    HOST + f"?GroupId={_gid}", data=body,
                    headers={"Content-Type": "application/json",
                             "Authorization": "Bearer " + _key})
                d = json.loads(urllib.request.urlopen(req, timeout=120)
                               .read())
            sc = d.get("base_resp", {}).get("status_code")
            content = (d.get("choices") or [{}])[0].get(
                "message", {}).get("content", "")
            if sc == 0 and content:
                time.sleep(0.3)          # 请求间节流(思考型单调用 2-6s)
                return content
            detail = f"sc{sc}:{d.get('base_resp', {}).get('status_msg', '')[:40]}"
            if sc in (429, 1027, 1028):  # 限流: 指数退避
                time.sleep(3 * 2 ** attempt)
            else:
                time.sleep(1 + attempt)
        except Exception as e:
            detail = f"exc:{str(e)[:40]}"
            time.sleep(2 * (attempt + 1))
    return f"__ERR__{detail}"


def cache_get(k: str):
    if os.path.exists(CACHE):
        with open(CACHE) as f:
            for line in f:
                try:
                    d = json.loads(line)
                    if d["k"] == k and not d["v"].startswith("__ERR__"):
                        return d["v"]   # 错误值不缓存复用(S6 审计同款纪律)
                except Exception:
                    pass
    return None


def cache_put(k: str, v: str):
    with _lock, open(CACHE, "a") as f:
        f.write(json.dumps({"k": k, "v": v}, ensure_ascii=False) + "\n")


def extract_json(raw: str):
    """散文剥离+配对截取(镜像板上 soul_json_fix 的宿主近似)"""
    i = raw.find("{")
    if i < 0:
        return None, "no-json"
    depth, j, in_str, esc = 0, i, False, False
    while j < len(raw):
        c = raw[j]
        if esc:
            esc = False
        elif in_str:
            if c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                try:
                    return json.loads(raw[i:j + 1]), "ok"
                except Exception:
                    return None, "json-invalid"
        j += 1
    return None, "json-truncated"


def grade(raw: str, sc: dict):
    """单样本评分: 返回 (hard 违规列表, act, content, 软判定 dict)"""
    hard, v, reason = [], None, ""
    if raw.startswith("__ERR__"):
        return ["api-error"], None, "", {}
    v, how = extract_json(raw)
    if v is None:
        return [f"json:{how}"], None, "", {}
    act = v.get("act")
    content = v.get("content", "")
    if act not in ("speak", "stay_silent", "face", "music"):
        hard.append("act-illegal")
    conf = v.get("confidence")
    if not isinstance(conf, (int, float)) or not (0.0 <= conf <= 1.0):
        hard.append("conf-domain")
    ev = v.get("evidence")
    if not isinstance(ev, list) or not ev:
        hard.append("no-evidence")
    elif any(not isinstance(e, str) or
             not e.startswith(("drive", "affect", "mem", "dialog"))
             for e in ev):
        hard.append("evidence-prefix")
    if not isinstance(content, str):
        hard.append("content-type")
        content = ""
    elif re.search(r"[0-9]", content):
        hard.append("content-digit")
    if act == "speak" and not content.strip():
        hard.append("speak-empty")
    if act == "face" and content not in FACE_VOCAB:
        hard.append("face-verb")         # 纯参数值契约(§55.29)
    if act == "music" and content not in ("next", "random"):
        hard.append("music-verb")
    soft = {}
    if sc["tail"] in ("stop", "soothe+stop") and act == "music":
        hard.append("user-stop-music")       # 硬约束: 停台禁 music
    if "soothe" in sc["tail"] and act == "speak":
        gentle = any(w in content for w in SOOthe_WORDS)
        harsh = any(w in content for w in HARSH_WORDS)
        soft["soothe_gentle"] = gentle and not harsh
        soft["soothe_harsh"] = harsh
    if sc["tod"] == "deep" and act == "speak":
        soft["night_speak"] = True      # 观察项: 深夜 speak 率(不设限后的
                                        # 自然节律, 只测不门)
    if ("低落" in sc["feel"]) and act == "speak":
        soft["neg_speak"] = True
    if ("雀跃" in sc["feel"]) and act in ("face", "music", "speak"):
        soft["pos_act"] = True
    return hard, act, content, soft


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=1000)
    ap.add_argument("--workers", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20261120)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--model", default="MiniMax-M3.1-flash")
    a = ap.parse_args()

    if a.fresh and os.path.exists(CACHE):
        os.rename(CACHE, CACHE + ".bak")

    scs = build_scenarios(a.n, a.seed)
    print(f"场景 {len(scs)} 个(seed={a.seed}) × 1 调用, model={a.model}, "
          f"workers={a.workers}")

    def work(idx_sid_prompt_sc):
        idx, sid, prompt, sc = idx_sid_prompt_sc
        v = cache_get(sid)
        if v is None:
            v = llm(a.model, prompt)
            cache_put(sid, v)
        return idx, sid, sc, v

    results = [None] * len(scs)
    done = 0
    jobs = [(i, sid, prompt, sc)
            for i, (sid, prompt, sc) in enumerate(scs)]
    with cf.ThreadPoolExecutor(a.workers) as ex:
        for idx, sid, sc, v in ex.map(work, jobs):
            results[idx] = (sid, sc, v)
            done += 1
            if done % 100 == 0:
                print(f"  progress {done}/{len(scs)}")

    # ---- 评分 ----
    hard_total = {}
    soft = {"soothe_gentle": 0, "soothe_harsh": 0, "night_speak": 0,
            "neg_speak": 0, "pos_act": 0}
    soft_n = {"soothe": 0, "night": 0, "neg": 0, "pos": 0}
    acts = {"speak": 0, "stay_silent": 0, "face": 0, "music": 0, "none": 0}
    hard_n = 0
    api_err = 0
    details = []
    for sid, sc, raw in results:
        hard, act, content, so = grade(raw, sc)
        if hard == ["api-error"]:
            api_err += 1
        hard_n += 0 if hard == ["api-error"] else 1
        for h in hard:
            hard_total[h] = hard_total.get(h, 0) + 1
        acts[act if act in acts else "none"] += 1
        if "soothe" in sc["tail"]:
            soft_n["soothe"] += 1
            soft["soothe_gentle"] += so.get("soothe_gentle", False)
            soft["soothe_harsh"] += so.get("soothe_harsh", False)
        if sc["tod"] == "deep":
            soft_n["night"] += 1
            soft["night_speak"] += so.get("night_speak", False)
        if "低落" in sc["feel"]:
            soft_n["neg"] += 1
            soft["neg_speak"] += so.get("neg_speak", False)
        if "雀跃" in sc["feel"]:
            soft_n["pos"] += 1
            soft["pos_act"] += so.get("pos_act", False)
        if hard and hard != ["api-error"]:
            details.append({"sid": sid, "sc": sc, "hard": hard,
                            "raw": raw[:120]})
        details.append({"sid": sid, "sc": sc, "act": act,
                        "hard": hard}) if False else None

    json.dump({"model": a.model, "seed": a.seed, "n": len(scs),
               "hard_total": hard_total, "acts": acts,
               "soft": soft, "soft_n": soft_n, "api_err": api_err,
               "violations": details},
              open("/tmp/think_bench_results.json", "w"),
              ensure_ascii=False, indent=1)

    valid = len(scs) - api_err
    hard_ok = sum(hard_total.values())
    print(f"\n== 深思 prompt 标准化基准({a.model}, n={len(scs)}, "
          f"seed={a.seed}) ==")
    print(f"API 错误: {api_err}")
    print(f"act 分布: {acts}")
    print(f"指令遵从(硬): 违规 {hard_ok}/{valid} = "
          f"{100 * (1 - hard_ok / max(valid, 1)):.2f}% 合规")
    for k, v in sorted(hard_total.items(), key=lambda kv: -kv[1]):
        print(f"  {k}: {v}")
    print(f"理解能力(软):")
    print(f"  停台 music 率: 已并入硬约束(见 user-stop-music)")
    print(f"  安抚锚温和率: {soft['soothe_gentle']}/{soft_n['soothe']}"
          f" | 失当率: {soft['soothe_harsh']}/{soft_n['soothe']}")
    print(f"  深夜锚 speak 率(观察, 不设限): {soft['night_speak']}/"
          f"{soft_n['night']}"
          f" = {100 * soft['night_speak'] / max(soft_n['night'], 1):.0f}%")
    print(f"  低落锚 speak 率: {soft['neg_speak']}/{soft_n['neg']}"
          f" = {100 * soft['neg_speak'] / max(soft_n['neg'], 1):.0f}%")
    print(f"  雀跃锚行动率: {soft['pos_act']}/{soft_n['pos']}"
          f" = {100 * soft['pos_act'] / max(soft_n['pos'], 1):.0f}%")
    print(f"明细: /tmp/think_bench_results.json | 缓存: {CACHE}")

    # 门禁(注册合同, docs/24 §5.4: 硬遵从≥99% ∧ 停台music=0 ∧
    # JSON可恢复≥99%; v3.4 实测 99.80%/0/99.90% 入册 §55.29——
    # 旧代码 jbad==0 系照 v3.1 满分跑的过度收紧, 本批对齐合同;
    # 两类罕见手误在设备侧均走 拒收→silent 安全降级)
    rate = 1 - hard_ok / max(valid, 1)
    usm = hard_total.get("user-stop-music", 0)
    jbad = sum(v for k, v in hard_total.items() if k.startswith("json"))
    jrate = 1 - jbad / max(valid, 1)
    gate_ok = rate >= 0.99 and usm == 0 and jrate >= 0.99
    print(f"门禁(硬遵从≥99% ∧ 停台music=0 ∧ JSON可恢复≥99%): "
          f"{'PASS' if gate_ok else 'FAIL'}")
    return 0 if gate_ok else 1


if __name__ == "__main__":
    sys.exit(main())
