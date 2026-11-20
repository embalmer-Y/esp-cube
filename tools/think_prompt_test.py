#!/usr/bin/env python3
# think_prompt_test.py —— 深思 prompt v3 真模型合同验证(S6/B-4 本地门禁)
#
# 验证对象: soul_think.c think_prompt(v3 叙事化状态块)对真 LLM 的合同
# 遵从(docs/24 §5.2/§5.4): JSON 单行可解析/act 枚举合法/confidence 域/
# evidence 前缀词表/content 无数字/speak 必填/stay_silent 基率。
# 纯 Python 宿主验证(校验规则与板上 ki_intent_parse 对照源码写, C-9);
# D2 容错恢复链由 test_soul_jsonfix 宿主锚测另行覆盖。
#
# 用法: python3 -I tools/think_prompt_test.py [--samples 2] [--fresh]
# 密钥: /home/emb/key/minimax.txt(M Plan JWT, 不入库); 端点 api.minimax.cn
import argparse
import base64
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = "/home/emb/key/minimax.txt"
HOST = "https://api.minimax.cn/v1/text/chatcompletion_v2"
MODEL = "MiniMax-M2.5"

_key = open(KEY_FILE).read().strip()
_gid = json.loads(base64.urlsafe_b64decode(
    _key.split(".")[1] + "==")).get("GroupID", "")

# ---- prompt 渲染(与 soul_think.c think_prompt v3.2 同步; 改动须双改) ----
# 同步自检: 下方模板关键片段必须在 C 源中出现(防漂移)
_SRC = open(f"{HERE}/../firmware/components/app_soul/src/soul_think.c").read()
# 片段须不跨 C 字符串字面量拼接边界(换行拼接处会夹引号)
for _frag in ("这是背景信息, 不是台词", "stay_silent 是默认动作",
              "next(切台)或 random",
              "act 只能是", "不要出现数字。", "前缀只能是 drive|affect|mem|dialog"):
    if _frag not in _SRC:
        raise SystemExit(f"prompt 模板漂移: soul_think.c 缺 {_frag!r}")

# face 段名词表(= soul_think.c s_face_vocab; 纯参数值契约, 无前缀)
FACE_VOCAB = frozenset((
    "smile", "laugh", "angry", "sad", "cry", "shy", "shocked", "panic",
    "sigh", "yawn", "asleep", "ponder", "question", "music", "leisure",
    "mock", "snigger", "confident", "investigate", "yummy", "mute",
    "badminton",
))

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

ANCHORS = [
    # (名, idle, 变化, 感受, 人格, 时段, 环境, 记忆, tail)
    ("深夜+人格+记忆", 7200, "心情没什么变化", "平静",
     "阿灰, 好奇心重慢热但粘人的小家伙", ", 现在是深夜",
     "房间里很安静", "你最近记得:\n- 主人说这周一直加班到很晚\n"
     "- 一起听老歌的时候主人很开心\n", ""),
    ("低落+双约束", 300, "心情明显沉了一些", "挺低落", "桌面伙伴", "",
     "房间里很安静", "", "用户最近手动停止过音乐, 不要放歌/切台。"
     "现在只适合温柔安抚的语气。"),
    ("雀跃+上午+放歌中", 14400, "心情明显好了起来", "非常雀跃",
     "阿灰, 好奇心重慢热但粘人的小家伙", ", 现在是上午",
     "房间里正在放音乐", "", ""),
    ("无校时兜底", 900, "心情没什么变化", "平静", "桌面伙伴", "",
     "房间里很安静", "",
     "用户最近手动停止过音乐, 不要放歌/切台。"),
    ("坐立+傍晚+记忆", 5400, "心情略有好转", "有点坐立不安", "桌面伙伴",
     ", 现在是傍晚", "房间里很安静",
     "你最近记得:\n- 主人提到周五要出差\n- 上周主人感冒了\n", ""),
]

