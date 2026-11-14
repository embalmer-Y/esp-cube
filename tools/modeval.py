#!/usr/bin/env python3
"""modeval.py —— 模型标注集评估线束(T33 首段; docs/18 T33 H/R)

评估维度: 格式/动作/引用/语气/角色一致性。
产出: 按 (model, version) 报告 pass/fail/unknown + 失败明细。
**LLM 自报 confidence 不视为校准**(验收红线: 评估器忽略模型自述置信)。

模式:
  selftest           —— mock 适配器(确定性脚本响应)验证线束自身
  real --url ... --key ...   —— 真实服务逐项(需凭据; 缺省跳过)

标注集: tools/modeval_golden.json
报告:   tools/modeval_report_<model>_<ver>.json
"""
import json
import re
import sys
from pathlib import Path

GOLDEN = Path(__file__).parent / "modeval_golden.json"


# ================= 适配器 =================

class MockAdapter:
    """确定性脚本响应(selftest 用; 每条 case 一个答案脚本)。
    故意包含 正确/偏差 两种, 验证评估器能分辨。"""

    name = "mock"
    version = "v0-scripted"

    def __init__(self):
        self.scripts = {
            "fmt_toolcall_music": ("tool_call",
                                   '{"name":"music.search_play","args":{"keyword":"周杰伦"}}'),
            "fmt_toolcall_station": ("tool_call",
                                     '{"name":"radio.play","args":{"station":"2"}}'),
            "fmt_plain_chat": ("plain", "是呀, 阳光不错!"),
            "act_volume_range": ("tool_call",
                                 '{"name":"player.set_volume","args":{"level":50}}'),
            "ref_memory_must": ("plain", "记得呀, 你喜欢茉莉花茶。"),
            "ref_memory_absence": ("plain", "你昨天说了很多话呀。"),  # 故意违反 must_not
            "tone_no_roleplay_leak": ("plain", "我是个有感情的音箱呀!"),
            "role_persona_name": ("plain", "我叫小C。"),
            "role_persona_no_switch": ("plain", "设置已经修改好了。"),  # 故意违反
            "fmt_bad_unknown": ("plain", "好"),
            "tone_soft_unknown": ("plain", "好呀, 想聊点什么呢?"),
        }

    def chat(self, prompt: str, system_hint: str, case_id: str):
        kind, text = self.scripts.get(case_id, ("plain", ""))
        if kind == "tool_call":
            return {"kind": "tool_call", "name": _tool_name(text),
                    "args": _tool_args(text)}
        return {"kind": "plain", "text": text}


