# promptfoo 深思 prompt 门禁(S6/B-4; docs/24 §5.4 D4)

本目录是深思 prompt(`app_soul/src/soul_think.c think_prompt`)的本地
评估门禁配置。**零凭据入 CI**——promptfoo 需要 API key 才能真跑,
本仓库 CI 不配置(本地手跑, 密钥走环境变量 `MINIMAX_KEY`)。

## 用法(本地, 可选)

```bash
npm install -g promptfoo          # 或 npx promptfoo
export MINIMAX_KEY=$(cat /home/emb/key/minimax.txt)
promptfoo eval --config tools/promptfoo/promptfooconfig.yaml
promptfoo view                    # 看报告
```

## 门禁合同(docs/24 §5.4)

- 确定性断言(零成本, CI 可做后续): JSON 合法率 100%/枚举合法/
  silent 基率带 [0.4,0.9]/content 无数字;
- judge 子分(emotion_match/overacting/persona, 异模型优先——当前仅
  MiniMax 单供应商, judge=同模型隔离上下文, 偏差已入册 docs/24 §14):
  均分 ≥7 且无单样本 ≤3;
- 20% 用例 held-out 防过拟合; 改 think_prompt 必跑本门禁。

## 用例

`cases.yaml` 内 9 锚点状态 × 3 剂量(工作初值), 与 modeval.py 的
禁词断言互补(角色/引用/格式三层)。
