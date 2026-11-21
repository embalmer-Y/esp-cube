// minimax_provider.js —— promptfoo custom provider(MiniMax chatcompletion_v2)
// 0.118 file:// 形态=类导出(callApi 方法); 密钥经 MINIMAX_KEY 注入
// (零凭据入库); 端点与 think_prompt_bench 同源。
const https = require("https");

const MODEL = "MiniMax-M3.1-flash";
const HOST = "api.minimax.cn";
const PATH = "/v1/text/chatcompletion_v2";

function groupId(jwt) {
  try {
    const p = JSON.parse(
      Buffer.from(jwt.split(".")[1].replace(/-/g, "+").replace(/_/g, "/"),
        "base64").toString());
    return p.GroupID || "";
  } catch (e) {
    return "";
  }
}

class MiniMaxProvider {
  constructor(options) {
    this.providerId = (options && options.id) || "minimax-m31flash";
    this.config = (options && options.config) || {};
  }

  id() {
    return this.providerId;
  }

  callApi(prompt) {
    return new Promise((resolve, reject) => {
      const key = process.env.MINIMAX_KEY;
      if (!key) return reject(new Error("MINIMAX_KEY env 缺席"));
      const body = JSON.stringify({
        model: MODEL,
        messages: [{ role: "user", content: prompt }],
        max_tokens: 2000,
        temperature: 0.7,
      });
      const req = https.request(
        {
          hostname: HOST,
          path: PATH + "?GroupId=" + groupId(key),
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            Authorization: "Bearer " + key,
            "Content-Length": Buffer.byteLength(body),
          },
        },
        (res) => {
          let data = "";
          res.on("data", (c) => (data += c));
          res.on("end", () => {
            try {
              const d = JSON.parse(data);
              const sc = (d.base_resp || {}).status_code;
              const content =
                ((d.choices || [{}])[0].message || {}).content || "";
              if (sc === 0 && content)
                resolve({ output: content, tokenUsage: d.usage || {} });
              else
                resolve({ error: `sc${sc}:${(d.base_resp || {}).status_msg}` });
            } catch (e) {
              resolve({ error: String(e.message || e) });
            }
          });
        }
      );
      req.on("error", (e) => resolve({ error: String(e.message || e) }));
      req.setTimeout(120000, () => {
        req.destroy();
        resolve({ error: "timeout" });
      });
      req.write(body);
      req.end();
    });
  }
}

module.exports = MiniMaxProvider;
