#!/usr/bin/env python3
"""EFC asset checks for the face pipeline.

--contract PATH [PATH ...]
    Pre-flash contract (task 26 subset). A directory checks every *.efc directly inside it.
    A file passes only if all of these hold:
      - magic is b"EFC1"
      - geometry is 460x460
      - frame count is in [1, 300]
      - file size is at most 262144 bytes
      - key ratio is above 0 (some pixels are transparent)
      - key ratio is below 99% (transparent pixels do not fill the frame)
    A missing path or a directory with no .efc files fails. A file that is not EFC is not
    decoded, so its key and visible ratios are reported as 0.
    Other checks (tile, scaling, MANIFEST, sha256 match, lit ratio) are added in task 30.3.

Usage (repo root):
    ~/.venvs/efc/bin/python tools/efc_verify.py --contract <path> [<path> ...] [--report FILE.json]
Exit codes: 0 all pass; 1 at least one failure; 2 usage error or encoder hash mismatch.
"""
import argparse
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import efc_build  # noqa: E402  (frozen encoder hashes, default codec path, decoder import)

MAGIC = b"EFC1"
SIDE = 460
MAX_BYTES = 262144
FRAME_RANGE = (1, 300)
KEY_CEILING = 0.99

_decoders = {}


def load_decoder(codec_dir):
    """Import the pinned decoder after its sha256 passes the same gate as the encoder."""
    if codec_dir not in _decoders:
        efc_build.check_encoder(codec_dir)
        efc_py, _ = efc_build.import_codec(codec_dir)
        _decoders[codec_dir] = efc_py
    return _decoders[codec_dir]


def new_record(path):
    return {
        "path": path,
        "ok": True,
        "reasons": [],
        "bytes": None,
        "magic": None,
        "width": None,
        "height": None,
        "frames": None,
        "decoded": False,
        "tile": None,
        "scaled": None,
        "zlib": None,
        "key_ratio_mean": 0.0,
        "key_ratio_frame0": 0.0,
        "visible_ratio_mean": 0.0,
    }


def check_file(path, codec_dir):
    rec = new_record(path)

    def fail(reason):
        rec["ok"] = False
        rec["reasons"].append(reason)

    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        fail(f"unreadable: {exc}")
        return rec
    rec["bytes"] = len(data)
    rec["magic"] = data[:4].hex()
    if len(data) > MAX_BYTES:
        fail(f"size {len(data)} bytes > {MAX_BYTES}")
    if data[:4] != MAGIC:
        fail(f"magic {data[:4].hex()} != {MAGIC.hex()} (EFC1)")
        return rec
    if len(data) < 20:
        fail("header truncated")
        return rec
    width, height, _, _, frames, _ = struct.unpack_from("<6H", data, 8)
    rec.update(width=width, height=height, frames=frames)
    if (width, height) != (SIDE, SIDE):
        fail(f"geometry {width}x{height} != {SIDE}x{SIDE}")
    if not FRAME_RANGE[0] <= frames <= FRAME_RANGE[1]:
        fail(f"frame count {frames} outside [{FRAME_RANGE[0]}, {FRAME_RANGE[1]}]")

    efc_py = load_decoder(codec_dir)
    try:
        pixels, info = efc_py.decode_frames(data)
    except Exception as exc:  # malformed streams raise ValueError, zlib.error or IndexError
        fail(f"decode error: {type(exc).__name__}: {exc}")
        return rec
    rec["decoded"] = True
    rec.update(tile=info["tile"], scaled=info["scaled"], zlib=info["compressed"])
    if info["color_key"] is None:
        key_mean = key0 = 0.0
    else:
        key = pixels == info["color_key"]
        key_mean = float(key.mean())
        key0 = float(key[0].mean())
    rec.update(key_ratio_mean=key_mean, key_ratio_frame0=key0, visible_ratio_mean=1.0 - key_mean)
    if key_mean == 0.0:
        fail("no transparent pixels (key ratio 0%)")
    if key0 >= KEY_CEILING or key_mean >= KEY_CEILING:
        fail(f"transparent pixels fill the frame (key ratio >= {KEY_CEILING:.0%})")
    return rec


def missing(path, reason):
    rec = new_record(path)
    rec["ok"] = False
    rec["reasons"].append(reason)
    return rec


def run_contract(paths, codec_dir):
    records = []
    for path in paths:
        if os.path.isdir(path):
            files = sorted(glob.glob(os.path.join(path, "*.efc")))
            if not files:
                records.append(missing(path, "directory has no .efc files"))
            records.extend(check_file(f, codec_dir) for f in files)
        elif os.path.isfile(path):
            records.append(check_file(path, codec_dir))
        else:
            records.append(missing(path, "path not found"))
    return records


def format_line(rec):
    status = "PASS" if rec["ok"] else "FAIL"
    geom = f"{rec['width']}x{rec['height']}" if rec["width"] is not None else "n/a"
    frames = rec["frames"] if rec["frames"] is not None else "n/a"
    size = rec["bytes"] if rec["bytes"] is not None else "n/a"
    magic = rec["magic"] if rec["magic"] is not None else "n/a"
    decoded = "" if rec["decoded"] else " (not decoded)"
    line = (f"{status} {rec['path']}: bytes={size} magic={magic} geometry={geom} frames={frames} "
            f"key={rec['key_ratio_mean'] * 100:.2f}% visible={rec['visible_ratio_mean'] * 100:.2f}%{decoded}")
    if rec["reasons"]:
        line += " | " + "; ".join(rec["reasons"])
    return line


def main(argv=None):
    ap = argparse.ArgumentParser(description="EFC asset checks for the face pipeline.")
    ap.add_argument("--contract", action="store_true", help="run the face asset contract")
    ap.add_argument("--codec-dir", default=efc_build.DEFAULT_CODEC_DIR,
                    help="pinned esp_efc_codec 1.1.0 pc/ directory used for decoding")
    ap.add_argument("--report", default=None, help="write the JSON results to this path")
    ap.add_argument("paths", nargs="*", help="EFC files or directories")
    a = ap.parse_args(argv)
    if not a.contract:
        ap.error("--contract is the only mode implemented")
    if not a.paths:
        ap.error("no paths given")

    records = run_contract(a.paths, a.codec_dir)
    for rec in records:
        print(format_line(rec))
    failed = sum(1 for r in records if not r["ok"])
    print(f"contract: {len(records)} checked, {len(records) - failed} pass, {failed} fail")
    if a.report:
        efc_build.write_json({"mode": "contract", "paths": a.paths, "env": efc_build.runtime_env(),
                              "results": records}, a.report)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