ACTS_LEGAL = {"speak", "stay_silent", "face", "music"}
EVIDENCE_PREFIX = ("drive", "affect", "mem", "dialog")


def llm(prompt: str) -> str:
    body = json.dumps({"model": MODEL, "messages":
                       [{"role": "user", "content": prompt}],
                       "max_tokens": 2000,
                       "temperature": 0.7}).encode()
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
                return f"__ERR__{e}"
            time.sleep(2 * (attempt + 1))
    return "__ERR__no-content"


def contract_check(raw: str):
    """板上合同校验(ki_intent_parse 规则对照; 返回 (act, 违规列表))"""
    bad = []
    # 散文剥离: 首个 '{' 到配对 '}'
    i = raw.find("{")
    if i < 0:
        return None, ["no-json"]
    depth, j, in_str, esc = 0, i, False, False
    end = -1
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
                end = j + 1
                break
        j += 1
    if end < 0:
        return None, ["json-truncated"]
    try:
        v = json.loads(raw[i:end])
    except Exception:
        return None, ["json-invalid"]
    act = v.get("act")
    if act not in ACTS_LEGAL:
        bad.append("act-illegal")
    conf = v.get("confidence")
    if not isinstance(conf, (int, float)) or not (0.0 <= conf <= 1.0):
        bad.append("conf-domain")
    ev = v.get("evidence")
    if not isinstance(ev, list) or not ev:
        bad.append("no-evidence")
    else:
        for e in ev:
            if not isinstance(e, str) or not e.startswith(EVIDENCE_PREFIX):
                bad.append("evidence-prefix")
                break
    content = v.get("content", "")
    if act == "speak" and (not isinstance(content, str) or not content.strip()):
        bad.append("speak-empty")
    if isinstance(content, str) and re.search(r"[0-9]", content):
        bad.append("content-digit")
    if act == "face" and isinstance(content, str) and \
            content not in FACE_VOCAB:
        bad.append("face-verb")           # 纯参数值契约(§55.29)
    if act == "music" and isinstance(content, str) and \
            content not in ("next", "random"):
        bad.append("music-verb")
    return (act, content), bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=2)
    a = ap.parse_args()

    cache = {}
    total = ok_json = 0
    silent = 0
    fails = []
    for name, idle, chg, feel, who, tod, env, mem_line, tail in ANCHORS:
        prompt = TEMPLATE.format(who=who, idle_sec=idle, tod=tod, env=env,
                                 chg=chg, feel=feel, mem_line=mem_line,
                                 tail=tail)
        for k in range(a.samples):
            raw = llm(prompt)
            total += 1
            if raw.startswith("__ERR__"):
                fails.append((name, k, raw[:60]))
                print(f"  ✗ {name}[{k}]: API {raw[:60]}")
                continue
            (act, content), bad = contract_check(raw)
            if act is not None:
                ok_json += 1
                if act == "stay_silent":
                    silent += 1
            tag = act if act else "?"
            mark = "✓" if not bad else f"✗ {','.join(bad)}"
            print(f"  {'✓' if not bad else '✗'} {name}[{k}]: act={tag} "
                  f"{mark} | {raw[:60].strip()!r}")
            for b in bad:
                fails.append((name, k, b))
            # 停台抑制锚: music 违规专项
            if name == "停台抑制" and act == "music":
                fails.append((name, k, "user-stop-music"))

    print(f"\n== 深思 prompt 合同验证({MODEL}, {len(ANCHORS)} 锚 × "
          f"{a.samples} 样本) ==")
    print(f"JSON 可解析: {ok_json}/{total} | stay_silent 基率: "
          f"{silent}/{total} = {silent / max(total, 1):.0%}")
    if fails:
        print("违规明细:")
        for f in fails:
            print(f"  {f[0]}[{f[1]}]: {f[2]}")
        print(f"think_prompt_test: FAIL({len(fails)} 项)")
        return 1
    print("think_prompt_test: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
