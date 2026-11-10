# QSPI 屏幕问题调研与性能优化方案(2026-11-11)

> 背景:S31 + 鱼鹰 AM200Q460460FLS1(CO5300, 460×460) 人工调试确认
> **单线控制正常、多线(quad)刷新失败**;S3 官方测试板直插同一块屏完全正常
> (40MHz QSPI)。本文调研问题点(按证据强度排序)并给出性能优化路线。

## 1. QSPI 面板协议要点(定位问题的共同基础)

CO5300/SH8601 族 QSPI 面板上电默认 **3-wire SPI(9-bit: D/C+命令)**;
写 `0xC4 0x80`(QSPI 使能)后进入 QSPI 协议:

- **命令相位恒为单线**(D0):32-bit 头 = D/C 位 + 命令 + 参数计数
  → 这就是 esp_lcd 配置 `lcd_cmd_bits=32` 的原因;
- **数据相位四线**(D0-D3 并行)→ 像素带宽 ×4。

**推论**:"单线控制正常" = 32-bit 头经 D0 正确到达且面板已进 QSPI 模式
(否则头都会被误解、连控制都不会通);"多线刷新失败" = 四线数据相位断,
**故障面收敛到 D1/D2/D3 三根线或 S31 四线发送侧**。

## 2. 问题点清单(按证据强度排序)

### P1 ⭐ D1/D2/D3 杜邦线/接触(头号嫌疑)

证据:
- S3 验证时屏**直插测试板座子**,杜邦线从未被 S3 路径验证过;
- 40MHz 与 5MHz 同样失败(5MHz 对信号完整性极宽容 → 非时序/SI 问题,
  更像物理断路);
- 症状随插拔次数**逐次劣化直到完全无显示**(接触氧化/疲劳断裂的典型签名)。

验证方法(不依赖面板配合):
1. **GPIO 通路自检**:固件把 4 根数据脚临时配成 GPIO 输出,轮流转 0/1,
   万用表/示波器在**转接板侧**量 D0-D3——逐根验证导通(排除面板变量);
2. 摇线测试(已给过):摇到某根屏幕闪 → 凶手;
3. 全换新短线(≤10cm)重试。

### P2 S31 GPSPI 四线数据相位(芯片侧嫌疑,次优先)

- S31 为新芯片,IDF v6.1 preview;官方 S31 SPI LCD 文档确认支持
  quad_mode,但无已知问题清单 → 需实测排雷;
- 排查:① GPIO 驱动能力——pad 驱动默认档位对长线可能偏弱,可在总线
  初始化后对 46-49 调 `gpio_set_drive_capability(GPIO_DRIVE_CAP_3)`
  (pad 属性,不冲突 SPI 矩阵);② 换一组 GPIO(SPI 走 GPIO 矩阵,
  40/42/46-49 → 任意空闲脚)复测,排除特定 pad 问题;
- 仍失败 → espressif/esp-idf 提 issue(S31 QSPI LCD 状态页明确接收)。

### P3 spi_bus_lock 多核竞态(官方应用笔记条目)

- 官方 SPI/QSPI 屏应用笔记:**SPI 总线锁在多核场景有竞态**,建议
  `spi_bus_config_t.isr_cpu_id` 与 LVGL 任务同核;
