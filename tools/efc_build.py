#!/usr/bin/env python3
"""Frozen E0 emote pipeline: Espressif Emote Pack GIF -> 460x460 keyed EFC.

Replaces tools/gif2efc.py (kept until task 30.4). Uses the official esp_efc_codec 1.1.0
PC encoder (pc/efc_py.py and pc/video_to_efc.py). The archive and both scripts are
sha256-checked at startup, and the run is refused on any mismatch.

Frozen steps per GIF:
  1. Sample the GIF timeline on a 100 ms grid (sample-and-hold).
  2. Premultiply RGB by alpha, LANCZOS-resize 848 -> 460, then un-premultiply.
  3. Key pixels whose resized alpha is below 128 to RGB565 0xF81F (official apply_color_key).
  4. efc_py.encode_frames(tile=4, color_key=0xF81F, compress=True, legacy=False).
  5. Decode the blob and require exact RGB565 equality with the packed input.
  6. Compute the clip facts for MANIFEST.json from the decoded frames (see clip_metrics).

A full run also writes MANIFEST.json (default: firmware/components/app_ui/ui/assets/face/).
Every MANIFEST value is computed in the run. None is typed in by hand.

Encoder environment: ~/.venvs/efc (Python 3.14.4 from /usr/bin/python3, Pillow 13.0.0, numpy 2.5.4).
Create it with: /usr/bin/python3 -m venv ~/.venvs/efc, then ~/.venvs/efc/bin/python -m pip install pillow==13.0.0 numpy==2.5.4

Usage (any cwd; Pillow and numpy required):
  ~/.venvs/efc/bin/python tools/efc_build.py [--out-dir DIR] [--record FILE] [--manifest FILE] [--archive FILE]
  ~/.venvs/efc/bin/python tools/efc_build.py --inventory-only [--record FILE]

Exit codes: 0 success; 2 refused (archive or encoder hash mismatch, wrong input set,
round-trip failure, MANIFEST total mismatch).
"""
import argparse
import bisect
import glob
import hashlib
import json
import os
import platform
import sys
import time
import zlib
from datetime import datetime, timezone

import numpy as np
from PIL import Image, ImageSequence

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SPEC_DIR = os.path.join(REPO, ".kiro", "specs", "freeze-and-face-blackscreen-fix")
SOURCE_DIR = os.path.join(SPEC_DIR, "evidence", "source")
DEFAULT_GIF_DIR = os.path.join(SOURCE_DIR, "EmotePack", "gif")
DEFAULT_CODEC_DIR = os.path.join(SOURCE_DIR, "esp_efc_codec_1.1.0", "pc")
DEFAULT_OUT_DIR = os.path.join(SOURCE_DIR, "efc_460")
ENCODER_VENV = os.path.expanduser("~/.venvs/efc")  # the only encoder environment (D-VENV)

# Frozen inputs and parameters. Changing any of these needs a new freeze record.
ARCHIVE_NAME = "espressif__esp_efc_codec-v1.1.0.zip"
ARCHIVE_SHA256 = "d50c10fbec464e4f52dd361abeb7dcbe4f0af9cfe42f281cb6aaa26ef8659316"
ENCODER_SHA256 = {
    "efc_py.py": "02a2114e75fd257035d5177b4dc0a661c03ea39427ba19c63aa1d057628bb61b",
    "video_to_efc.py": "1d9ebfae72fdffbe3ec8f9121acfebf27a233a5e5fd208c30e6e44c5e0c8a6be",
}
GIF_COUNT = 23
GIF_SIDE = 848
SIZE = 460
STEP_MS = 100
TILE = 4
COMPRESS = True
LEGACY = False
KEY_565 = 0xF81F
ALPHA_KEY_LIMIT = 128  # resized alpha < 128 is keyed

