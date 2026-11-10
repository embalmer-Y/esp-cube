# EspCube PC 模拟器（LVGL9 + SDL2）

与固件**同源编译**的 UI 开发环境：页面/动画代码（`firmware/components/app_ui/ui/`）在
PC 上开发调试，屏幕修复后直接上板，UI 代码零分叉。

## 快速开始

```bash
# 1. 一次性安装 SDL2(无 root, apt-get download + 解包到 ~/.local/sdl2)
../tools/install_sdl2.sh

# 2. 构建并运行交互窗口
./run.sh

# 3. 无屏自动演示(全部场景 + shots/ 截图, CI/回归可用)
./run.sh --demo
```

## 交互

| 输入 | 动作 |
|---|---|
| 鼠标上下拖动 | 调音量(实时 HUD) |
| 鼠标左右滑动 | 切电台 / BT 切曲 |
| 滚轮 / ↑↓ | 音量 |
| 空格 | 播放/停止(电台) |
| N / P 或 ←→ | 下一曲 / 上一曲 |
| B | 蓝牙连接切换(BT 接管挂起电台, 与设备语义一致) |
| M | BT 切歌(元数据/歌词刷新) |
| T | 电台 StreamTitle 刷新 |
| S / F / H / ESC | 截图 / 帧率 / 帮助 / 退出 |

## 架构

```
┌─────────────── 同源 UI 代码 ───────────────┐
│ ui_model.h   应用状态快照 + ui_ctl_* 控制边界 │
│ ui_pages.c   极简氛围 UI(音频直线/歌词/手势)  │
│ cube_fft.c   频谱 FFT(与混音器抽头同款)      │
│ ui_font_cjk*.c  自生成 CJK 字体(全 GB2312)  │
└──────────────┬────────────┬───────────────┘
        设备后端 │            │ 模拟器后端
   ui_port_esp.c │            │ mock_app.c
  (事件总线→快照, │            │ (合成音频/BT/
   控制→app_*)   │            │  歌词时间线/语音脚本)
```

- 后端实现 `ui_model.h` 声明的快照与控制接口，**链接期绑定**；
- UI 以 30fps 轮询快照 + 变更检测，控制动作走 `ui_ctl_*`，无跨任务回调；
- 内存：设备侧 >4KB 分配自动落 PSRAM（canvas/字体位图），PC 侧走 libc 堆。

## 字体再生成

内置 `lv_font_source_han_sans_sc_16_cjk` 是日语子集（缺"语/设/蓝/唤"等），
本项目用自生成字体（Noto Sans CJK SC，A8 反走样，ASCII+全 GB2312+常用符号）：

```bash
python3 ../tools/gen_lv_font.py --size 18 --name ui_font_cjk18
python3 ../tools/gen_lv_font.py --size 24 --name ui_font_cjk24
```

注意：LVGL `SPARSE_TINY` cmap 的 `unicode_list` 必须存**相对码点**
（`letter - range_start`，见 `lv_font_fmt_txt.c`），生成器已处理。

## 已知取舍

- 交互窗口模式依赖 X11/Wayland 运行库（系统自带），开发仅需 SDL2 解包；
- `--demo` 用 `SDL_VIDEODRIVER=dummy` 无屏跑，截图到 `shots/*.bmp`；
- ASAN 构建：`cmake -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address -g -O1"
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`（曾抓出频谱峰值负索引越界）。
