#!/usr/bin/env python3
"""生成 3 段候选表情动画 GIF(480x480, 暗底适配 UI 主题)供 gif2efc 转换"""
from PIL import Image, ImageDraw
import math, os

S = 480
BG = (10, 10, 18)        # C_BG
CYAN = (64, 200, 248)    # C_ACCENT
GREEN = (127, 212, 168)  # C_AI
WHITE = (233, 239, 247)  # C_TXT

def save(frames, path, dur=90):
    frames[0].save(path, save_all=True, append_images=frames[1:],
                   duration=dur, loop=0)
    print(path, len(frames), "帧", os.path.getsize(path)//1024, "KB")

# ── 1. eyes: 大眼睛眨眼 + 瞳孔巡视 ─────────────────────────
def gen_eyes():
    frames = []
    N = 36
    for i in range(N):
        t = i / N
        im = Image.new("RGB", (S, S), BG)
        d = ImageDraw.Draw(im)
        # 眨眼包络: 周期 1 次快眨(0.42-0.55) + 常态
        blink = 1.0
        if 0.42 < t < 0.58:
            b = (t - 0.42) / 0.16
            blink = abs(math.cos(b * math.pi)) ** 2  # 1→0→1
        # 瞳孔左右缓巡
        px = 26 * math.sin(t * 2 * math.pi)
        for cx in (150, 330):
            ey, ery = 200, 86          # 眼心/眼纵半径
            if blink < 0.15:
                d.line([cx-70, ey, cx+70, ey], fill=WHITE, width=14)  # 闭眼线
                continue
            ry = int(ery * blink)
            # 眼白
            d.ellipse([cx-78, ey-ry, cx+78, ey+ry], fill=WHITE)
            # 瞳
            pr = int(min(ry * 0.92, 44))
            d.ellipse([cx+px-pr, ey-pr, cx+px+pr, ey+pr], fill=(20, 30, 50))
            pr2 = int(pr * 0.45)
            d.ellipse([cx+px-pr2, ey-pr2, cx+px+pr2, ey+pr2], fill=CYAN)
        # 微笑
        d.arc([150, 300, 330, 400], 20, 160, fill=GREEN, width=12)
        frames.append(im)
    return frames

# ── 2. pulse: 中心呼吸圆 + 音律环(音箱身份) ────────────────
def gen_pulse():
    frames = []
    N = 30
    for i in range(N):
        t = i / N
        im = Image.new("RGB", (S, S), BG)
        d = ImageDraw.Draw(im)
        ph = t * 2 * math.pi
        # 主圆呼吸
        r = 90 + 14 * math.sin(ph)
        d.ellipse([240-r, 240-r, 240+r, 240+r], outline=CYAN, width=10)
        # 三层涟漪
        for k in range(3):
            rr = 150 + 34 * ((t + k / 3) % 1.0)
            op = max(0, 1 - (rr - 150) / 100)
            if op > 0.05:
                c = (int(CYAN[0]*op), int(CYAN[1]*op), int(CYAN[2]*op))
                d.ellipse([240-rr, 240-rr, 240+rr, 240+rr], outline=c, width=6)
        # 底部音律条
        for k in range(7):
            x = 120 + k * 40
            h = 18 + 34 * abs(math.sin(ph + k * 0.9))
            d.rounded_rectangle([x, 430-h, x+22, 430], 8, fill=GREEN)
        frames.append(im)
    return frames

# ── 3. face: 圆脸表情(眨眼+嘴形随呼吸) ─────────────────────
def gen_face():
    frames = []
    N = 32
    for i in range(N):
        t = i / N
        im = Image.new("RGB", (S, S), BG)
        d = ImageDraw.Draw(im)
        ph = t * 2 * math.pi
        # 脸圆(呼吸缩放)
        r = 150 + 6 * math.sin(ph)
        d.ellipse([240-r, 240-r, 240+r, 240+r], fill=(24, 32, 52),
                  outline=WHITE, width=8)
        blink = 1.0
        if 0.5 < t < 0.62:
            b = (t - 0.5) / 0.12
            blink = abs(math.cos(b * math.pi)) ** 2
        for cx in (190, 290):
            if blink < 0.2:
                d.line([cx-22, 210, cx+22, 210], fill=WHITE, width=10)
            else:
                rr = int(16 * blink)
                d.ellipse([cx-16, 210-rr, cx+16, 210+rr], fill=WHITE)
        # 嘴: 微笑↔唱歌开合
        open_m = 0.5 + 0.5 * math.sin(ph)
        mw = 44 + int(18 * open_m)
        if open_m > 0.7:
            d.ellipse([240-mw//2, 280, 240+mw//2, 280+int(26*open_m)], fill=CYAN)
        else:
            d.arc([240-mw, 260, 240+mw, 320], 15, 165, fill=CYAN, width=10)
        # 腮红
        d.ellipse([130, 250, 170, 274], fill=(255, 107, 107))
        d.ellipse([310, 250, 350, 274], fill=(255, 107, 107))
        frames.append(im)
    return frames

os.makedirs("/tmp/faces", exist_ok=True)
save(gen_eyes(), "/tmp/faces/eyes.gif")
save(gen_pulse(), "/tmp/faces/pulse.gif")
save(gen_face(), "/tmp/faces/face.gif")