ENCODER_PACKAGE = "espressif/esp_efc_codec"
ENCODER_VERSION = "1.1.0"
DEFAULT_ARCHIVE = os.path.join(SOURCE_DIR, "esp_efc_codec_1.1.0", ARCHIVE_NAME)
DEFAULT_MANIFEST = os.path.join(REPO, "firmware", "components", "app_ui", "ui", "assets", "face", "MANIFEST.json")
RESAMPLE = "LANCZOS-premultiplied"
# EFC header of the pinned codec: byte 5 holds the flags, byte 6 the tile size.
HDR_FLAGS_OFFSET = 5
FLAG_ZLIB = 0x04  # efc_py._emubcrdl
FLAG_PRED = 0x08  # efc_py._qgbcnn
LIT_GRID = 32  # screen self-check sample grid (design Glossary, lit 占比)
LIT_LUMA_MIN = 128  # luma 0.299R+0.587G+0.114B at or above this counts as lit
# Test-embedded set and pool totals (design B7). A total that differs from these refuses the run.
EMBEDDED_CLIPS = ("smile_static", "laugh", "investigate", "shocked")
POOL_BYTES_EXPECTED = 206846
EMBEDDED_BYTES_EXPECTED = 29641


def refuse(message):
    print(f"REFUSED: {message}", file=sys.stderr)
    sys.exit(2)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_encoder(codec_dir):
    """Hash the encoder scripts against the frozen values. Refuse on any mismatch."""
    seen = {}
    problems = []
    for name, frozen in ENCODER_SHA256.items():
        path = os.path.join(codec_dir, name)
        if not os.path.isfile(path):
            problems.append(f"{name} not found in {codec_dir}")
            continue
        seen[name] = sha256_file(path)
        if seen[name] != frozen:
            problems.append(f"{name} sha256 {seen[name]} != frozen {frozen}")
    if problems:
        refuse("encoder check failed: " + "; ".join(problems))
    return seen


def check_archive(path):
    """Hash the esp_efc_codec archive against the frozen value. Refuse on any mismatch."""
    if not os.path.isfile(path):
        refuse(f"encoder archive not found: {path}")
    seen = sha256_file(path)
    if seen != ARCHIVE_SHA256:
        refuse(f"archive sha256 {seen} != frozen {ARCHIVE_SHA256}")
    return seen


def import_codec(codec_dir):
    """Import the encoder. Call check_encoder first so unverified code never runs."""
    sys.dont_write_bytecode = True  # keep the evidence codec directory unchanged
    if codec_dir not in sys.path:
        sys.path.insert(0, codec_dir)
    import efc_py
    import video_to_efc
    if getattr(efc_py, "_emubcrdl", None) != FLAG_ZLIB or getattr(efc_py, "_qgbcnn", None) != FLAG_PRED:
        refuse("codec header flags differ from FLAG_ZLIB / FLAG_PRED")
    return efc_py, video_to_efc


def frame_durations_raw(im):
    return [int(fr.info.get("duration", 0) or 0) for fr in ImageSequence.Iterator(im)]


def effective_durations(raw):
    # A 0 ms GIF frame is shown as 100 ms, the browser convention the earlier encode used.
    return [d if d > 0 else 100 for d in raw]


def sample_plan(durs, step=STEP_MS):
    """Map each 100 ms output slot to the GIF frame showing at that time (sample-and-hold)."""
    starts = [0]
    for d in durs[:-1]:
        starts.append(starts[-1] + d)
    total = sum(durs)
    n_out = max(1, round(total / step))
    plan = []
    for k in range(n_out):
        i = bisect.bisect_right(starts, k * step) - 1
        plan.append(min(max(i, 0), len(durs) - 1))
    return n_out, plan, total


def to_460(rgba):
    """Premultiplied LANCZOS resize of one RGBA frame to SIZE x SIZE, then un-premultiply."""
    a = rgba[..., 3].astype(np.float32) / 255.0
    prem = rgba[..., :3].astype(np.float32) * a[..., None]
    chans = [
        np.asarray(Image.fromarray(prem[..., c].copy()).resize((SIZE, SIZE), Image.Resampling.LANCZOS))
        for c in range(3)
    ]
    alpha = np.asarray(Image.fromarray(a.copy()).resize((SIZE, SIZE), Image.Resampling.LANCZOS))
    rgb = np.stack(chans, axis=-1) / np.maximum(alpha, 1e-6)[..., None]
    rgb = np.clip(np.rint(rgb), 0, 255).astype(np.uint8)
    alpha8 = np.clip(np.rint(alpha * 255.0), 0, 255).astype(np.uint8)
    return rgb, alpha8


