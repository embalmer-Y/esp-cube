#!/usr/bin/env python3
"""clone_voice.py —— M5 品牌音色复刻脚手架(双引擎同源复刻)

同一段参考音频, 在两个引擎各克隆一次, 让快线(omni realtime)与
慢线(cosyvoice)说同一把嗓子:
  - cosyvoice  : 慢线 TTS(级联三段链), target_model=cosyvoice-v3-flash
                 [Kaleido V-33 板上实证: 免费分钟级, 10-20s 样本]
  - omni       : 快线 realtime 出声, target_model=qwen-audio-3.1-realtime
                 -flash(命名随百炼版本, 可 --target-model 覆写)
用法:
  KEY=sk-xxx python3 tools/clone_voice.py create --engine both \
      --file ref.wav --prefix brand        # ref: 10-30s 清晰单人
  python3 tools/clone_voice.py list [--engine cosyvoice|omni]
  python3 tools/clone_voice.py query --vid <voice_id>
  python3 tools/clone_voice.py delete --vid <voice_id>
产物 voice_id 回填: sdkconfig 的 AI_TTS_VOICE(慢线)/AI_RT_VOICE(快线)。
"""
import argparse, base64, io, json, os, sys, time, wave
import urllib.request

LEGACY = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/customization"
ENGINE_TARGET = {
    "cosyvoice": "cosyvoice-v3-flash",
    "omni": "qwen-audio-3.1-realtime-flash",   # 快线; 版本命名可覆写
}


def key():
    k = os.environ.get("KEY") or (
        os.path.exists("/home/emb/key/aliyun.txt") and
        open("/home/emb/key/aliyun.txt").read().strip())
    if not k:
        sys.exit("缺少 API key: 设 KEY 环境变量(勿入库)")
    return k


def call(payload):
    req = urllib.request.Request(
        LEGACY, data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key()}",
                 "Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read()), (time.perf_counter()-t0)*1e3
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}'), (time.perf_counter()-t0)*1e3


def wav_b64(path):
    pcm = open(path, "rb").read()
    if path.endswith(".wav"):
        return base64.b64encode(pcm).decode()
    buf = io.BytesIO()                       # 裸 PCM16 16k → wav 容器
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
        w.writeframes(pcm)
    return base64.b64encode(buf.getvalue()).decode()


def create(args):
    audio = f"data:audio/wav;base64,{wav_b64(args.file)}"
    engines = ["cosyvoice", "omni"] if args.engine == "both" else [args.engine]
    for e in engines:
        tm = args.target_model or ENGINE_TARGET[e]
        st, body, ms = call({"model": "voice-enrollment", "input": {
            "action": "create_voice", "target_model": tm,
            "prefix": args.prefix, "audio": audio,
            "language_hints": ["zh"]}})
        vid = body.get("output", {}).get("voice_id", "")
        print(f"[{e}] create http={st} {ms:.0f}ms voice_id={vid} "
              f"{json.dumps(body, ensure_ascii=False)[:200]}")
        if not vid:
            continue
        for i in range(30):                  # 轮询到 OK/UNDEPLOYED
            time.sleep(4)
            st, body, ms = call({"model": "voice-enrollment", "input": {
                "action": "query_voice", "voice_id": vid}})
            status = body.get("output", {}).get("status")
            print(f"[{e}] query#{i} status={status}")
            if status in ("OK", "UNDEPLOYED"):
                break
        print(f"[{e}] FINAL {vid} status={status}")
        if e == "cosyvoice":
            print(f"  → 慢线: sdkconfig 设 CONFIG_ESP_CUBE_AI_TTS_VOICE=\"{vid}\"")
        else:
            print(f"  → 快线: sdkconfig 设 CONFIG_ESP_CUBE_AI_RT_VOICE=\"{vid}\"")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("--engine", choices=["cosyvoice", "omni", "both"],
                   default="both")
    c.add_argument("--file", required=True, help="参考音频 10-30s 单人")
    c.add_argument("--prefix", required=True, help="音色前缀(≤10 字母数字)")
    c.add_argument("--target-model", default=None, help="覆写 target_model")
    sub.add_parser("list").add_argument("--engine", default=None)
    q = sub.add_parser("query"); q.add_argument("--vid", required=True)
    d = sub.add_parser("delete"); d.add_argument("--vid", required=True)
    args = ap.parse_args()

    if args.cmd == "create":
        create(args)
    elif args.cmd == "list":
        st, body, ms = call({"model": "voice-enrollment", "input": {
            "action": "list_voice", "page_size": 20}})
        for v in body.get("output", {}).get("voice_list", []):
            print(v)
    elif args.cmd == "query":
        st, body, ms = call({"model": "voice-enrollment", "input": {
            "action": "query_voice", "voice_id": args.vid}})
        print(json.dumps(body, ensure_ascii=False, indent=2))
    elif args.cmd == "delete":
        st, body, ms = call({"model": "voice-enrollment", "input": {
            "action": "delete_voice", "voice_id": args.vid}})
        print(f"delete http={st} {json.dumps(body, ensure_ascii=False)[:200]}")


if __name__ == "__main__":
    main()
