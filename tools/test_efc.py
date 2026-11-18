#!/usr/bin/env python3
"""EFC checks for the face asset pipeline: host_tests.sh suite [18], EFC part.

Run with the EFC encoder environment (Pillow 13.0.0, numpy 2.5.4):
    ~/.venvs/efc/bin/python tools/test_efc.py

Covers Property 8 (lossless round trip, keyed semantics, reproducible source) of
the EFC pipeline contract (原 .kiro spec design.md, 2026-11-18 随 .kiro 移除; 编码器现于 tools/efc/pc). The contract check
(tools/efc_verify.py --contract) is run by host_tests.sh, not here.

**Validates: Requirements 2.9, 2.13, 2.14**

Property-based checks use fixed seeds and CASES cases each. The committed assets are only read.
The rebuild writes to a temporary directory that is removed afterwards.
Exit code: 0 when every check passes, 1 otherwise.
"""
import glob
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True  # keep tools/ and the evidence codec directory free of .pyc files

import numpy as np  # noqa: E402
from PIL import Image, ImageSequence  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import efc_build  # noqa: E402

efc_build.check_encoder(efc_build.DEFAULT_CODEC_DIR)  # refuse before any codec code runs
efc_py, video_to_efc = efc_build.import_codec(efc_build.DEFAULT_CODEC_DIR)

CASES = 1000
EXPECTED_CHECKS = 11  # EFC-1 .. EFC-11; EFC-12 (contract) is run by host_tests.sh
SEED_ROUNDTRIP = 30301
SEED_TIMELINE = 30302
KEY = efc_build.KEY_565  # 0xF81F, the colour key
ASSET_DIR = os.path.dirname(efc_build.DEFAULT_MANIFEST)
RESULTS = []


def report(check_id, name, ok, detail):
    RESULTS.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} [EFC-{check_id}] {name}: {detail}", flush=True)


def load_manifest():
    with open(efc_build.DEFAULT_MANIFEST, encoding="utf-8") as fh:
        return json.load(fh)


def random_frames(rng, nrng):
    """One random case: RGB565 frames with a 4-aligned size and a random key mask.
    Non-key pixels never equal KEY, so the key mask is exactly the set of KEY pixels."""
    w = 4 * rng.randint(1, 12)
    h = 4 * rng.randint(1, 12)
    n = rng.randint(1, 3)
    r = rng.random()
    density = 0.0 if r < 0.1 else 1.0 if r < 0.2 else rng.random()
    key = nrng.random((n, h, w)) < density
    if rng.random() < 0.5:  # few colours: palette token path
        palette = nrng.integers(0, 0x10000, size=rng.randint(1, 255)).astype(np.uint16)
        vals = palette[nrng.integers(0, palette.size, size=(n, h, w))]
    else:  # many colours: full colour token path
        vals = nrng.integers(0, 0x10000, size=(n, h, w)).astype(np.uint16)
    vals = np.where(vals == KEY, np.uint16(KEY ^ 1), vals).astype(np.uint16)
    frames = np.where(key, np.uint16(KEY), vals).astype(np.uint16)
    return frames, key


def check_roundtrip_and_keyed():
    """EFC-1: decode(encode(X)) equals X element-wise. EFC-2: keyed alpha after decode."""
    rng = random.Random(SEED_ROUNDTRIP)
    nrng = np.random.default_rng(SEED_ROUNDTRIP)
    rt_fail, key_fail = [], []
    n_palette = n_full = n_keyed = 0
    for case in range(CASES):
        frames, key = random_frames(rng, nrng)
        n_keyed += bool(key.any())
        try:
            blob = efc_py.encode_frames(frames, tile=4, color_key=KEY if key.any() else None,
                                        compress=True, legacy=False)
            back, info = efc_py.decode_frames(blob)
        except Exception as exc:  # a codec error fails the case
            rt_fail.append((case, f"{type(exc).__name__}: {exc}"))
            key_fail.append(case)
            continue
        n_palette += info["token_width"] == 1
        n_full += info["token_width"] == 2
        if not (back.shape == frames.shape and np.array_equal(back, frames)
                and info["frame_count"] == frames.shape[0] and info["width"] == frames.shape[2]
                and info["height"] == frames.shape[1] and info["tile"] == 4):
            rt_fail.append((case, f"encoded {frames.shape}, decoded {back.shape}"))
            key_fail.append(case)
            continue
        alpha = np.where(back == KEY, 0, 0xFF)
        if not (np.all(alpha[key] == 0) and np.all(alpha[~key] == 0xFF) and np.all(back[key] == KEY)):
            key_fail.append(case)
    report(1, "round trip RGB565 (PBT, seed 30301)", not rt_fail,
           f"{CASES - len(rt_fail)}/{CASES} exact; palette {n_palette}, full colour {n_full}, "
           f"with key {n_keyed}" + (f"; first failure {rt_fail[0]}" if rt_fail else ""))
    report(2, "keyed semantics after decode (PBT, seed 30301)", not key_fail,
           f"{CASES - len(key_fail)}/{CASES} cases: key alpha 0, other alpha 0xFF, key colour 0xF81F"
           + (f"; first failure case {key_fail[0]}" if key_fail else ""))