def stem_of(name):
    base = os.path.splitext(name)[0]
    return base.split("_[")[0] if "_[" in base else base


def inventory_gif(path):
    with Image.open(path) as im:
        raw = frame_durations_raw(im)
        size = list(im.size)
    n_out, _, total = sample_plan(effective_durations(raw))
    return {
        "file": os.path.basename(path),
        "stem": stem_of(os.path.basename(path)),
        "sha256": sha256_file(path),
        "bytes": os.path.getsize(path),
        "size": size,
        "frames": len(raw),
        "raw_frame_ms": sorted(set(raw)),
        "total_ms": total,
        "grid_frames": n_out,
    }


def header_facts(blob):
    """Flags the MANIFEST records, read from the EFC header bytes."""
    flags = blob[HDR_FLAGS_OFFSET]
    return {"zlib": bool(flags & FLAG_ZLIB), "pred": bool(flags & FLAG_PRED), "tile": blob[6]}


def lit_sample_index():
    """Face-circle sample points: cell centres of a LIT_GRID x LIT_GRID grid over the SIZE square,
    kept when inside the inscribed circle (centre SIZE/2, radius SIZE/2). Returns (rows, cols)."""
    centres = (np.arange(LIT_GRID) + 0.5) * SIZE / LIT_GRID
    xs, ys = np.meshgrid(centres, centres)
    r = SIZE / 2.0
    inside = (xs - r) ** 2 + (ys - r) ** 2 <= r * r
    return ys[inside].astype(np.intp), xs[inside].astype(np.intp)


def rgb565_luma(px):
    """0.299R + 0.587G + 0.114B, with 8-bit channels expanded from RGB565 by bit replication."""
    px = px.astype(np.uint32)
    r5, g6, b5 = (px >> 11) & 31, (px >> 5) & 63, px & 31
    r = (r5 << 3) | (r5 >> 2)
    g = (g6 << 2) | (g6 >> 4)
    b = (b5 << 3) | (b5 >> 2)
    return 0.299 * r + 0.587 * g + 0.114 * b


