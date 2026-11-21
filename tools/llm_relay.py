#!/usr/bin/env python3
"""llm_relay.py —— 设备 LLM 局域网明文中继(B-6a 出站 TLS 档期方案)

背景: 板上出站 TLS 客户端在 IDF 6.1/mbedtls v4 栈确定性握手失败
(-0x0087, §55.30 根因挂账); 入站 TLS/明文 HTTP 全正常。本中继让
设备经 LAN 明文 HTTP 调 LLM, 密钥只留在主机:

  设备 --HTTP(LAN)--> 本中继 --HTTPS--> api.minimax.cn

用法: python3 -I tools/llm_relay.py [port=8787]
密钥: /home/emb/key/minimax.txt(不入库; 与 bench 同源)
安全: 仅绑 LAN; 无鉴权(个人局域网威胁模型, 台账注记); 上游错误
原样透传。SSE 逐行转发(sp_llm stream:true 依赖)。
"""
import base64
import json
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY_FILE = "/home/emb/key/minimax.txt"
UPSTREAM_HOST = "https://api.minimax.cn/v1/text/chatcompletion_v2"

_key = open(KEY_FILE).read().strip()
_gid = json.loads(base64.urlsafe_b64decode(
    _key.split(".")[1] + "==")).get("GroupID", "")


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[relay] %s %s\n" %
                         (time.strftime("%H:%M:%S"), fmt % args))

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/health"):
            self._json(200, {"ok": True, "gid": _gid[:4] + "***"})
        else:
            self._json(404, {"error": "path"})

    def do_POST(self):
        if not self.path.endswith("/chat/completions"):
            self._json(404, {"error": "path"})
            return
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n else b""
        req = urllib.request.Request(
            UPSTREAM_HOST + f"?GroupId={_gid}", data=body,
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + _key})
        try:
            up = urllib.request.urlopen(req, timeout=120)
        except urllib.error.HTTPError as e:
            self._json(e.code, {"upstream": e.read().decode("utf-8", "replace")[:400]})
            return
        except Exception as e:
            self._json(502, {"upstream": str(e)[:200]})
            return
        # SSE/明文透传: 逐块读上游, 立刻写给设备(不缓冲)
        self.send_response(up.status)
        ctype = up.headers.get("Content-Type", "application/json")
        self.send_header("Content-Type", ctype)
        if ctype.startswith("text/event-stream"):
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                while True:
                    chunk = up.read(1)
                    if not chunk:
                        break
                    self.wfile.write(b"%x\r\n" % 1 + chunk + b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            data = up.read()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    srv = ThreadingHTTPServer(("0.0.0.0", port), Relay)
    print(f"llm_relay on :{port} -> {UPSTREAM_HOST} (gid={_gid[:4]}***)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