def check_sample_plan():
    """EFC-3: the 100 ms sample plan on random GIF timelines (N = round(total/100), monotone index)."""
    step = efc_build.STEP_MS
    rng = random.Random(SEED_TIMELINE)
    bad = []
    for case in range(CASES):
        raw = [rng.choice([0, rng.randint(1, 1000)]) for _ in range(rng.randint(1, 60))]
        durs = efc_build.effective_durations(raw)
        n_out, plan, total = efc_build.sample_plan(durs)
        starts, t = [], 0
        for d in durs:
            starts.append(t)
            t += d
        ok = n_out == len(plan) == max(1, round(total / step))
        ok = ok and all(a <= b for a, b in zip(plan, plan[1:]))
        ok = ok and all(starts[i] <= step * k < starts[i] + durs[i] for k, i in enumerate(plan))
        if not ok:
            bad.append(case)
    report(3, "sample plan on random GIF timelines (PBT, seed 30302)", not bad,
           f"{CASES - len(bad)}/{CASES} cases: N = round(total/100), index monotone, start <= t < end"
           + (f"; first failure case {bad[0]}" if bad else ""))


def gif_facts(path):
    with Image.open(path) as im:
        raw = efc_build.frame_durations_raw(im)
        size = im.size
    total = sum(efc_build.effective_durations(raw))
    return {"raw": raw, "size": size, "total": total,
            "expect": max(1, round(total / efc_build.STEP_MS)), "sha256": efc_build.sha256_file(path)}


def reference_565(gif_path):
    """RGB565 frames and key mask rebuilt from a GIF the same way efc_build.build_clip makes them."""
    with Image.open(gif_path) as im:
        durs = efc_build.effective_durations(efc_build.frame_durations_raw(im))
        n_out, plan, _ = efc_build.sample_plan(durs)
        needed = {}
        for k, i in enumerate(plan):
            needed.setdefault(i, []).append(k)
        rgbs, alphas = [None] * n_out, [None] * n_out
        for i, fr in enumerate(ImageSequence.Iterator(im)):
            if i in needed:
                rgb, a8 = efc_build.to_460(np.asarray(fr.convert("RGBA")))
                for k in needed[i]:
                    rgbs[k], alphas[k] = rgb, a8
    frames = np.stack(rgbs)
    key = np.stack([a < efc_build.ALPHA_KEY_LIMIT for a in alphas])
    rgb565 = video_to_efc.to_565(video_to_efc.apply_color_key(frames, key if key.any() else None))
    return rgb565, key


def check_committed_assets(manifest):
    """EFC-4, 5, 7, 8, 9, 10: GIF sources, frame counts, committed .efc bytes, decode, reference, keys."""
    gifs = {efc_build.stem_of(os.path.basename(p)): p
            for p in glob.glob(os.path.join(efc_build.DEFAULT_GIF_DIR, "*.gif"))}
    clips = manifest["clips"]
    fails = {k: [] for k in ("source", "frames", "sha", "decode", "reference", "keyed")}
    if len(gifs) != efc_build.GIF_COUNT or sorted(gifs) != sorted(c["name"] for c in clips):
        fails["source"].append("GIF stems differ from MANIFEST clip names")
    for clip in clips:
        name = clip["name"]
        gif = gifs.get(name)
        if gif is None:
            fails["source"].append(f"{name}: GIF missing")
            continue
        facts = gif_facts(gif)
        if (facts["size"] != (efc_build.GIF_SIDE, efc_build.GIF_SIDE) or facts["sha256"] != clip["src_sha256"]
                or len(facts["raw"]) != clip["src_frames"] or facts["total"] != clip["src_total_ms"]):
            fails["source"].append(name)
        efc_path = os.path.join(ASSET_DIR, name + ".efc")
        if not os.path.isfile(efc_path):
            fails["sha"].append(f"{name}: missing")
            continue
        with open(efc_path, "rb") as fh:
            blob = fh.read()
        if hashlib.sha256(blob).hexdigest() != clip["efc_sha256"] or len(blob) != clip["efc_bytes"]:
            fails["sha"].append(name)
        try:
            frames, info = efc_py.decode_frames(blob)
        except Exception as exc:
            fails["decode"].append(f"{name}: {type(exc).__name__}: {exc}")
            continue
        n = frames.shape[0]
        if not (info["frame_count"] == n == clip["out_frames"] == facts["expect"]):
            fails["frames"].append(f"{name}: decoded {info['frame_count']}, MANIFEST {clip['out_frames']}, "
                                   f"round(total/100) {facts['expect']}")
        crcs = [f"{zlib.crc32(np.ascontiguousarray(frames[k], dtype='<u2').tobytes()):08x}" for k in range(n)]
        if not (info["width"] == info["height"] == efc_build.SIZE and info["tile"] == 4
                and info["compressed"] and crcs == clip["crc32_per_frame"]):
            fails["decode"].append(f"{name}: geometry, zlib, tile or per-frame CRC32 differs from MANIFEST")
        ref, ref_key = reference_565(gif)
        if ref.shape != frames.shape or not np.array_equal(ref, frames):
            fails["reference"].append(name)
            continue
        alpha = np.where(frames == KEY, 0, 0xFF)
        frame0 = round(1000.0 * float(np.count_nonzero(ref_key[0])) / ref_key[0].size, 2)
        if not (np.all(alpha[ref_key] == 0) and np.all(alpha[~ref_key] == 0xFF)
                and np.all(frames[ref_key] == KEY) and frame0 == clip["frame0_key_permille"]):
            fails["keyed"].append(name)
    total = len(clips)

    def detail(key):
        bad = fails[key]
        return f"{total - len(bad)}/{total} clips" + (f"; failing: {bad[:3]}" if bad else "")

    report(4, "GIF sources match MANIFEST (848x848, sha256, frames, total ms)",
           not fails["source"], detail("source"))
    report(5, "frame count: MANIFEST = decoded = round(GIF total/100)", not fails["frames"], detail("frames"))
    report(7, "committed .efc sha256 and size match MANIFEST", not fails["sha"], detail("sha"))
    report(8, "committed .efc decode: 460x460, zlib, tile 4, per-frame CRC32 = MANIFEST",
           not fails["decode"], detail("decode"))
    report(9, "committed .efc decode equals RGB565 reference rebuilt from GIF",
           not fails["reference"], detail("reference"))
    report(10, "committed .efc keyed: alpha 0 on key, 0xFF elsewhere, key 0xF81F, frame0 key permille = MANIFEST",
           not fails["keyed"], detail("keyed"))


