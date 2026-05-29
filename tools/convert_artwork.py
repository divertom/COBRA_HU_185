#!/usr/bin/env python3
"""
Scan artwork/ for raster sources and emit LVGL 8-compatible .bin files under storage/.
Matches LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1 (see sdkconfig).

Binary layout: lv_img_header_t (4 LE bytes, non–big-endian LVGL layout), then pixels.
Opaque: LV_IMG_CF_TRUE_COLOR (cf=4), 2 bytes/pixel LE (lv_color.full).
Alpha: LV_IMG_CF_TRUE_COLOR_ALPHA (cf=5), 3 bytes/pixel — [rgb565_lo][rgb565_hi][opa].
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

from PIL import Image

# Matches components/lvgl__lvgl/src/draw/lv_img_buf.h enum order start
LV_IMG_CF_TRUE_COLOR = 4
LV_IMG_CF_TRUE_COLOR_ALPHA = 5

SRC_EXT = {".png", ".jpg", ".jpeg", ".bmp"}


def rgb888_to_lvgl16_swapped(r8: int, g8: int, b8: int) -> int:
    """Match lv_color_hex() for LV_COLOR_DEPTH==16 && LV_COLOR_16_SWAP!=0."""
    c = ((r8 & 0xFF) << 16) | ((g8 & 0xFF) << 8) | (b8 & 0xFF)
    full = (((c & 0xF80000) >> 16) | ((c & 0xFC00) >> 13) | ((c & 0x1C00) << 3) | ((c & 0xF8) << 5))
    return full & 0xFFFF


def pack_header(cf: int, w: int, h: int) -> bytes:
    """LVGL 8 lv_img_header_t, little-endian 32-bit (GCC ESP32 LE bitfield packing)."""
    if w <= 0 or h <= 0 or w > 2047 or h > 2047:
        raise ValueError(f"invalid dimensions {w}x{h}")
    packed = (
        (cf & 0x1F)
        | (0 << 5)
        | (0 << 8)
        | ((w & 0x7FF) << 10)
        | ((h & 0x7FF) << 21)
    )
    return struct.pack("<I", packed)


def img_may_have_alpha_channel(img: Image.Image) -> bool:
    mode = img.mode
    if mode in ("RGBA", "LA"):
        return True
    if mode == "P":
        if "transparency" in img.info:
            return True
    return False


def normalize_to_rgba(img: Image.Image) -> Image.Image:
    if img.mode == "RGBA":
        return img
    if img.mode == "RGB":
        return img.convert("RGBA")
    return img.convert("RGBA")


def normalize_to_rgb_opaque(img: Image.Image) -> Image.Image:
    if img.mode == "RGB":
        return img
    return img.convert("RGB")


def has_any_transparent_pixel(rgba: Image.Image) -> bool:
    """True if any pixel alpha < 255."""
    a = rgba.split()[-1]
    return a.getextrema() != (255, 255)


def convert_file(src: Path, dst: Path, force: bool) -> str:
    if dst.exists() and not force:
        if dst.stat().st_mtime >= src.stat().st_mtime:
            return "skip"

    img = Image.open(src)
    img.load()

    use_alpha = img_may_have_alpha_channel(img)
    if use_alpha:
        work = normalize_to_rgba(img)
        if not has_any_transparent_pixel(work):
            use_alpha = False
            work = normalize_to_rgb_opaque(work)
    else:
        work = normalize_to_rgb_opaque(img)

    if use_alpha:
        cf = LV_IMG_CF_TRUE_COLOR_ALPHA
        payload = pixel_payload_true_color_alpha(work)
        w, h = work.size
    else:
        cf = LV_IMG_CF_TRUE_COLOR
        payload = pixel_payload_true_color(work)
        w, h = work.size

    header = pack_header(cf, w, h)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(header + payload)
    return "converted"


def pixel_payload_true_color(rgb: Image.Image) -> bytes:
    out = bytearray()
    px = rgb.load()
    for y in range(rgb.height):
        for x in range(rgb.width):
            r, g, b = px[x, y][:3]
            v = rgb888_to_lvgl16_swapped(r, g, b)
            out.extend(struct.pack("<H", v))
    return bytes(out)


def pixel_payload_true_color_alpha(rgba: Image.Image) -> bytes:
    out = bytearray()
    px = rgba.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, a = px[x, y]
            v = rgb888_to_lvgl16_swapped(r, g, b)
            out.extend(struct.pack("<H", v))
            out.append(a & 0xFF)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert artwork raster images to LVGL 8 SPIFFS .bin assets.")
    ap.add_argument("--src", default="artwork", type=Path, help="Source tree (default: artwork)")
    ap.add_argument("--dst", default="storage", type=Path, help="Output tree mirror (default: storage)")
    ap.add_argument("--force", action="store_true", help="Reconvert even if destination is newer")
    args = ap.parse_args()

    root = args.src.resolve()
    dst_root = args.dst.resolve()
    if not root.is_dir():
        print(f"ERROR: source directory not found: {root}", file=sys.stderr)
        return 1

    converted: list[str] = []
    skipped: list[str] = []
    errs: list[str] = []

    for dirpath, _dirnames, filenames in os.walk(root):
        dp = Path(dirpath)
        for name in sorted(filenames):
            suf = Path(name).suffix.lower()
            if suf not in SRC_EXT:
                if suf == ".svg":
                    try:
                        rel = (dp / name).relative_to(root).as_posix()
                    except ValueError:
                        rel = str(dp / name)
                    print(f"skip (unsupported SVG): {rel}")
                continue
            src_file = dp / name
            rel = src_file.relative_to(root)
            dst_file = dst_root / rel.with_suffix(".bin")
            try:
                status = convert_file(src_file, dst_file, args.force)
                key = rel.with_suffix(".bin").as_posix()
                if status == "skip":
                    skipped.append(key)
                else:
                    converted.append(key)
            except OSError as exc:
                errs.append(f"{rel.as_posix()}: {exc}")
            except Exception as exc:  # noqa: BLE001
                errs.append(f"{rel.as_posix()}: {exc}")

    if errs:
        for e in errs:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    for c in sorted(converted):
        print(f"converted: {c}")
    for s in sorted(skipped):
        print(f"skipped:   {s}")
    print(f"Artwork conversion done — {len(converted)} converted, {len(skipped)} skipped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
