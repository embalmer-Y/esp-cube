#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_lv_font.py —— 用系统 TTF/TTC 生成 LVGL9 fmt_txt 字体 C 文件(A8 反走样)

背景: LVGL 内置 lv_font_source_han_sans_sc_16_cjk 是面向日语的子集,
缺"语/设/蓝/唤"等常用简体字, 不可用于本产品 UI。
本脚本用 Pillow 渲染全 GB2312(6763 字)+ASCII+常用符号, 输出与
lv_font_conv 等价结构的 lv_font_t, 模拟器与固件共用同一产物。

用法:
    python3 tools/gen_lv_font.py [--size 18] [--name ui_font_cjk18] \
        [--out-dir firmware/components/app_ui/ui]

注意: 输出字体要求 lv_conf 开启 LV_FONT_FMT_TXT_LARGE(位图 >1MB,
20bit bitmap_index 装不下)。sim/lv_conf.h 与设备 sdkconfig 均已配置。
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# 候选字体: 优先 Noto Sans CJK SC(观感好), 退化到 Droid 后备字体
FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
]

EXTRA_SYMBOLS = (
    "·—…、。，？！：；“”‘’（）《》【】—～￥％℃→↑↓←播放暂停音量电台蓝牙设置"
    "频谱缓冲连接断开重试成功失败正在空闲唤醒聆听思考播报网络亮度关于版本"
)


def gb2312_charset():
    """全 GB2312 汉字区(区位 16..87) → unicode 集合"""
    chars = set()
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def pick_font(px):
    for path in FONT_CANDIDATES:
        if not Path(path).exists():
            continue
        try:
            if path.endswith(".ttc"):
                # 遍历 face 找 SC(简体), 避免日文优先的默认 face
                for idx in range(8):
                    try:
                        f = ImageFont.truetype(path, px, index=idx)
                    except OSError:
                        break
                    full = " ".join(f.getname())
                    if "SC" in full or "Simplified" in full:
                        return f, f"{path}#{idx} {full}"
            f = ImageFont.truetype(path, px)
            return f, path
        except OSError:
            continue
    sys.exit("未找到可用 CJK 字体(fc-list :lang=zh 确认)")


