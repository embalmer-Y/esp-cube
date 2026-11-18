#!/usr/bin/env python3
"""face_reject_variants.py —— SD 热载拒绝码验收的坏文件矩阵生成器(§53 A3)

用冻结编码器(tools/efc/pc/efc_py.py, 环境 ~/.venvs/efc)产出"合法流"变体
(非 460 尺寸/无颜色键/全键第 0 帧) + 手工破坏变体(截断/伪 magic/超限/空)。
上板路径: 变体写入 SD face/<name>.efc 后逐个 `face <name>` 触发预检。

用法: ~/.venvs/efc/bin/python tools/face_reject_variants.py [输出目录(默认 /tmp/a3)]
预期拒绝码: good→OK(对照) badmagic/trunc/empty→BAD_FILE big→TOO_LARGE
            not460→NOT_460 nokey→BAD_FORMAT allkeyed→MOSTLY_KEYED
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "efc", "pc"))
import numpy as np                      # noqa: E402
import efc_py                           # noqa: E402

KEY = 0xF81F


def grad_frames(n, w, h):
    """任意非键色内容(渐变), 避开 0xF81F"""
    xs = np.arange(w, dtype=np.uint32)[None, :] * 31 // max(w - 1, 1)
    ys = np.arange(h, dtype=np.uint32)[:, None] * 63 // max(h - 1, 1)
    v = ((xs << 11) | ys | 0x0020).astype(np.uint16)
    return np.stack([v.copy() for _ in range(n)])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/a3"
    os.makedirs(out, exist_ok=True)

    enc = lambda a, k: efc_py.encode_frames(  # noqa: E731
        a, tile=4, color_key=k, compress=True, legacy=False)
    open(f"{out}/not460.efc", "wb").write(enc(grad_frames(3, 200, 200), None))
    open(f"{out}/nokey.efc", "wb").write(enc(grad_frames(3, 460, 460), None))
    fr = grad_frames(3, 460, 460)
    fr[0, :, :] = KEY                    # 第 0 帧全键色 → alpha0=1000‰
    open(f"{out}/allkeyed.efc", "wb").write(enc(fr, KEY))

    good = open(os.path.join(os.path.dirname(__file__), "..", "firmware",
                "components", "app_ui", "ui", "assets", "face", "laugh.efc"), "rb").read()
    open(f"{out}/good.efc", "wb").write(good)                     # 正例对照
    open(f"{out}/trunc.efc", "wb").write(good[:2048])             # 截断流
    open(f"{out}/badmagic.efc", "wb").write(bytes(range(256)) * 2)  # 伪 magic
    open(f"{out}/big.efc", "wb").write(good + b"\x00" * (300000 - len(good)))
    open(f"{out}/empty.efc", "wb").write(b"")

    for f in sorted(os.listdir(out)):
        print(f, os.path.getsize(os.path.join(out, f)))


if __name__ == "__main__":
    main()