def clip_metrics(frames, key, stem):
    """Clip facts from the decoded RGB565 frames (N x H x W) and the input key mask (N x H x W bool).

    frame0_key_permille   keyed pixels / all pixels in frame 0, x1000. The board checks alpha0 against it.
    frame0_lit_share_pct  lit_share_pct of frame 0, x100. The board checks the screen against it.
    key_share_permille    keyed pixels / all pixels over all frames, x1000. Information only.
    changed_tile_pct      for each frame k, the share of TILE x TILE tiles that differ from frame k+1
                          (the last frame wraps to the first). Averaged over frames, x100.
    lit_share_pct         Glossary lit share: the share of face-circle sample points with
                          luma >= LIT_LUMA_MIN. Averaged over frames, x100. Information only.
                          A keyed point holds KEY_565 (luma about 105), so it is never lit.
    centre_offset_px      vertical offset of the bounding box of pixels visible in any frame, from SIZE/2,
                          positive down. Recorded only, not a failure condition.
    crc32_per_frame       IEEE 802.3 CRC of each frame's colour plane (w*h little-endian u16), 8 hex digits.
    """
    n = frames.shape[0]
    keyed = frames == KEY_565
    if not np.array_equal(keyed, key):
        refuse(f"{stem}: decoded keyed pixels differ from the input key mask")
    visible = ~keyed
    union = visible.any(axis=0)
    if not union.any():
        refuse(f"{stem}: no visible pixel in any frame")
    rows = np.flatnonzero(union.any(axis=1))
    y0, y1 = int(rows[0]), int(rows[-1]) + 1
    tile_fracs = []
    for k in range(n):
        changed = frames[k] != frames[(k + 1) % n]
        tiles = changed.reshape(SIZE // TILE, TILE, SIZE // TILE, TILE).any(axis=(1, 3))
        tile_fracs.append(float(np.count_nonzero(tiles)) / tiles.size)
    sy, sx = lit_sample_index()
    lit_fracs = []
    for k in range(n):
        lit = rgb565_luma(frames[k][sy, sx]) >= LIT_LUMA_MIN
        lit_fracs.append(float(np.count_nonzero(lit)) / lit.size)
    return {
        "crc32_per_frame": [f"{zlib.crc32(np.ascontiguousarray(frames[k], dtype='<u2').tobytes()):08x}"
                            for k in range(n)],
        "frame0_key_permille": round(1000.0 * float(np.count_nonzero(keyed[0])) / keyed[0].size, 2),
        "frame0_lit_share_pct": round(100.0 * lit_fracs[0], 2),
        "key_share_permille": round(1000.0 * float(np.count_nonzero(keyed)) / keyed.size, 2),
        "changed_tile_pct": round(100.0 * sum(tile_fracs) / n, 2),
        "lit_share_pct": round(100.0 * sum(lit_fracs) / n, 2),
        "centre_offset_px": round((y0 + y1) / 2.0 - SIZE / 2.0, 2),
    }


def build_clip(gif_path, out_dir, efc_py, video_to_efc):
    stem = stem_of(os.path.basename(gif_path))
    t0 = time.perf_counter()
    with Image.open(gif_path) as im:
        durs = effective_durations(frame_durations_raw(im))
        n_out, plan, _ = sample_plan(durs)
        needed = {}
        for k, i in enumerate(plan):
            needed.setdefault(i, []).append(k)
        rgbs = [None] * n_out
        alphas = [None] * n_out
        for i, fr in enumerate(ImageSequence.Iterator(im)):
            if i in needed:
                rgb, a8 = to_460(np.asarray(fr.convert("RGBA")))
                for k in needed[i]:
                    rgbs[k], alphas[k] = rgb, a8
    frames = np.stack(rgbs)
    key = np.stack([a < ALPHA_KEY_LIMIT for a in alphas])
    key_arg = key if key.any() else None
    rgb565 = video_to_efc.to_565(video_to_efc.apply_color_key(frames, key_arg))
    blob = efc_py.encode_frames(
        rgb565,
        tile=TILE,
        color_key=KEY_565 if key_arg is not None else None,
        compress=COMPRESS,
        legacy=LEGACY,
    )
    back, info = efc_py.decode_frames(blob)
    if back.shape != rgb565.shape or not np.array_equal(back, rgb565):
        refuse(f"{stem}: EFC does not decode back to the packed RGB565 frames")
    if info["frame_count"] != n_out or info["width"] != SIZE or info["height"] != SIZE:
        refuse(f"{stem}: EFC header {info['width']}x{info['height']} x{info['frame_count']} "
               f"!= expected {SIZE}x{SIZE} x{n_out}")
    hdr = header_facts(blob)
    if hdr["zlib"] != bool(info["compressed"]) or hdr["tile"] != TILE:
        refuse(f"{stem}: header flags disagree with the decoder")
    metrics = clip_metrics(back, key, stem)  # computed before the file is written
    out_path = os.path.join(out_dir, stem + ".efc")
    with open(out_path, "wb") as fh:
        fh.write(blob)
    return {
        "stem": stem,
        "file": os.path.basename(out_path),
        "source": os.path.basename(gif_path),
        "sha256": hashlib.sha256(blob).hexdigest(),
        "bytes": len(blob),
        "width": info["width"],
        "height": info["height"],
        "frames": info["frame_count"],
        "tile": info["tile"],
        "zlib": bool(info["compressed"]),
        "token_width": info["token_width"],
        "palette_count": info["palette_count"],
        "color_key": info["color_key"],
        "key_ratio_mean": round(float(key.mean()), 6),
        "key_ratio_frame0": round(float(key[0].mean()), 6),
        "pred": hdr["pred"],
        **metrics,
        "encode_s": round(time.perf_counter() - t0, 3),
    }


def runtime_env():
    import PIL
    return {
        "venv": ENCODER_VENV,
        "venv_match": os.path.realpath(sys.prefix) == os.path.realpath(ENCODER_VENV),
        "python": sys.version.split()[0],
        "python_executable": sys.executable,
        "pillow": PIL.__version__,
        "numpy": np.__version__,
        "platform": platform.platform(),
    }


def write_json(obj, path):
    text = json.dumps(obj, indent=2, ensure_ascii=False) + "\n"
    if path is None:
        sys.stdout.write(text)
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def build_manifest(inventory, outputs, encoder_sha, archive_sha, env):
    """MANIFEST.json content. Every value comes from the outputs of this run."""
    if len(outputs) != GIF_COUNT:
        refuse(f"{len(outputs)} clips built, expected {GIF_COUNT}")
    for field in ("width", "height", "tile", "zlib", "pred"):
        values = {o[field] for o in outputs}
        if len(values) != 1:
            refuse(f"clips disagree on {field}: {sorted(values)}")
    first = outputs[0]
    if first["width"] != SIZE or first["height"] != SIZE or first["tile"] != TILE:
        refuse(f"header geometry {first['width']}x{first['height']} tile {first['tile']} != frozen params")
    by_file = {e["file"]: e for e in inventory}
    clips = []
    for o in sorted(outputs, key=lambda item: item["stem"]):
        src = by_file[o["source"]]
        clips.append({
            "name": o["stem"],
            "src_sha256": src["sha256"],
            "src_frames": src["frames"],
            "src_total_ms": src["total_ms"],
            "out_frames": o["frames"],
            "delay_ms": STEP_MS,
            "efc_bytes": o["bytes"],
            "efc_sha256": o["sha256"],
            "crc32_per_frame": o["crc32_per_frame"],
            "frame0_key_permille": o["frame0_key_permille"],
            "frame0_lit_share_pct": o["frame0_lit_share_pct"],
            "key_share_permille": o["key_share_permille"],
            "changed_tile_pct": o["changed_tile_pct"],
            "lit_share_pct": o["lit_share_pct"],
            "centre_offset_px": o["centre_offset_px"],
            "embedded": o["stem"] in EMBEDDED_CLIPS,
        })
    names = [c["name"] for c in clips]
    if len(set(names)) != len(names):
        refuse("duplicate clip names")
    embedded = [c for c in clips if c["embedded"]]
    if sorted(c["name"] for c in embedded) != sorted(EMBEDDED_CLIPS):
        refuse(f"embedded clips {sorted(c['name'] for c in embedded)} != {sorted(EMBEDDED_CLIPS)}")
    pool_bytes = sum(c["efc_bytes"] for c in clips)
    embedded_bytes = sum(c["efc_bytes"] for c in embedded)
    if pool_bytes != POOL_BYTES_EXPECTED or embedded_bytes != EMBEDDED_BYTES_EXPECTED:
        refuse(f"totals pool {pool_bytes} / embedded {embedded_bytes} != design "
               f"{POOL_BYTES_EXPECTED} / {EMBEDDED_BYTES_EXPECTED}")
    return {
        "encoder": {
            "package": ENCODER_PACKAGE,
            "version": ENCODER_VERSION,
            "archive_sha256": archive_sha,
            "efc_py_sha256": encoder_sha["efc_py.py"],
            "video_to_efc_sha256": encoder_sha["video_to_efc.py"],
        },
        "params": {
            "size": first["width"],
            "step_ms": STEP_MS,
            "tile": first["tile"],
            "zlib": first["zlib"],
            "pred": first["pred"],
            "resample": RESAMPLE,
            "key": f"0x{KEY_565:04X}",
            "key_threshold": ALPHA_KEY_LIMIT,
        },
        "env": {"python": env["python"], "pillow": env["pillow"], "numpy": env["numpy"]},
        "clips": clips,
        "totals": {
            "pool_clips": len(clips),
            "pool_bytes": pool_bytes,
            "embedded_clips": len(embedded),
            "embedded_bytes": embedded_bytes,
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build frozen 460x460 keyed EFC clips from Emote Pack GIFs.")
    ap.add_argument("--gif-dir", default=DEFAULT_GIF_DIR)
    ap.add_argument("--codec-dir", default=DEFAULT_CODEC_DIR)
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    ap.add_argument("--record", default=None, help="freeze record path (default: OUT_DIR/FREEZE.json)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST, help="MANIFEST.json path (full runs only)")
    ap.add_argument("--archive", default=DEFAULT_ARCHIVE, help="esp_efc_codec archive (sha256-checked)")
    ap.add_argument("--inventory-only", action="store_true", help="hash and inventory the GIFs, then stop")
    a = ap.parse_args(argv)

    encoder_sha = check_encoder(a.codec_dir)  # first step: refuse before any encoder code runs
    archive_sha = check_archive(a.archive)

    gifs = sorted(glob.glob(os.path.join(a.gif_dir, "*.gif")))
    if len(gifs) != GIF_COUNT:
        refuse(f"expected {GIF_COUNT} GIFs in {a.gif_dir}, found {len(gifs)}")
    inventory = [inventory_gif(g) for g in gifs]
    wrong = [e["file"] for e in inventory if e["size"] != [GIF_SIDE, GIF_SIDE]]
    if wrong:
        refuse(f"GIFs not {GIF_SIDE}x{GIF_SIDE}: {wrong}")

    record = {
        "schema": "efc-build-record-1",
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "argv": sys.argv,
        "tool_sha256": sha256_file(os.path.abspath(__file__)),
        "env": runtime_env(),
        "encoder": {
            "package": ENCODER_PACKAGE,
            "version": ENCODER_VERSION,
            "archive": ARCHIVE_NAME,
            "archive_sha256": archive_sha,
            "scripts_sha256": encoder_sha,
        },
        "params": {
            "size": SIZE,
            "grid_ms": STEP_MS,
            "sampling": "sample-and-hold on the GIF timeline",
            "resize": "premultiplied LANCZOS, then un-premultiply",
            "key_565": f"0x{KEY_565:04X}",
            "key_rule": f"resized alpha < {ALPHA_KEY_LIMIT}",
            "encode_call": "efc_py.encode_frames(tile=4, color_key=0xF81F, compress=True, legacy=False, hold=False)",
            "tile": TILE,
            "compress": COMPRESS,
            "legacy": LEGACY,
            "gif_side": GIF_SIDE,
            "gif_count": GIF_COUNT,
        },
        "inputs": inventory,
        "outputs": [],
    }
    if a.inventory_only:
        write_json(record, a.record)
        return 0

    efc_py, video_to_efc = import_codec(a.codec_dir)
    os.makedirs(a.out_dir, exist_ok=True)
    for gif in gifs:
        out = build_clip(gif, a.out_dir, efc_py, video_to_efc)
        print(f"{out['file']:<18} frames={out['frames']:>3} bytes={out['bytes']:>7} "
              f"key={out['key_ratio_mean'] * 100:6.2f}% sha256={out['sha256'][:12]} {out['encode_s']}s",
              flush=True)
        record["outputs"].append(out)
    manifest = build_manifest(inventory, record["outputs"], encoder_sha, archive_sha, record["env"])
    write_json(manifest, a.manifest)
    record["manifest"] = {"path": a.manifest, "sha256": sha256_file(a.manifest)}
    write_json(record, a.record or os.path.join(a.out_dir, "FREEZE.json"))
    totals = manifest["totals"]
    print(f"MANIFEST {a.manifest} sha256={record['manifest']['sha256']} clips={totals['pool_clips']} "
          f"embedded={totals['embedded_clips']} pool_bytes={totals['pool_bytes']} "
          f"embedded_bytes={totals['embedded_bytes']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
