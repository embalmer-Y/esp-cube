#!/usr/bin/env python3
"""eaf_codec.py —— EAF 宿主编解码+460 重编码(T36 素材重编码; 批次 42)

格式参照 esp_lv_eaf_player/src/esp_eaf_dec.c(设备端解析器, 本工具产出
必须能被其原样解析):
  文件 = 0x89+"EAF" + count u32 + checksum u32 + total u32
       + 帧表(count×{size u32, off u32}) + 0x5A5A + 帧数据
  帧   = "_S" + ver(6) + depth u8 + w u16 + h u16 + blocks u16
       + block_h u16 + block_len[](blocks×u32) + palette(256×4 BGR)
       + 块数据(各块 = enc u8 + RLE 载荷)
  尾条目 = "_C" + 元数据(编码器签名)

用法:
  python3 tools/eaf_codec.py decode IN.eaf [--png out.png]
  python3 tools/eaf_codec.py re460 IN.eaf OUT.eaf [size]
"""
import struct
import sys
from pathlib import Path

DATA_HEAD = b"\x5a\x5a"                 # 数据区头(magic 0x5A5A)
BLOCK_HEIGHT = 92                          # 460/5 块


# ================= 解码 =================

def decode_file(data: bytes):
    assert data[0] == 0x89 and data[1:4] == b"EAF", "bad magic"
    count = struct.unpack("<I", data[4:8])[0]
    data_base = 16 + count * 8 + 2         # 帧表 + 0x5A5A
    out = []
    prev = None
    for i in range(count):
        fsize, foff = struct.unpack("<II", data[16 + i * 8: 24 + i * 8])
        fd = data[data_base + foff: data_base + foff + fsize]
        if fd[0:2] == b"_C":
            if prev is not None:
                out.append(dict(prev))
            continue
        assert fd[0:2] == b"_S", f"frame{i} not _S"
        depth = fd[9]
        w, h = struct.unpack("<HH", fd[10:14])
        blocks = struct.unpack("<H", fd[14:16])[0]
        block_h = struct.unpack("<H", fd[16:18])[0]
        num_colors = 1 << depth if depth == 8 else 0
        pal_off = 18 + blocks * 4
        block_lens = struct.unpack(f"<{blocks}I", fd[18: 18 + blocks * 4])
        palette = []
        if num_colors:
            for c in range(num_colors):
                b_, g_, r_ = fd[pal_off + c * 4: pal_off + c * 4 + 3]
                palette.append((r_, g_, b_))
        blk_off = pal_off + num_colors * 4
        img = bytearray(w * h)
        for b in range(blocks):
            enc = fd[blk_off]
            blen = block_lens[b]           # 含 enc 字节(设备端语义)
            payload = fd[blk_off + 1: blk_off + blen]
            assert enc == 0, f"blk{b} enc={enc} 未实现(RLE 之外)"
            start_row = b * block_h
            nrows = min(block_h, h - start_row)
            o = 0
            k = 0
            while k + 1 < len(payload) and o < nrows * w:
                cnt, val = payload[k], payload[k + 1]
                img[start_row * w + o: start_row * w + o + cnt] = \
                    bytes([val]) * cnt
                o += cnt
                k += 2
            assert o == nrows * w, f"blk{b} RLE out {o} != {nrows * w}"
            blk_off += blen
        entry = {"w": w, "h": h, "depth": depth, "palette": palette,
                 "indices": bytes(img)}
        prev = entry
        out.append(entry)
    return out


# ================= 放大 =================

