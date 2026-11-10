# 电台 App 移植鱼鹰 P4C5 开发板（ESP32-P4 + C5）

## Context

屏幕调试挂起（等转接板）。新任务：把网络电台链路从 EspCube S31 工程中剥离成**独立 App**，适配到新接入的鱼鹰光电 P4C5 开发板（ESP32-P4 v3.x + ESP32-C5 共芯，P4 无 WiFi/BT）。

**重大利好（三路调研结论）**：Kaleido 项目当年用的就是**同款鱼鹰 P4C5 板**，全部板级事实有真机验证记录归档：
- 音频：**ES8311 + NS4150B + LMA3729 麦**（与 S31 核心板同款芯片，仅引脚不同）：I2C SDA=7/SCL=8，MCLK=13、BCLK=12、WS=10、DOUT=9、DIN=11，PA_EN=GPIO53（codec open 后需手工 gpio 拉高——es8311 驱动 pa_pin 在本板不生效，V-38 实证）
- WiFi：C5 走 **SDIO slot1 4-bit @40MHz**（CLK=18/CMD=19/D0-D3=14-17，C5 复位=GPIO54，延时 1500ms）——恰为 esp_hosted 在 P4 的默认值，零引脚配置
- **本板 C5 固件 = esp-hosted 3.0.10 slave（用户已刷）** → host 3.0.10 × slave 3.0.10 **同版本配对**（比 Kaleido 混搭组合更稳）；初始化用显式两段式 + `AUTO_CALL_INIT_BEFORE_APP_MAIN=n` + `CACHE_L2_CACHE_LINE_64B=y`（V-45/#219 教训）
- 烧录：CH343P 串口，`ESPTOOLPY_NO_STUB=y`（stub 起不来，230400 波特），分区表偏移 0x10000，P4 下载 strap=GPIO35

## 复用清单（同源不拷贝）

| 组件 | 方式 | 说明 |
|---|---|---|
| cubecore、mp3_helix、app_player、app_audio | **symlink** 进新工程 | 全部平台无关（盘点实锤：mp3_helix 走 `__riscv` 分支零改动；mixer 对 drv_audio 仅 2 个调用点） |
| app_player 依赖 | 抄 `main/idf_component.yml` 三行 | esp_audio_codec ^2.6.0 / esp_hls_stream ^1.0.4（保 4 个内置 HLS 台）|
| app_net / drv_audio / boardcfg | **radio 工程本地新建** | 见下 |

不带入：app_bt / app_ui / app_voice / app_ai_session / speech_provider / drv_display（P4 无 BT，电台 App 无 UI/语音）。

## 新工程结构 `radio-p4c5/`（仓库新顶层目录）

```
radio-p4c5/
├── CMakeLists.txt
├── partitions.csv                    # nvs/phy/factory4MB, 表偏移 0x10000(Kaleido 同款)
├── sdkconfig.defaults                # 见下
├── .gitignore                        # sdkconfig/build 本地化
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml             # esp_hosted 3.0.10 + esp_codec_dev 1.6.2 + esp_audio_codec + esp_hls_stream
│   ├── Kconfig.projbuild             # WIFI_SSID/PASSWORD(占位)/DEFAULT_VOLUME/NET_PREFILL_MS
│   └── main.c                        # NVS→drv_audio→mixer→player_init→net_start→(WIFI_CONNECTED)→player_start
└── components/                       # 4 个 symlink + 3 个新组件
    ├── cubecore -> ../../firmware/components/cubecore
    ├── mp3_helix -> ../../firmware/components/mp3_helix
    ├── app_player -> ../../firmware/components/app_player
    ├── app_audio  -> ../../firmware/components/app_audio
    ├── boardcfg/                     # P4C5 引脚表(唯一登记处惯例延续)
    ├── drv_audio/                    # ES8311 @48k 立体声(esp_codec_dev 1.6.2, 移植 Kaleido audio_io.c)
    └── app_net/                      # esp_hosted 两段式 + 标准 esp_wifi STA(透传)
```

