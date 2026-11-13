# 12 · LLM 通信与情绪引擎前沿调研(四路子智能体全网检索, 2026-11-13)

> 承接 docs/11 §6 的问题(状态如何送 LLM/模型适配/误差校准), 四路并行
> 检索: ①情感注入 LLM ②人格一致性与指令遵循 ③LLM 输出校准(黑盒 API)
> ④业界情感 AI 伴侣产品。本文档为合并精华+产品落地映射。

## 1. 四路独立收敛的架构共识(交叉验证)

**① 状态持有权归本地引擎, LLM 只读/表达** —— 五重独立验证:
- Regan 2024: 让 LLM 自评"该产生什么情绪"不可靠(arXiv:2402.04232)
- Control Illusion 2025: LLM"承认指令却不执行", 指令层级失效
  (arXiv:2502.15851) —— "口头答应不执行"的学术对应物
- Loona 产品实证: LLM 直接选表情一个月后被官方回滚(不可控)
- openfeelz 纪律: 情绪写权限只归引擎, LLM 只能提议事件+过置信门限
- 小冰 IQ/EQ 分离架构(2018 论文) —— 我们的"本地情绪引擎+LLM 大脑"
  是 EQ/IQ 分离在端侧的重演
**→ 我们 Phase1 的架构选择(ke/kd 本地权威+LLM 表达)方向正确。**

**② 叙事化注入优于参数注入, 但剂量要克制**:
- Sentipolis(2026): PAD→KNN 情绪标签→结合人设/记忆的短叙事注入,
  情绪连续性提升 100-300%; **小模型(GPT-4o-mini 级)会过度表演穿帮**
- E-STEER(2026 机制研究): 倒 U 剂量效应(中等唤醒最优); **情绪主要
  影响决策规划层而非文风层** —— 注入重心放深思/决策 prompt
- Anthropic 功能情绪(2026): LLM 内部确有可激活的线性情绪表征
  —— 注入=激活已有方向, 不是玄学

**③ 注入位置三重证据 → 三层注入架构**:
- Lost in the Middle(U 形注意力) + Instruction Position Matters
  (指令靠生成点最优) + Multi-IF(多轮必衰减需周期重申) + SillyTavern
  社区十年实践(Author's Note @ Depth 0-4)
- 落地: 静态人格 system 卡 + **动态状态 depth 0-2 注入块**(对话历史
  之后、生成之前) + 每 N 轮末尾重申硬禁令

**④ 状态变更必须走结构化通道**:
- JSONSchemaBench(2025): API 原生 structured output 接近 100% 遵循,
  纯 prompt 复杂 schema 跌到 13-21%
- BFCL/τ-bench: 选模型看 pass^k 口径; 关键轮强制工具调用
- 落地: mood 标签/ki_intent 走 strict JSON(response_format), 台词走
  自由文本(双通道拆分, 硬约束伤文笔)

## 2. 可直接融入产品的成果(按投入产出排序)

| # | 成果 | 来源 | 落地 |
|---|---|---|---|
| 1 | **openfeelz 数值情绪引擎**(MIT, 可整体移植) | github.com/trianglegrrl/openfeelz | PAD+OCEAN; **指数衰减回归人格基线**(半衰期分维: 惊讶5h/唤醒8h/信任20h); 人格调制衰减(高神经质→负面消退慢); Rumination 反刍(>0.7 触发多阶段持续); **只注入偏离基线>0.15 的维度**(省token+可解释); 置信门限+钳制+可reset 防失控全套 —— ke 引擎升级参考+Phase 2b trait 基线衔接 |
| 2 | **校准层三件套** | SSoT(Sakana 2025)/分位数映射/置信分箱 | ①SSoT seed 技巧: LLM 先产 seed 字符串再推导数值, 打散 mood delta 窄区间聚集(几行 prompt) ②分位数映射: LLM 输出→经验CDF→目标分布逆CDF, 换模型只需重标定(气象水文成熟方法) ③口头置信度只信排序不信绝对值(Xiong ICLR24), 分箱映射到历史命中率 |
| 3 | **EmoACT 表情选择器+频率结论** | arXiv:2504.12125 | 情绪向量→余弦相似度匹配预定义段+**60%阈值兜底 Neutral**(MCU 最简); **高频情绪展示才自然**(低频变脸反而显得神经质——30人对照实验) |
| 4 | **Sims moodlet 时效机制** | GDC/GameDeveloper 访谈 | 情绪 buff 自带过期时间=天然防失控; **情绪门控行为菜单**(无聊→解锁主动搭话, 开心→解锁小舞蹈)而非只换表情 |
| 5 | **EmojiVoice 离散标签注 TTS** | Mirokaï 论文(arXiv:2506.15085) | 情绪→emoji 式离散风格标签注入 TTS, **按短语粒度切换**; 标签选错比不选伤害大→标签由本地状态决定 |
| 6 | **情绪保真回归测试** | EmoCharacter(NAACL25)/CharacterEval(ACL24 中文) | 固定状态集×固定脚本→LLM-as-judge 打情绪一致性分, 纳入 CI(兼模型选型); CharacterEval 证明中文模型中文角色扮演普遍优于 GPT-4 |
| 7 | **promptfoo+DSPy/OPRO 自动寻优** | 生产级组合 | 校准误差做评分函数, 措辞迭代自动回归门禁, 防模型换代校准漂移 |
| 8 | **人格卡三件套+后台记忆整理** | Character.AI 亿级验证 | 描述+问候语+示例对话; 记忆整理后台异步(摘要+关键事实分存) |
| 9 | **Replika 反例+Moflin 底线** | 产品史 | 人格押模型权重极脆弱(ERP 事件全体突变)→显式本地人格卡; 无 LLM 纯本地情绪也能撑起情感连接(音调/动作映射)→断网降级"活着"设计 |
| 10 | **Generative Agents 轻量版** | 官方+中文复现 | 记忆四元组{文本,时间,importance,效价}; 三因子检索零LLM; 重要性累计超阈才反思一次 —— 我们 soul_mem 已同构, 补 importance 累计反思 |

## 3. 与现有架构的差量清单(待实施)

立即(注入层改造, 半天-一天):
- soul_prompt_suffix → 叙事化模板(带因果体验描述, 克制长度适配小模型)
- 状态注入位置移到 depth 0-2(system 内尾部重组)
- 深思 prompt 同步叙事化(情绪影响决策层的实证落地)

短期(soul_calib 校准层, 一天):
- mood delta 分位数映射去偏+展宽; confidence 分箱归一
- SSoT seed 技巧进 mood 协议 prompt

中期(协议与测试, 2-3 天):
- mood/intent 走 DashScope response_format 严格模式(验证支持度)
- 情绪保真回归测试集(固定状态×脚本×judge) + promptfoo 门禁

Phase 2b 衔接(性格演化):
- openfeelz 衰减回归基线 = trait 底座的动力学实现参考
- Sentipolis 双速更新 = 统计式演化的完整配方
- 注入克制纪律(小模型过度表演)写进渲染模板设计约束

表情/表现层升级(Phase 3):
- 余弦匹配+Neutral 兜底选择器(替代/增强现有最近邻)
- moodlet 过期机制; 情绪门控行为菜单(无聊→主动搭话已近似, 扩展)
- TTS 风格标签按短语切换(cosyvoice 支持度验证)
