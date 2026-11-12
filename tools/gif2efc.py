#!/usr/bin/env python3
"""gif2efc.py —— GIF → EFC 表情素材转换(esp_efc_codec 纯 Python 编码器)

用法: python3 tools/gif2efc.py <输入.gif> [输出.efc] [--size 480] [--max-frames 60]
输出缺省 = 输入名.efc。编码 4x4 tile + zlib(设备 ESP_EFC_DECODE_4X4_ONLY
默认路径); 分辨率须 4 的倍数, 缺省 480(设备 CONTAIN_DOWNSCALE 到 460)。

素材管线(docs/09 face/ 目录): GIF(任意来源) → 本工具 → PC http.server
→ 设备 CLI `wget http://<pc>:8000/x.efc face/x` → `face x` 热切。
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

PC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "firmware", "managed_components",
                      "espressif__esp_efc_codec", "pc")
sys.path.insert(0, PC_DIR)
from efc_py import encode_frames  # noqa: E402


def gif_to_rgb565_frames(path: str, size: int, max_frames: int):
    im = Image.open(path)
    frames, durs = [], []
    try:
        while True:
            # RGBA → RGB(透明落黑底; EFC color key 演示层不需要)
            fr = im.convert("RGBA")
            bg = Image.new("RGBA", fr.size, (0, 0, 0, 255))
            fr = Image.alpha_composite(bg, fr).convert("RGB")
            if fr.size != (size, size):
                fr = fr.resize((size, size), Image.LANCZOS)
            arr = np.asarray(fr, np.uint8)
            r5 = (arr[..., 0] >> 3).astype(np.uint16)
            g6 = (arr[..., 1] >> 2).astype(np.uint16)
            b5 = (arr[..., 2] >> 3).astype(np.uint16)
            frames.append((r5 << 11) | (g6 << 5) | b5)
            durs.append(im.info.get("duration", 80) or 80)
            im.seek(im.tell() + 1)
    except EOFError:
        pass
    if not frames:
        raise SystemExit(f"无帧可读: {path}")
    if len(frames) > max_frames:
        step = len(frames) / max_frames
        idx = [min(int(i * step), len(frames) - 1) for i in range(max_frames)]
        frames = [frames[i] for i in idx]
    return np.stack(frames).astype("<u2"), durs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--size", type=int, default=480)
    ap.add_argument("--max-frames", type=int, default=60)
    a = ap.parse_args()
    if a.size % 4:
        raise SystemExit("--size 须为 4 的倍数")
    out = a.out or os.path.splitext(a.gif)[0] + ".efc"
    frames, durs = gif_to_rgb565_frames(a.gif, a.size, a.max_frames)
    blob = encode_frames(frames, tile=4, compress=True)
    with open(out, "wb") as f:
        f.write(blob)
    avg = sum(durs[:len(frames)]) / max(len(durs), 1)
    print(f"{out}: {len(frames)}帧 @{a.size}x{a.size} "
          f"≈{avg:.0f}ms/帧 {len(blob)//1024}KB(zlib)")


if __name__ == "__main__":
    main()