- 我们 LVGL 在 core1、SPI 中断默认 core0 → 命中该模式;
- 但该竞态典型表现是**冻结/超时**(esp-idf#18177: dev_wait 卡死),
  与我们"完全不显示"不符 → 低嫌疑,但修法顺手(PCLK 提速前必做):
  `bus_cfg.isr_cpu_id = INTR_CPU_ID_AUTO_1`(与 LVGL 同核)。

### P4 PSRAM DMA 路径(已排除,记录)

- 老版本 SPI 驱动不能 GDMA 直发 PSRAM → 内部 SRAM bounce + memcpy;
- 我们已设 `flags.psram_dma_direct=1`(S31 支持,官方文档确认
  "has speed limit, but no temp buffer"),且早期用内部 RAM 测试帧
  也失败 → 排除。

### P5 兜底:单线慢刷模式(点亮优先,帧率牺牲)

- 面板在 QSPI 模式下数据相位定义为四线,单线像素流**协议上不成立**;
  但用户人工"单线控制"若指未发 0xC4 0x80 的 3-wire 模式下发命令成功,
  则说明面板默认模式可达 → 理论可停在 3-wire 模式用 9-bit 帧发
  0x2C+像素(esp_lcd 无现成 9-bit 主机支持,需自写 bit-bang 或扩展)。
- **定位为最后手段**(工作量≈P1/P2 排查,收益低:10MHz 单线 ≈ 3fps)。

## 3. 排查执行顺序(建议)

```
1. GPIO 通路自检(10 分钟,零风险) → 定位断线
2. 换线 + 重插 FPC → 复跑首亮彩条(内部 RAM 测试帧)
3. 仍失败 → 驱动能力 CAP_3 + 换 GPIO 组复测
4. 仍失败 → 最小时序示波器(有条件) / 提 espressif issue
5. 点亮后 → 第 4 节性能阶梯
```

## 4. 性能优化方案(点亮后执行,按收益排序)

### 4.1 带宽预算(460×460 RGB565 = 423KB/帧 = 3.38Mb/帧)

| PCLK(QSPI) | 帧传输耗时 | 理论上限 fps | 现实估计* | 30fps 红线 |
|---|---|---|---|---|
| 10MHz(现状) | 84.6ms | 11.8 | ~10 | ❌ 差 3 倍 |
| 20MHz | 42.3ms | 23.6 | ~20(杜邦线极限区) | ❌ 带宽 80<101.6Mbps |
| **40MHz(demo 默认)** | **21.2ms** | **47** | **~35-40** | ✅ 余量 36% |
| 80MHz(S3 论坛口径极限) | 10.6ms | 94 | 需 PCB/座接 | ✅(60fps 挑战用) |

**最低性能红线(docs/01 §1.3 定稿):全屏刷新稳定 30fps** → 带宽 101.6Mbps
硬性排除 10/20MHz;PCLK 40MHz 为唯一达标档(杜邦线=点亮过渡, PCB=验收形态)。

*含命令头开销、PSRAM 读竞争、LVGL 渲染并行;数据来自
LVGL 论坛 QSPI 500×500 帖与 esp_lvgl_port 性能指南。

### 4.2 优化阶梯

1. **PCLK 10→40MHz**(S3 直插已验证面板可跑 40MHz;杜邦线阶段先试
   20MHz,上 PCB 后 40MHz)——单点收益最大;
2. **isr_cpu_id 与 LVGL 同核**(core1):官方应用笔记的多核竞态修法,
   提速前的前置安全项;
3. **保持全帧 direct 双缓冲(PSRAM)+ psram_dma_direct**:
   esp_lvgl_port 性能指南——缓冲 >25% 屏幕无额外收益,但我们的
   氛围 UI 每帧全屏动画 → 全帧 direct 是正确形态(渲染与 DMA 并行);
4. **脏区传输**(M3):当前 `full_refresh=1` 强制整帧发送;氛围 UI 的
   直线画布只占 460×220 = 48% 屏幕 → 关 full_refresh 后 LVGL 只发
   脏区,**带宽立省一半以上**(歌词场景更小),静态页接近零流量;
5. **TE 引脚同步**(M3,已留 39):帧率超过面板扫描(≈60Hz)时防撕裂;
6. 系统项(性能指南口径):编译 Performance、LVGL 任务保持 core1、
   音频热路径 IRAM(已完成 libhelix noflash);
7. S31 Octal PSRAM @200MHz(16MB,Kaleido V-04 实证)——帧缓冲读取
   带宽充裕,psram_dma_direct 限速可接受。

### 4.3 不要做的

- 不要上 80MHz(杜邦线必挂;PCB 阶段再评估);
- 不要把 LVGL 缓冲挪回内部 SRAM(挤 WiFi/BT,历史上已吃过
  NO_MEM 开机循环);
- 不要开 LV_USE_VECTOR_GRAPHIC/ThorVG(CPU 重,自绘 AA 已覆盖需求)。

## 5. 参考链接

- 乐鑫 SPI/QSPI 屏应用笔记(PSRAM bounce/总线锁竞态/接线建议):
  docs.espressif.com/.../lcd-application-note/spi-qspi-summary.html
- ESP32-S31 SPI LCD 官方文档(quad/psram_dma_direct):
  docs.espressif.com/.../esp32s31/api-reference/peripherals/lcd/spi_lcd.html
- esp-idf#18177(CO5300 QSPI + 双缓冲 dev_wait 冻结,单缓冲绕过)
- LVGL 论坛 QSPI 500×500 60fps 帖(带宽计算:80MHz×4 线=320Mbps 上限)
- esp_lvgl_port 官方性能指南(25% 缓冲阈值/双缓冲/系统项量化 FPS)