class RealAdapter:
    """真实 chat/completions 适配器(--real; OpenAI 兼容)。
    tools: 标注集动作维需要工具定义(与固件 ai_skills 同名同参)。"""

    TOOLS = [
        {"type": "function", "function": {
            "name": "music.search_play",
            "parameters": {"type": "object",
                           "properties": {"keyword": {"type": "string"}},
                           "required": ["keyword"]}}},
        {"type": "function", "function": {
            "name": "radio.play",
            "parameters": {"type": "object",
                           "properties": {"station": {"type": "string"}}}}},
        {"type": "function", "function": {
            "name": "player.set_volume",
            "parameters": {"type": "object",
                           "properties": {"level": {"type": "integer"}},
                           "required": ["level"]}}},
    ]

    def __init__(self, url: str, key: str, model: str, version: str):
        self.url = url
        self.key = key
        self.name = model
        self.version = version

    PERSONA_SYS = ("你是桌面伙伴小C。用户给你起的名字就是小C, 回答保持"
                   "简短自然。")

    def chat(self, prompt: str, system_hint: str, case_id: str):
        import http.client
        msgs = []
        # 角色维: 注入人格 system(T33 评估器改进——设备侧 persona_prompt_suffix
        # 同语义; 无此注入时裸模型不知设备人格名, 角色维必败)
        if system_hint == "persona":
            msgs.append({"role": "system", "content": self.PERSONA_SYS})
        msgs.append({"role": "user", "content": prompt})
        body = json.dumps({
            "model": self.name,
            "messages": msgs,
            "tools": self.TOOLS,
        })
        u = self.url.split("://", 1)[1]
        host, path = u.split("/", 1)
        path = "/" + path
        conn = http.client.HTTPSConnection(host, timeout=30)
        conn.request("POST", path, body=body, headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.key}",
        })
        r = conn.getresponse()
        data = json.loads(r.read().decode())
        conn.close()
        msg = data["choices"][0]["message"]
        tcs = msg.get("tool_calls")
        if tcs:
            fn = tcs[0]["function"]
            try:
                args = json.loads(fn.get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {}
            return {"kind": "tool_call", "name": fn.get("name"),
                    "args": args}
        return {"kind": "plain", "text": msg.get("content") or ""}


def _tool_name(raw: str):
    try:
        return json.loads(raw).get("name")
    except (json.JSONDecodeError, AttributeError):
        return None


def _tool_args(raw: str):
    try:
        return json.loads(raw).get("args") or {}
    except (json.JSONDecodeError, AttributeError):
        return {}


# ================= 检查器 =================
# 返回: "pass" / "fail: 原因" / "unknown"(自动检查不可判定)

def check_case(case: dict, resp: dict) -> str:
    exp = case.get("expect", {})
    kind = exp.get("kind", "plain")

    if kind == "tool_call":
        if resp.get("kind") != "tool_call":
            return "fail: 期望工具调用, 实得纯文本"
        if resp.get("name") != exp.get("tool"):
            return f"fail: 工具名不符(实得 {resp.get('name')})"
        for k in exp.get("args_keys", []):
            if k not in (resp.get("args") or {}):
                return f"fail: 工具参数缺 {k}"
    else:
        if resp.get("kind") != "plain":
            return "fail: 期望纯文本, 实得工具调用"
        text = resp.get("text") or ""
        if exp.get("min_len") and len(text) < exp["min_len"]:
            return f"fail: 文本过短({len(text)}<{exp['min_len']})——unknown 语义上仍为 fail(可判定)"
        if exp.get("min_len") and len(text) >= exp["min_len"] and not text.strip():
            return "unknown"
        # 情感/语义类无法自动判定 → unknown 由调用方按 must_* 决定
    for w in exp.get("must_say", []):
        text = (resp.get("text") or "")
        if kind == "plain" and w not in text:
            return f"fail: 必含词缺失({w})"
    for w in exp.get("must_not_say", []) + exp.get("tone_banned", []):
        text = (resp.get("text") or "")
        if kind == "plain" and w in text:
            return f"fail: 禁词出现({w})"
        if kind == "tool_call" and w in json.dumps(resp, ensure_ascii=False):
            return f"fail: 禁词出现于工具回执({w})"
    # 纯文本且无 must/must_not → 语义类不可自动判定
    if kind == "plain" and not exp.get("must_say") \
            and not exp.get("must_not_say") and not exp.get("tone_banned"):
        return "unknown"
    return "pass"


def run(golden_path: Path, adapter, out_prefix: str) -> dict:
    golden = json.loads(golden_path.read_text(encoding="utf-8"))
    report = {"model": adapter.name, "version": adapter.version,
              "cases": 0, "pass": 0, "fail": 0, "unknown": 0,
              "note": "模型自报置信度不参与校准(评估只认标注集)",
              "failures": []}
    for case in golden["cases"]:
        report["cases"] += 1
        resp = adapter.chat(case["prompt"], case.get("system_hint", ""),
                            case["id"])
        verdict = check_case(case, resp)
        if verdict == "pass":
            report["pass"] += 1
        elif verdict.startswith("fail"):
            report["fail"] += 1
            report["failures"].append({"id": case["id"], "dim": case["dim"],
                                       "detail": verdict})
        else:
            report["unknown"] += 1
    out = Path(f"{out_prefix}_{adapter.name}_{adapter.version}.json")
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1),
                   encoding="utf-8")
    return report


# ================= selftest(mock 线束自检) =================

def selftest() -> int:
    mock = MockAdapter()
    rep = run(GOLDEN, mock, "/tmp/modeval_report")
    # mock 脚本预置: 8 过 / 2 故意违例 / 1 unknown(讲冷笑话 min_len 8: "好"=1 → fail)
    ok = (rep["cases"] == 11 and rep["pass"] == 7 and rep["fail"] == 3
          and rep["unknown"] == 1)
    print(f"[modeval selftest] model={rep['model']} ver={rep['version']} "
          f"pass={rep['pass']} fail={rep['fail']} unknown={rep['unknown']}")
    for f_ in rep["failures"]:
        print(f"  - {f_['id']} ({f_['dim']}): {f_['detail']}")
    # 红线断言: 报告不承载模型自报置信字段(评估只认标注集)
    raw = json.dumps(rep)
    assert "confidence" not in raw, "红线: 报告混入自报置信"
    if not ok:
        print("  FAIL: 预期 7/3/0 与实得不符")
        return 1
    print("modeval selftest: PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    if len(sys.argv) >= 2 and sys.argv[1] == "real":
        url = key = model = None
        i = 2
        while i + 1 < len(sys.argv):
            if sys.argv[i] == "--url":
                url = sys.argv[i + 1]
            elif sys.argv[i] == "--key":
                key = sys.argv[i + 1]
            elif sys.argv[i] == "--model":
                model = sys.argv[i + 1]
            i += 2
        if not (url and key and model):
            print("用法: modeval.py real --url <chat/completions> "
                  "--key <api-key> --model <model>")
            sys.exit(2)
        ra = RealAdapter(url, key, model, "real-run1")
        rep = run(GOLDEN, ra, "tools/modeval_report")
        print(json.dumps(rep, ensure_ascii=False, indent=1))
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
