# 21 · 数据 schema 升级与回退方案（T48 前置整理，2026-11-14）

> 用途：docs/18 T48 验收句"固件与数据 schema 升级/回退和恢复方案"的前置整理（H 域）。
> 现有 schema 版本矩阵与各数据的升级/回退行为、兼容读语义、失败恢复路径。

## 1. schema 版本矩阵

| 数据域 | 位置 | 版本标识 | 升级行为 | 回退（旧固件读新数据） |
|---|---|---|---|---|
| 人格 registry | SD `v1/personas/registry`（store_slot 双槽） | `PSN_STORE_VER`（当前 **2**） | v1→v2：**旧格式丢弃重建**（T22 用户裁决：测试资料不保留） | 旧固件见未知版本 → RECOVERY_REQUIRED（双槽回退/引导重建） |
| 角色记忆 | SD `v1/personas/<key>/memory/episodes.tsv` + `deleted.log` | 无显式版本（格式=7 列 TSV，解析严格校验） | 字段演进=解析失败行跳过（宽进严出），不破坏原文件 | 旧固件读新字段行=多余列忽略（解析按前 7 列） |
| 删除台账 | 同上 `deleted.log` | 追加式（无版本头） | 追加即升级 | 旧固件逐行读，未知格式行跳过 |
| SD 记忆容器 | SD 全部 app_storage 文件 | **EPSCV2** magic（8B） | 明文→加密：写全加密、读双兼容（批次 16）；无迁移批处理（自然转密） | 旧固件读 EPSCV2 blob → magic 未知=按明文读出乱码 → 业务层解析失败（如实：不可读） |
| 凭据 | NVS `creds` 区（credstore） | EPSCV2 同族（dev_key 域） | 随 dev_key 生命期 | trust 区清除 → AUTH 拒 → 目录摘除 → NOTFND |
| 配对关系 | NVS `pair` 区（指纹表） | 单 blob 无版本 | 表损 → 出厂态（重配对） | 同左 |
| 证书 | NVS `pairc` 区（PEM） | 无版本（PEM 自描述） | 重灌即升级 | 旧/新 PEM 通用 |
| WiFi 凭据 | C5（esp_hosted slave） | C5 固件 3.0.10 管理 | 不归本项目 schema | esp_wifi erase → 重配网 |

## 2. 失败恢复路径（按失钥/损坏域）

| 场景 | 后果 | 恢复 |
|---|---|---|
| sd_key 丢（trust 区清除） | SD 记忆不可读（AUTH） | 已授权清除流程（无云备份=设计边界，docs/20 §6） |
| dev_key 丢 | 配对失效+凭据不可读 | `trustreset CONFIRM` → 重新配对+重灌证书+凭据重录 |
| NVS `pair` 表损 | 已配对关系失效 | 重配对（代价低，无数据损失） |
| SD 拔卡/坏块 | 记忆/素材不可用 | 双槽回退（registry）/优雅降级（无卡运行） |
| registry 双槽均损 | 人格全失 | RECOVERY_REQUIRED → 引导重建默认角色 |

## 3. 升级操作规程（首版）

1. **升级前**：`truststat` + `memstat` 留档；SD 有价值数据先由用户确认归属。
2. **升级**：全量擦后烧录（idf.py flash 不动 NVS；SD 数据就地兼容读/自然转密）。
3. **升级后首启**：观察 `psn`（registry 状态）、`memstat`（记忆条数）、`truststat`（密钥/配对）。
4. **回退**：旧固件可直接启动（新数据对旧固件按上表"回退"列表现）；不可读项如实呈现（不假装成功）。

## 4. FR/NFR 放行审查清单（T48 用，持续累积）

- [x] FR-14 自主行为默认开+总开关（cube_act autonomous+类别控制）
- [x] FR-21 勿扰默认关（cube_act quiet hours 默认 off；夜间 soul 行为另行记录）
- [x] FR-25/28 记忆本地 SD+角色隔离（T20/T21）
- [x] FR-12 三种音频隐私状态呈现（T18 首段，板证挂账）
- [ ] FR-07/08 唤醒词自定义（T25–T27，外部训练站未核实）
- [ ] FR-04 LLM 可切换（T30 首段完成，真实第二服务商待凭据）
- [ ] FR-18/19 电台+网易云原型（电台✅；网易云游客路径 T41 未动）
- [ ] NFR-03 日志无凭据/原文/音频（批次 16/22 已验：credstore at-rest+日志无明文；全量复查归 T48）
- [ ] ≥72h 混合运行 + ≥100 重启（T45/T48，冻结根治后执行）
