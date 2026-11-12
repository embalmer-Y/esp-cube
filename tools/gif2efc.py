#!/usr/bin/env python3
"""gif2efc.py —— 动图 → EFC 表情素材转换(esp_efc_codec 纯 Python 编码器)

输入支持: GIF / APNG / 动画 WebP(PIL 多帧) 或 PNG/JPG 帧序列目录。
用法: python3 tools/gif2efc.py <输入.gif|.png|.webp|帧目录> [输出.efc] [--size 460] [--max-frames 60]
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



def cover_square(im: Image.Image, size: int, anchor: str = "center") -> Image.Image:
    """非正方形素材: 裁最大正方(anchor: center/top/bottom)再缩放 —— 避免
    直接 resize 的拉伸变形(2026-11-13 用户竖版素材 240x426)"""
    w, h = im.size
    side = min(w, h)
    if w == h == size:
        return im
    if w != h:
        left = (w - side) // 2
        top = {"top": 0, "bottom": h - side}.get(anchor, (h - side) // 2)
        im = im.crop((left, top, left + side, top + side))
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return im

def seq_to_rgb565_frames(path: str, size: int, max_frames: int, anchor="center"):
    """PNG/JPG 帧序列目录: 按文件名序合成(命名保证顺序, 如 f_001.png)"""
    import glob as _g
    files = sorted(_g.glob(os.path.join(path, "*.png"))
                   + _g.glob(os.path.join(path, "*.jpg")))
    if not files:
        raise SystemExit(f"目录无帧图: {path}")
    if len(files) > max_frames:
        step = len(files) / max_frames
        files = [files[min(int(i * step), len(files) - 1)]
                 for i in range(max_frames)]
    frames = []
    for f in files:
        fr = cover_square(Image.open(f).convert("RGB"), size, anchor)
        arr = np.asarray(fr, np.uint8)
        r5 = (arr[..., 0] >> 3).astype(np.uint16)
        g6 = (arr[..., 1] >> 2).astype(np.uint16)
        b5 = (arr[..., 2] >> 3).astype(np.uint16)
        frames.append((r5 << 11) | (g6 << 5) | b5)
    return np.stack(frames).astype("<u2"), [90] * len(frames)


def gif_to_rgb565_frames(path: str, size: int, max_frames: int, anchor="center"):
    im = Image.open(path)
    frames, durs = [], []
    try:
        while True:
            # RGBA → RGB(透明落黑底; EFC color key 演示层不需要)
            fr = im.convert("RGBA")
            bg = Image.new("RGBA", fr.size, (0, 0, 0, 255))
            fr = Image.alpha_composite(bg, fr).convert("RGB")
            fr = cover_square(fr, size, anchor)
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
    ap.add_argument("--size", type=int, default=460)
    ap.add_argument("--max-frames", type=int, default=60)
    ap.add_argument("--anchor", choices=["center","top","bottom"], default="center",
                    help="非正方素材裁方基准(默认居中)")
    a = ap.parse_args()
    if a.size % 4:
        raise SystemExit("--size 须为 4 的倍数")
    out = a.out or os.path.splitext(a.gif)[0] + ".efc"
    if os.path.isdir(a.gif):
        frames, durs = seq_to_rgb565_frames(a.gif, a.size, a.max_frames, a.anchor)
    else:
        frames, durs = gif_to_rgb565_frames(a.gif, a.size, a.max_frames, a.anchor)
    blob = encode_frames(frames, tile=4, compress=True)
    with open(out, "wb") as f:
        f.write(blob)
    avg = sum(durs[:len(frames)]) / max(len(durs), 1)
    print(f"{out}: {len(frames)}帧 @{a.size}x{a.size} "
          f"≈{avg:.0f}ms/帧 {len(blob)//1024}KB(zlib)")


if __name__ == "__main__":
    main()
