# EspCube · 立方体小音响

ESP32-S31(乐鑫 2026)+ 鱼鹰光电 2寸 QSPI AMOLED(460×460,CO5300)+ Peerless TC5FC07-04(1.5寸全频 4Ω)。
功能:手机蓝牙 A2DP 音乐 / 网络电台 / 语音唤醒 + 大模型控制(AI DJ)/ 电池供电。

## 当前状态(2026-11-09)

- **M1 音频+UI 闭环已完成**:输出链路(ES8311→NS4150B→喇叭)真实录音验收;MP3 连续播放(libhelix 解码);蓝牙 A2DP Sink 就绪;WiFi + 播放列表轮播。
- **MP3 解码选型**:`chmorgan/esp-libhelix-mp3`(esp_audio_codec 的 MP3 即闭源 minimp3 派生,S31 上慢且吞帧,详见 docs/01 §网络音频栈)。
- 固件 `idf.py build` 通过,app ~3.0MB(factory 4MB,余 25%)。
- 下一步:国内电台源+HLS → M2 语音 AI(esp-sr + 自建 GLM 服务端)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/01_技术方案.md](docs/01_技术方案.md) | 调研结论(带来源)、选型决策表、风险清单 |
| [docs/02_详细设计.md](docs/02_详细设计.md) | 架构图、模块接口、音频引擎/语音AI/UI 设计、任务内存预算 |
| [docs/03_接线与验证指南.md](docs/03_接线与验证指南.md) | 接线表、烧录步骤、上电验证清单、待核对项 |

## 固件

```
firmware/
├── main/                 # 装配层(初始化顺序 + 演示入口)
└── components/
    ├── cubecore/         # 事件总线/环形缓冲/重采样/FFT
    ├── boardcfg/         # 引脚集中登记(board_pins.h)
    ├── drv_display/      # CO5300 QSPI AMOLED
    ├── drv_audio/        # ES8311 + I2S(esp_codec_dev v2)
    ├── app_audio/        # 48k 混音引擎(ducking/AEC抽头/频谱)
    ├── app_player/       # 网络音源: icy_stream(ICY协议) + 播放任务(MP3=libhelix/AAC=esp_audio_codec/PCM直通)
    ├── app_bt/           # A2DP Sink + AVRCP(S31 原生经典蓝牙)
    ├── app_net/          # WiFi STA
    ├── app_ui/           # LVGL 9(全帧双缓冲/频谱/3D立方体)
    ├── app_voice/        # (M2)esp-sr 唤醒/AEC/命令词
    └── app_ai_session/   # (M2)AI 会话状态机 + xiaozhi/GLM 适配器
```

构建:IDF v6.1,`cd firmware && idf.py --preview set-target esp32s31 && idf.py build`。
WiFi 凭据经 `idf.py menuconfig` → EspCube Configuration 填写(不入侵源码)。

里程碑:M1 音频+UI 闭环(本轮)→ M2 语音 AI → M3 触摸/配网 → M4 AI DJ → M5 最终硬件(DSP codec)。