## 关键实现点

1. **drv_audio_p4**（新写，~150 行）：Kaleido `voice-loop-p4/main/audio_io.c:45-161` 的 proven 序列——i2c master(glitch 7) → i2s_std 全双工(auto_clear=true, 8×dma) → audio_codec i2s/i2c data → `es8311_codec_new`(slave+mclk+pa53) → open 48000/2ch/16bit → **PA53 手工 gpio_config 拉高**。对外暴露现有 `drv_audio.h` 同名 API（init/output/set_volume/pa_set），mixer 零改动；mic_read 提供空实现（无语音）。
2. **app_net_p4**（改写自 app_net.c，~150 行）：`app_net_start()` = nvs → netif_init → event_loop → **`esp_hosted_init()` → `esp_hosted_connect_to_slave()`（阻塞~1.5s，失败降级不 abort）** → 标准 esp_wifi STA 序列（esp_wifi_remote 透传，API 与 S31 版相同）；删 BT suspend/PS_NONE/coex；保留 `CUBE_EVT_WIFI_CONNECTED/DISCONNECTED` 发布。
3. **main.c**（~120 行）：S31 main 的电台最小集裁剪版；`WIFI_CONNECTED → app_player_start(NULL)` 轮播桥接照搬；失败域策略照搬（audio 链 abort、net/player 降级）。
4. **sdkconfig.defaults**（Kaleido P4 实证口径）：`IDF_TARGET=esp32p4`、400MHz、HEX PSRAM 32MB@200M+SPIRAM_USE_MALLOC+ALLOW_STACK_EXTERNAL(player 12KB PSRAM 栈)、flash 16MB QIO 80M+BOYA 支持、**ESPTOOLPY_NO_STUB=y**、分区表 0x10000、FREERTOS_HZ=1000、**CACHE_L2_CACHE_LINE_64B=y**、HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n。
5. WiFi 凭据沿用 BYOK 政策：defaults 只放占位符，真实 SSID/密码进本地 `radio-p4c5/sdkconfig`（gitignore，同 firmware/ 现状）。

## 执行顺序（约 4 个 commit）

1. 工程骨架：目录/symlink/partitions/sdkconfig.defaults/Kconfig/idf_component.yml/main.c
2. boardcfg+drv_audio（48k 立体声 ES8311）+ 首次 `idf.py set-target esp32p4` 全量编译通过
3. app_net_p4（hosted 两段式）+ 全链编译
4. 上板：烧录 → 串口验证 hosted 建链/WiFi IP/HLS playing/mixer health peak → 喇叭出声（J9 需接喇叭；无喇叭则以 mixer peak 为准）

## 验证

- 编译：`idf.py -B build set-target esp32p4 && idf.py build`（同 /home/emb/esp/esp-idf v6.1 定制分支，P4 为正式 target）
- 上板串口里程碑：`esp_hosted connect ok` → `got ip` → `player: playing ...playlist.m3u8 (hls)` → `mixer: health: peak=非0 net_buf=3-5s` → 连续 5 分钟无重启
- 用户听感：J9 喇叭放电台（广东音乐之声卫星台）
- S31 工程零改动（symlink 单向依赖，check_layers 不涉及新工程）

## 风险与对策

- **C5 3.0.10 与 host 对接异常**（芯片枚举/版本协商失败）：备用 = Kaleido c5-recovery SOP 重刷 C5（恢复包含 v661=IDF6.1 构建的 3.0.10 slave 镜像与烧写步骤，`kaleido-fw/tools/hil/c5-recovery/`）
- **esp_codec_dev 1.6.2 vs 我们 S31 的 2.0.0-beta5**：radio 工程独立锁 1.6.2（Kaleido 本板实证），互不影响
- 喇叭未接：以串口 mixer peak 验证 DAC 链路，听感留给用户接喇叭后确认