def render_glyph(font, asc, ch):
    """单字形渲染 → (A8 bytes, adv_w*16, box_w, box_h, ofs_x, ofs_y_from_line_top)"""
    pad = 6
    adv = int(round(font.getlength(ch) * 16))
    canvas_w = int(font.getlength(ch)) + pad * 2 + 2
    canvas_h = font.size + pad * 2  # getmetrics 上限之内
    img = Image.new("L", (max(canvas_w, 4), max(canvas_h, 4)), 0)
    ImageDraw.Draw(img).text((pad, pad + asc), ch, font=font, fill=255, anchor="ls")
    bbox = img.getbbox()
    if bbox is None:
        return b"", adv, 0, 0, 0, 0
    x0, y0, x1, y1 = bbox
    crop = img.crop(bbox)
    ofs_x = x0 - pad
    ofs_y = y0 - pad  # 图内 y=0 即行顶(baseline 放在 asc), 直接就是距行顶偏移
    return crop.tobytes(), adv, x1 - x0, y1 - y0, ofs_x, ofs_y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=18)
    ap.add_argument("--name", default="ui_font_cjk18")
    ap.add_argument("--out-dir", default="firmware/components/app_ui/ui")
    ap.add_argument("--no-gb2312-l2", action="store_true",
                    help="只留一级汉字(3755), 产物减半")
    args = ap.parse_args()

    px = args.size
    font, src_path = pick_font(px)
    asc, desc = font.getmetrics()
    print(f"字体: {src_path}  size={px}  asc={asc} desc={desc}")

    chars = set(chr(c) for c in range(0x20, 0x7F))  # ASCII
    chars |= gb2312_charset()
    if args.no_gb2312_l2:
        # 区位 16..55 = 一级汉字
        chars = {c for c in chars if ord(c) < 0x10000 and (
            ord(c) < 0x4E00 or _in_level1(c))}
    chars |= set(EXTRA_SYMBOLS)
    chars.discard("\x7F")
    cps = sorted(ord(c) for c in chars if ord(c) < 0x10000)  # unicode_list 为 uint16
    print(f"字符数: {len(cps)}")

    bitmaps = [b""]
    dscs = [(0, 0, 0, 0, 0, 0)]  # glyph 0 = 未定义占位
    total_bytes = 0
    for cp in cps:
        bm, adv, bw, bh, ox, oy = render_glyph(font, asc, chr(cp))
        bitmaps.append(bm)
        dscs.append((total_bytes, adv, bw, bh, ox, oy))
        total_bytes += len(bm)
    print(f"位图总量: {total_bytes/1024:.0f} KB (A8)")

    out_c = Path(args.out_dir) / f"{args.name}.c"
    out_h = Path(args.out_dir) / "ui_font_cjk.h"
    with open(out_c, "w", encoding="utf-8") as f:
        w = f.write
        w(f"/*\n * {args.name}.c —— 自生成 CJK 字体(勿手改; 再生成: tools/gen_lv_font.py)\n"
          f" * 源: {src_path} @ {px}px, A8, {len(cps)} 字符, "
          f"位图 {total_bytes/1024:.0f}KB\n"
          f" * 覆盖: ASCII + 全 GB2312 + 常用符号\n"
          f" * 依赖: LV_FONT_FMT_TXT_LARGE=1 (位图超 1MB)\n */\n\n")
        w('#include "lvgl.h"\n\n')
        w(f"const uint8_t {args.name}_bitmap[] = {{\n")
        w("    /* glyph 0 未定义(零字节) */\n")
        for i, bm in enumerate(bitmaps[1:], 1):
            if bm:
                w(f"    /* U+{cps[i-1]:04X} {chr(cps[i-1])!r} */ " +
                  ",".join(f"0x{b:02X}" for b in bm) + ",\n")
            else:
                w(f"    /* U+{cps[i-1]:04X} {chr(cps[i-1])!r} */\n")
        w("};\n\n")
        w(f"const lv_font_fmt_txt_glyph_dsc_t {args.name}_glyph_dsc[] = {{\n")
        w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
        for (bi, adv, bw, bh, ox, oy) in dscs[1:]:
            w(f"    {{.bitmap_index = {bi}, .adv_w = {adv}, .box_w = {bw}, "
              f".box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy}}},\n")
        w("};\n\n")
        # LVGL SPARSE_TINY: 二分查找键为 rcp = letter - range_start,
        # unicode_list 必须存【相对码点】(见 lv_font_fmt_txt.c:342/356)
        w(f"const uint16_t {args.name}_unicode_list[] = {{\n")
        for i in range(0, len(cps), 12):
            w("    " + ",".join(f"0x{cp - cps[0]:04X}" for cp in cps[i:i+12]) + ",\n")
        w("};\n\n")
        w(f"const lv_font_fmt_txt_cmap_t {args.name}_cmaps[] = {{\n"
          f"    {{.range_start = {cps[0]}, .range_length = {cps[-1]-cps[0]+1}, "
          f".glyph_id_start = 1,\n"
          f"     .unicode_list = {args.name}_unicode_list, .glyph_id_ofs_list = NULL, "
          f".list_length = {len(cps)},\n"
          f"     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}},\n"
          f"}};\n\n")
        w(f"const lv_font_fmt_txt_dsc_t {args.name}_dsc = {{\n"
          f"    .glyph_bitmap = {args.name}_bitmap,\n"
          f"    .glyph_dsc = {args.name}_glyph_dsc,\n"
          f"    .cmaps = {args.name}_cmaps,\n"
          f"    .kern_dsc = NULL, .kern_scale = 0,\n"
          f"    .cmap_num = 1, .bpp = 8, .kern_classes = 0, .bitmap_format = 0,\n"
          f"    .stride = 0, .are_glyphs_dynamic_loaded = false,\n"
          f"}};\n\n")
        w(f"const lv_font_t {args.name} = {{\n"
          f"    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
          f"    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
          f"    .line_height = {asc + desc}, .base_line = {desc},\n")
        cap_h = render_glyph(font, asc, "H")[3]
        x_h = render_glyph(font, asc, "x")[3]
        w(f"#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10\n"
          f"    .cap_height = {cap_h}, .x_height = {x_h},\n#endif\n")
        w("    .subpx = LV_FONT_SUBPX_NONE,\n"
          "    .underline_position = -1, .underline_thickness = 1,\n"
          "#if LV_VERSION_CHECK(9, 3, 0)\n"
          "    .static_bitmap = 1,\n#endif\n"
          f"    .dsc = &{args.name}_dsc, .fallback = NULL, .user_data = NULL,\n"
          f"}};\n")
    with open(out_h, "w", encoding="utf-8") as f:
        f.write("/*\n * ui_font_cjk.h —— 自生成 CJK 字体(全 GB2312 + ASCII, A8)\n"
                " * 生成: python3 tools/gen_lv_font.py\n */\n#pragma once\n\n"
                "#include \"lvgl.h\"\n\n"
                "extern const lv_font_t ui_font_cjk18;\n")
    print(f"输出: {out_c} ({out_c.stat().st_size/1e6:.1f} MB), {out_h}")


def _in_level1(c):
    """区位 16..55(一级常用汉字)判定: 用回 GB2312 编码试探"""
    try:
        b = c.encode("gb2312")
    except UnicodeEncodeError:
        return False
    return 0xB0 <= b[0] <= 0xD7


if __name__ == "__main__":
    main()