def upscale(entry: dict, tw: int, th: int) -> dict:
    """最近邻放大(调色板索引空间, 颜色数不增)"""
    w, h = entry["w"], entry["h"]
    idx = entry["indices"]
    out = bytearray(tw * th)
    for y in range(th):
        sy = y * h // th
        row = sy * w
        orow = y * tw
        for x in range(tw):
            out[orow + x] = idx[row + x * w // tw]
    return {"w": tw, "h": th, "depth": 8, "palette": entry["palette"],
            "indices": bytes(out)}


# ================= 编码 =================

def rle_encode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        v = data[i]
        j = i
        while j < len(data) and data[j] == v and j - i < 255:
            j += 1
        out += bytes([j - i, v])
        i = j
    return bytes(out)


def encode_frame(f: dict) -> bytes:
    w, h, pal = f["w"], f["h"], f["palette"]
    blocks = max(1, (h + BLOCK_HEIGHT - 1) // BLOCK_HEIGHT)
    block_h = (h + blocks - 1) // blocks
    hdr = bytearray(b"_S" + b"\x00" * 7)   # ver 7B(设备读 depth@+9)
    hdr.append(8)
    hdr += struct.pack("<HHH", w, h, blocks)
    hdr += struct.pack("<H", block_h)
    idx = f["indices"]
    blens = []
    blocks_data = bytearray()
    for b in range(blocks):
        start = b * block_h * w
        nrows = min(block_h, h - b * block_h)
        rle = rle_encode(idx[start: start + nrows * w])
        blk = bytes([0]) + rle
        blens.append(len(blk))
        blocks_data += blk
    for bl in blens:
        hdr += struct.pack("<I", bl)
    for r, g, b in pal:
        hdr += bytes([b, g, r, 0])         # BGR+pad × 256
    return bytes(hdr) + bytes(blocks_data)


def encode_file(frames: list, out_path: str) -> int:
    """帧区 = 每帧 [0x5A5A][_S+palette+blocks](offset 指向 0x5A5A);
    checksum = data[16:16+total] 字节和(设备端 esp_eaf_format_calc_checksum)"""
    count = len(frames) + 1                # + _C 尾标记
    table = bytearray()
    region = bytearray()
    for f in frames:
        d = DATA_HEAD + encode_frame(f)
        table += struct.pack("<II", len(d), len(region))
        region += d
    meta = DATA_HEAD + b"_C" + b"\x00" * 6 + struct.pack("<I", 1) + \
        struct.pack("<I", 31) + \
        b"espcube 460 upscale\x00"
    table += struct.pack("<II", len(meta), len(region))
    region += meta
    total = len(table) + len(region)
    chk = sum(out for out in (table + region)) & 0xFFFFFFFF
    out = bytearray([0x89]) + b"EAF"
    out += struct.pack("<III", count, chk, total)
    out += table + region
    Path(out_path).write_bytes(bytes(out))
    return len(out)


# ================= CLI =================

def cmd_decode(argv):
    data = Path(argv[0]).read_bytes()
    fs = decode_file(data)
    print(f"frames={len(fs)}")
    f0 = fs[0]
    print(f"f0: {f0['w']}x{f0['h']} depth={f0['depth']}")
    if len(argv) > 1:
        from PIL import Image
        f = fs[0]
        img = Image.new("RGB", (f["w"], f["h"]))
        px = img.load()
        idx, pal = f["indices"], f["palette"]
        for y in range(f["h"]):
            for x in range(f["w"]):
                px[x, y] = pal[idx[y * f["w"] + x]]
        img.resize((f["w"] * 2, f["h"] * 2)).save(argv[1])
        print("png saved", argv[1])


def cmd_re460(argv):
    src, dst = argv[0], argv[1]
    fs = decode_file(Path(src).read_bytes())
    print(f"decoded {len(fs)} frames @ {fs[0]['w']}x{fs[0]['h']}")
    up = [upscale(f, 460, 460) for f in fs]
    n = encode_file(up, dst)
    print(f"encoded {len(up)} frames @460x460 → {dst} ({n} bytes, "
          f"{n // 1024} KB)")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "decode":
        cmd_decode(sys.argv[2:])
    elif len(sys.argv) >= 4 and sys.argv[1] == "re460":
        cmd_re460(sys.argv[2:])
    else:
        print(__doc__)
