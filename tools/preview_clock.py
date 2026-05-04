#!/usr/bin/env python3
"""
Page_Clock layout preview.

Renders an approximation of main/Page_Clock/Page_Clock.c to a PNG so you can
iterate on the layout (font sizes, paddings, alignments) without flashing.

The `#defines` in "Mirrored from firmware" must match `main/Page_Clock/Page_Clock.c`
and `main/gauge_bg_lvgl.h`.

**Layout:** Read `line_height` from `main/Fonts/font_ddin_*.c` at runtime (see
`load_lvgl_line_heights()`). Bitmap `line_height` ≠ PIL ink bbox; the former
must drive strip / date Y math.

**Date overlap:** Firmware uses a transparent `date_row` over `time_strip`, so
the bottom white rule shows through gaps between captions and values. For a
clean PNG, **`PREVIEW_DATE_OVERLAP_PX`** defaults to **0** so the date block
starts **below** the strip bottom edge (preview-only; does not change firmware).

Usage (from the project root):

    python tools/preview_clock.py                       # 12:00 AM SUN JAN 00
    python tools/preview_clock.py --time 10:38 --ampm AM --day WED --date "MAY 28"
    python tools/preview_clock.py --24h --time 23:59    # second clock subpage (no AM/PM)
    python tools/preview_clock.py --out build/clock_alt.png

Requires: Pillow (`pip install Pillow`).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, Tuple

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover
    sys.stderr.write("Pillow is required. Install with: pip install Pillow\n")
    raise SystemExit(1) from exc


# --- Mirrored from main/gauge_bg_lvgl.h + main/Page_Clock/Page_Clock.c --------

# main/gauge_bg_lvgl.h
GAUGE_PIXEL_SIZE = 360
GAUGE_RING_RADIUS = 170
GAUGE_TICK_LENGTH = 14

# Tick width / ring stroke (gauge_bg_lvgl.c)
TICK_W = 4
RING_STROKE = 3

# main/Page_Clock/Page_Clock.c — keep in lockstep with the #defines there
CLOCK_RULE_THICK = 2
CLOCK_TIME_BAND_PAD_ROW = 15  # pad between rule / time_row / rule inside time_strip
CLOCK_TIME_PAD_COLUMN = 8
CLOCK_RULE_VISIBLE_PCT = 90  # rule_visible_w = rule_w * pct // 100
CLOCK_DATE_GAP = 3
CLOCK_DATE_CAPTION_VALUE_GAP_PX = 10
CLOCK_ABS_TIME_ROW_CY_PX = 160
CLOCK_ABS_DATE_OVERLAP_PX = 25
CLOCK_TIME_LETTER_SPACE = 5
CLOCK_AMPM_LETTER_SPACE = 0
# CLOCK_DATE_VALUE_LETTER_SPACE = 0  # add if non-zero in C (PIL: hard to match)

# Bottom credit (Page_Clock.c)
CLOCK_CREDIT_LINE_SPACE_PX = 3
CLOCK_CREDIT_GAP_ABOVE_TICK = 8
CLOCK_CREDIT_TICK_TOP_Y = GAUGE_PIXEL_SIZE // 2 + GAUGE_RING_RADIUS - GAUGE_TICK_LENGTH

# main/Fonts (lv_font_conv)
FONT_SIZE_TIME = 115
FONT_SIZE_AMPM = 32
FONT_SIZE_VALUE = 44
FONT_SIZE_CAPTION = 20
FONT_SIZE_CREDIT = 15

# Preview-only vertical placement for date_row top (see module docstring).
PREVIEW_DATE_OVERLAP_PX = 0

_LVGL_HEIGHT_CACHE: Dict[str, int] | None = None


def load_lvgl_line_heights(repo_root: Path) -> Dict[str, int]:
    """Parse `.line_height = N` from generated LVGL font sources."""
    global _LVGL_HEIGHT_CACHE
    if _LVGL_HEIGHT_CACHE is not None:
        return _LVGL_HEIGHT_CACHE
    rx = re.compile(r"\.line_height\s*=\s*(\d+)")
    fonts_dir = repo_root / "main" / "Fonts"
    mapping = {
        "font_ddin_115.c": "time",
        "font_ddin_reg_32.c": "ampm",
        "font_ddin_reg_20.c": "caption",
        "font_ddin_reg_44.c": "value",
    }
    out: Dict[str, int] = {}
    for fname, key in mapping.items():
        path = fonts_dir / fname
        if not path.is_file():
            raise SystemExit(f"Missing font source for metrics: {path}")
        m = rx.search(path.read_text(encoding="utf-8", errors="replace"))
        if not m:
            raise SystemExit(f"No line_height in {path}")
        out[key] = int(m.group(1))
    _LVGL_HEIGHT_CACHE = out
    return out


def time_row_height_lvgl(heights: Dict[str, int]) -> int:
    """LVGL time_row flex row height (max of time + AM/PM label line heights)."""
    return max(heights["time"], heights["ampm"])


# --- Paths -----------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
TTF_BOLD = REPO_ROOT / "tools" / "fonts" / "D-DINCondensed-Bold.ttf"
TTF_REGULAR = REPO_ROOT / "tools" / "fonts" / "D-DINCondensed-Regular.ttf"
DEFAULT_OUT = REPO_ROOT / "build" / "preview_clock.png"

_FONT_HELP = (
    "Place D-DINCondensed-Bold.ttf and D-DINCondensed-Regular.ttf in tools/fonts/ "
    "(SIL OFL, e.g. https://github.com/amcchord/datto-d-din)."
)


# --- Helpers ---------------------------------------------------------------

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)


def load_font_bold(size: int) -> ImageFont.FreeTypeFont:
    if not TTF_BOLD.exists():
        raise SystemExit(f"Missing font: {TTF_BOLD}\n{_FONT_HELP}")
    return ImageFont.truetype(str(TTF_BOLD), size)


def load_font_regular(size: int) -> ImageFont.FreeTypeFont:
    if not TTF_REGULAR.exists():
        raise SystemExit(f"Missing font: {TTF_REGULAR}\n{_FONT_HELP}")
    return ImageFont.truetype(str(TTF_REGULAR), size)


def text_size(font: ImageFont.FreeTypeFont, text: str) -> Tuple[int, int, int, int]:
    """Return (left, top, right, bottom) ink bbox for drawing at origin."""
    return font.getbbox(text)


def text_width_letter_spaced(
    font: ImageFont.FreeTypeFont, text: str, letter_space: int
) -> int:
    """Approximate lv_txt_get_size width with letter spacing (pair gaps)."""
    if not text:
        return 0
    w = 0
    for i, ch in enumerate(text):
        bb = font.getbbox(ch)
        w += bb[2] - bb[0]
        if i + 1 < len(text):
            w += letter_space
    return w


def max_time_row_rule_width() -> int:
    """Match clock_max_time_row_width() in Page_Clock.c."""
    font_time = load_font_bold(FONT_SIZE_TIME)
    font_ampm = load_font_regular(FONT_SIZE_AMPM)
    ls_t = CLOCK_TIME_LETTER_SPACE
    ls_a = CLOCK_AMPM_LETTER_SPACE
    w12 = text_width_letter_spaced(font_time, "12:00", ls_t)
    w00 = text_width_letter_spaced(font_time, "00:00", ls_t)
    w_am = text_width_letter_spaced(font_ampm, "AM", ls_a)
    w_pm = text_width_letter_spaced(font_ampm, "PM", ls_a)
    return max(w12, w00) + CLOCK_TIME_PAD_COLUMN + max(w_am, w_pm)


def max_time_row_rule_width_24h() -> int:
    """Match clock_max_time_row_width_24h() in Page_Clock.c (time only, no AM/PM)."""
    font_time = load_font_bold(FONT_SIZE_TIME)
    ls_t = CLOCK_TIME_LETTER_SPACE
    w2359 = text_width_letter_spaced(font_time, "23:59", ls_t)
    w0959 = text_width_letter_spaced(font_time, "09:59", ls_t)
    w0000 = text_width_letter_spaced(font_time, "00:00", ls_t)
    return max(w2359, w0959, w0000)


def draw_gauge_background(img: Image.Image) -> None:
    """White ring + 4 cardinal ticks on black, mirroring gauge_bg_lvgl.c."""
    cx = GAUGE_PIXEL_SIZE // 2
    cy = GAUGE_PIXEL_SIZE // 2
    r = GAUGE_RING_RADIUS
    d = ImageDraw.Draw(img)

    d.ellipse(
        [cx - r, cy - r, cx + r, cy + r],
        outline=WHITE,
        width=RING_STROKE,
    )

    tlen = GAUGE_TICK_LENGTH
    tw = TICK_W
    half_tw = tw // 2
    d.rectangle([cx - half_tw, cy - r, cx - half_tw + tw, cy - r + tlen], fill=WHITE)
    d.rectangle([cx + (r - tlen), cy - half_tw, cx + r, cy - half_tw + tw], fill=WHITE)
    d.rectangle([cx - half_tw, cy + (r - tlen), cx - half_tw + tw, cy + r], fill=WHITE)
    d.rectangle([cx - r, cy - half_tw, cx - r + tlen, cy - half_tw + tw], fill=WHITE)


def draw_text_letter_spaced(
    d: ImageDraw.ImageDraw,
    x_left: int,
    y_bottom_ink: int,
    text: str,
    font: ImageFont.FreeTypeFont,
    fill: Tuple[int, ...],
    letter_space: int,
) -> None:
    """Draw text with per-gap letter spacing; bottom-align each glyph ink to y_bottom_ink."""
    pen_x = x_left
    for i, ch in enumerate(text):
        bb = font.getbbox(ch)
        y_top = y_bottom_ink - bb[3]
        d.text((pen_x, y_top), ch, font=font, fill=fill)
        pen_x += bb[2] - bb[0] + (letter_space if i + 1 < len(text) else 0)


def render_time_band(
    canvas: Image.Image,
    time_text: str,
    ampm_text: str,
    rule_w: int,
    strip_x: int,
    strip_y: int,
    heights: Dict[str, int],
    use_24h: bool = False,
) -> None:
    """Draw time_strip: rules + HH:MM [+ AM/PM] (mirrors flex layout + positions)."""
    d = ImageDraw.Draw(canvas)
    font_time = load_font_bold(FONT_SIZE_TIME)
    font_ampm = load_font_regular(FONT_SIZE_AMPM)

    time_w_ls = text_width_letter_spaced(
        font_time, time_text, CLOCK_TIME_LETTER_SPACE
    )

    if use_24h:
        ampm_w = 0
        tr_h = heights["time"]
    else:
        bbox_ampm = text_size(font_ampm, ampm_text)
        ampm_w = bbox_ampm[2] - bbox_ampm[0]
        tr_h = time_row_height_lvgl(heights)
    tr_y = CLOCK_RULE_THICK + CLOCK_TIME_BAND_PAD_ROW
    strip_h = tr_h + 2 * CLOCK_RULE_THICK + 2 * CLOCK_TIME_BAND_PAD_ROW

    rule_visible_w = rule_w * CLOCK_RULE_VISIBLE_PCT // 100
    cx = GAUGE_PIXEL_SIZE // 2

    # Rules (centered; strip width = rule_w at strip_x)
    rule_top_y = strip_y
    rule_bot_y = strip_y + strip_h - CLOCK_RULE_THICK
    d.rectangle(
        [
            cx - rule_visible_w // 2,
            rule_top_y,
            cx + rule_visible_w // 2,
            rule_top_y + CLOCK_RULE_THICK,
        ],
        fill=WHITE,
    )
    d.rectangle(
        [
            cx - rule_visible_w // 2,
            rule_bot_y,
            cx + rule_visible_w // 2,
            rule_bot_y + CLOCK_RULE_THICK,
        ],
        fill=WHITE,
    )

    row_w = time_w_ls if use_24h else time_w_ls + CLOCK_TIME_PAD_COLUMN + ampm_w
    row_left = strip_x + (rule_w - row_w) // 2

    time_row_bottom = strip_y + tr_y + tr_h
    time_x = row_left
    draw_text_letter_spaced(
        d,
        time_x,
        time_row_bottom,
        time_text,
        font_time,
        WHITE,
        CLOCK_TIME_LETTER_SPACE,
    )

    if not use_24h:
        bbox_ampm = text_size(font_ampm, ampm_text)
        ampm_x = row_left + time_w_ls + CLOCK_TIME_PAD_COLUMN - bbox_ampm[0]
        ampm_y = time_row_bottom - bbox_ampm[3]
        d.text((ampm_x, ampm_y), ampm_text, font=font_ampm, fill=WHITE)


def render_date_columns(
    canvas: Image.Image,
    day_text: str,
    date_text: str,
    row_top: int,
    heights: Dict[str, int],
) -> None:
    """DAY + weekday · DATE + month-day (caption_column + date_row top = row_top)."""
    d = ImageDraw.Draw(canvas)
    font_caption = load_font_regular(FONT_SIZE_CAPTION)
    font_value = load_font_regular(FONT_SIZE_VALUE)

    def column_metrics(caption: str, value: str) -> Tuple[int, int, Tuple, Tuple]:
        bb_c = text_size(font_caption, caption)
        bb_v = text_size(font_value, value)
        col_w = max(bb_c[2] - bb_c[0], bb_v[2] - bb_v[0])
        col_h = (
            heights["caption"]
            + CLOCK_DATE_CAPTION_VALUE_GAP_PX
            + heights["value"]
        )
        return col_w, col_h, bb_c, bb_v

    day_w, _, day_bb_c, day_bb_v = column_metrics("DAY", day_text)
    dat_w, _, dat_bb_c, dat_bb_v = column_metrics("DATE", date_text)

    row_w = day_w + CLOCK_DATE_GAP + dat_w
    cx = GAUGE_PIXEL_SIZE // 2
    row_left = cx - row_w // 2

    cap_top = row_top

    def draw_column(left_x: int, col_w: int, caption: str, value: str,
                    bb_c: Tuple, bb_v: Tuple) -> None:
        cap_w = bb_c[2] - bb_c[0]
        val_w = bb_v[2] - bb_v[0]
        cap_x = left_x + (col_w - cap_w) // 2 - bb_c[0]
        cap_y = cap_top - bb_c[1]
        d.text((cap_x, cap_y), caption, font=font_caption, fill=WHITE)

        val_x = left_x + (col_w - val_w) // 2 - bb_v[0]
        # Match LVGL caption label line box + pad_row + value (not PIL ink heights).
        val_y = (
            cap_top
            + heights["caption"]
            + CLOCK_DATE_CAPTION_VALUE_GAP_PX
            - bb_v[1]
        )
        d.text((val_x, val_y), value, font=font_value, fill=WHITE)

    draw_column(row_left, day_w, "DAY", day_text, day_bb_c, day_bb_v)
    draw_column(row_left + day_w + CLOCK_DATE_GAP, dat_w, "DATE", date_text, dat_bb_c, dat_bb_v)


def render_credit(canvas: Image.Image) -> None:
    """STEIN CLOCK DESIGN / 2026 above lower tick (matches Page_Clock credit_lbl)."""
    d = ImageDraw.Draw(canvas)
    font = load_font_regular(FONT_SIZE_CREDIT)
    text = "STEIN CLOCK DESIGN\n2026"
    spacing = CLOCK_CREDIT_LINE_SPACE_PX
    bbox = d.multiline_textbbox((0, 0), text, font=font, spacing=spacing, align="center")
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    bottom_y = CLOCK_CREDIT_TICK_TOP_Y - CLOCK_CREDIT_GAP_ABOVE_TICK
    top_y = bottom_y - th
    left = (GAUGE_PIXEL_SIZE - tw) // 2 - bbox[0]
    d.multiline_text((left, top_y), text, font=font, fill=WHITE, spacing=spacing, align="center")


def layout_clock(
    heights: Dict[str, int], *, use_24h: bool = False
) -> Tuple[int, int, int, int]:
    """
    Replicate clock_place_blocks_absolute() placement.

    Returns:
        rule_w, strip_x, strip_y, date_row_top_y
    """
    rule_w = max_time_row_rule_width_24h() if use_24h else max_time_row_rule_width()

    tr_h = heights["time"] if use_24h else time_row_height_lvgl(heights)
    tr_y = CLOCK_RULE_THICK + CLOCK_TIME_BAND_PAD_ROW
    mid_tr = tr_y + tr_h // 2
    strip_y = CLOCK_ABS_TIME_ROW_CY_PX - mid_tr
    strip_x = (GAUGE_PIXEL_SIZE - rule_w) // 2
    strip_h = tr_h + 2 * CLOCK_RULE_THICK + 2 * CLOCK_TIME_BAND_PAD_ROW

    # Firmware uses CLOCK_ABS_DATE_OVERLAP_PX (transparent widgets). Preview defaults
    # to PREVIEW_DATE_OVERLAP_PX so the bottom rule does not show through PNG gaps.
    date_row_top = strip_y + strip_h - PREVIEW_DATE_OVERLAP_PX

    return rule_w, strip_x, strip_y, date_row_top


def render(
    time_text: str,
    ampm_text: str,
    day_text: str,
    date_text: str,
    out_path: Path,
    *,
    use_24h: bool = False,
) -> Path:
    heights = load_lvgl_line_heights(REPO_ROOT)
    rule_w, strip_x, strip_y, date_row_top = layout_clock(heights, use_24h=use_24h)

    canvas = Image.new("RGBA", (GAUGE_PIXEL_SIZE, GAUGE_PIXEL_SIZE), BLACK)
    draw_gauge_background(canvas)
    render_time_band(
        canvas,
        time_text,
        ampm_text,
        rule_w,
        strip_x,
        strip_y,
        heights,
        use_24h=use_24h,
    )
    render_date_columns(canvas, day_text, date_text, date_row_top, heights)
    render_credit(canvas)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(out_path, format="PNG", optimize=True)
    return out_path


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--time", default="12:00", help="HH:MM (default %(default)s)")
    p.add_argument("--ampm", default="AM", choices=["AM", "PM"], help="AM/PM badge (ignored with --24h)")
    p.add_argument(
        "--24h",
        dest="use_24h",
        action="store_true",
        help="24-hour clock subpage layout (narrower rules, no AM/PM)",
    )
    p.add_argument("--day", default="SUN", help="3-letter weekday (default %(default)s)")
    p.add_argument("--date", default="JAN 00", help="Month + day (default '%(default)s')")
    p.add_argument("--out", default=str(DEFAULT_OUT), help="Output PNG path")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    out = render(
        args.time,
        args.ampm,
        args.day,
        args.date,
        Path(args.out),
        use_24h=args.use_24h,
    )
    rel = os.path.relpath(out, REPO_ROOT)
    print(f"Wrote preview: {rel}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
