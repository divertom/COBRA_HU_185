#!/usr/bin/env python3
"""Emit main/Page_Clock/cobra_wordmark_blk.c from storage/Logos/Cobra_text_blk_BG.png."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PNG = ROOT / "storage/Logos/Cobra_text_blk_BG.png"
OUT = ROOT / "main/Page_Clock/cobra_wordmark_blk.c"
WIDTH = 16


def main() -> None:
    data = PNG.read_bytes()
    magic = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    if data[:8] != magic:
        raise SystemExit(f"not a PNG: {PNG}")

    lines = []
    for i in range(0, len(data), WIDTH):
        chunk = data[i : i + WIDTH]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

    array_body = "\n".join(lines)
    text = f"""/**
 * COBRA wordmark PNG embedded for LVGL (LV_IMG_SRC_VARIABLE).
 * Source: storage/Logos/Cobra_text_blk_BG.png — regenerate: python tools/gen_cobra_wordmark_embed.py
 */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t cobra_wordmark_png_map[] = {{
{array_body}
}};

const lv_img_dsc_t cobra_wordmark_blk_dsc = {{
    .header.cf = 0,
    .header.always_zero = 0,
    .header.w = 0,
    .header.h = 0,
    .data_size = sizeof(cobra_wordmark_png_map),
    .data = cobra_wordmark_png_map,
}};
"""
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
