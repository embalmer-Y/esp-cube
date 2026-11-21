# promptfoo 深思 prompt 门禁(S6/B-4; docs/24 §5.4 D4)

本目录是深思 prompt(`app_soul/src/soul_think.c think_prompt`)的本地
评估门禁配置。**零凭据入 CI**——promptfoo 需要 API key 才能真跑,
本仓库 CI 不配置(本地手跑, 密钥走环境变量 `MINIMAX_KEY`)。

契约已同步 **v3.5-dose**(§55.29 预算裁决移除 + §55.33 appraisal
三元组); provider 为 custom JS 类(`minimax_provider.js`, callApi,
0.118 形态)。

## 用法(本地, 可选)

```bash
npx --yes promptfoo@0.118.8 eval -c tools/promptfoo/promptfooconfig.yaml
export MINIMAX_KEY=$(cat /home/emb/key/minimax.txt)   # 先于 eval
promptfoo view                    # 看报告
```

## 批量门禁的实际承载(本目录为可选 GUI 伴侣)

注册门禁与批量统计在 Python 线束(同 prompt 模板, PROMPT_VER 同步):

- `tools/think_prompt_bench.py` —— 千样本硬遵从/JSON/软理解(合同见
  docs/24 §5.4; 数字入 docs/19 台账);
- `tools/think_judge.py` —— 深思输出 **异模型 judge 自动化**
  (DashScope qwen, 供应商双隔离; 子分=emotion/over/persona)。

## 门禁合同(docs/24 §5.4)

- 确定性断言: JSON 合法率 100%/枚举合法/appraisal 对象在场/content
  无数字/停台锚禁 music;
- judge 子分(emotion_match/overacting/persona)三维均分 ≥7 且无单
  样本 ≤3 —— 自动化执行=`python3 -I tools/think_judge.py`;
- 改 think_prompt 必升 PROMPT_VER 且重跑 bench 门禁。