def check_square_scaling():
    """EFC-6: 848x848 to 460x460 uses one scale factor on both axes."""
    side, out = efc_build.GIF_SIDE, efc_build.SIZE
    x0, y0, w, h = 200, 300, 64, 32  # non-square block: an axis-dependent factor would move the centroid
    src = np.zeros((side, side, 4), np.uint8)
    src[y0:y0 + h, x0:x0 + w] = 255
    rgb, alpha = efc_build.to_460(src)
    name = "848->460 square scaling (isotropic)"
    if rgb.shape != (out, out, 3) or alpha.shape != (out, out):
        report(6, name, False, f"output shapes {rgb.shape} and {alpha.shape}")
        return
    weight = alpha.astype(np.float64)
    centres = np.arange(out) + 0.5
    cx = float((weight.sum(axis=0) * centres).sum() / weight.sum())
    cy = float((weight.sum(axis=1) * centres).sum() / weight.sum())
    ex = (x0 + w / 2.0) * out / side
    ey = (y0 + h / 2.0) * out / side
    ok = abs(cx - ex) <= 0.5 and abs(cy - ey) <= 0.5
    report(6, name, ok, f"block centroid ({cx:.2f}, {cy:.2f}) px, expected ({ex:.2f}, {ey:.2f}) px, "
                        f"output {out}x{out}")


def check_rebuild(manifest):
    """EFC-11: rebuilding every clip with tools/efc_build.py gives the .efc sha256 recorded in MANIFEST."""
    name = "rebuild with efc_build.py: .efc sha256 = MANIFEST"
    tmp = tempfile.mkdtemp(prefix="efc_rebuild_")
    try:
        out_dir = os.path.join(tmp, "efc")
        cmd = [sys.executable, os.path.join(HERE, "efc_build.py"), "--out-dir", out_dir,
               "--manifest", os.path.join(tmp, "MANIFEST.json"), "--record", os.path.join(tmp, "FREEZE.json")]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900,
                                  env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        except subprocess.TimeoutExpired:
            report(11, name, False, "efc_build.py timed out after 900 s")
            return
        if proc.returncode != 0:
            report(11, name, False, f"efc_build.py exit {proc.returncode}: {proc.stderr.strip()[-300:]}")
            return
        clips = manifest["clips"]
        bad = []
        for clip in clips:
            path = os.path.join(out_dir, clip["name"] + ".efc")
            if not os.path.isfile(path) or efc_build.sha256_file(path) != clip["efc_sha256"]:
                bad.append(clip["name"])
        report(11, name, not bad, f"{len(clips) - len(bad)}/{len(clips)} clips"
                                  + (f"; differ: {bad[:3]}" if bad else ""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    manifest = load_manifest()
    check_roundtrip_and_keyed()       # EFC-1, EFC-2
    check_sample_plan()               # EFC-3
    check_committed_assets(manifest)  # EFC-4, 5, 7, 8, 9, 10
    check_square_scaling()            # EFC-6
    check_rebuild(manifest)           # EFC-11
    passed = sum(RESULTS)
    print(f"EFC python checks: {passed}/{len(RESULTS)} pass", flush=True)
    return 0 if len(RESULTS) == EXPECTED_CHECKS and passed == EXPECTED_CHECKS else 1


if __name__ == "__main__":
    sys.exit(main())
