#!/usr/bin/env python3
"""
Page_Clock layout preview.

Renders an approximation of main/Page_Clock/Page_Clock.c to a PNG so you can
iterate on the layout (font sizes, paddings, alignments) without flashing.

The constants below mirror the C source. Edit them in lockstep with
`main/Page_Clock/Page_Clock.c` and `main/gauge_bg_lvgl.h` to keep the preview
faithful.

Usage (from the project root):

    python tools/preview_clock.py                       # 12:00 AM SUN JAN 00
    python tools/preview_clock.py --time 10:38 --ampm AM --day WED --date "MAY 28"
    python tools/preview_clock.py --out build/clock_alt.png

Requires: Pillow (`pip install Pillow`).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Tuple

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover
    sys.stderr.write("Pillow is required. Install with: pip install Pillow\n")
    raise SystemExit(1) from exc


# --- Constants mirrored from the firmware ----------------------------------

# main/gauge_bg_lvgl.h
GAUGE_PIXEL_SIZE = 360
GAUGE_RING_RADIUS = 170
GAUGE_TICK_LENGTH = 14

# Tick width (gauge_bg_lvgl.c:58)
TICK_W = 4
# Ring stroke (gauge_bg_lvgl.c:47)
RING_STROKE = 3

# main/Page_Clock/Page_Clock.c
CLOCK_RULE_THICK = 2
CLOCK_RULE_PAD_ROW = 10            # CLOCK_TIME_BAND_PAD_ROW in Page_Clock.c (rule–time–rule gap)
CLOCK_TIME_BAND_Y_OFFSET = 6       # lv_obj_align(LV_ALIGN_CENTER, 0, +6)
CLOCK_TIME_PAD_COLUMN = 14         # gap between HH:MM and AM/PM
CLOCK_RULE_VISIBLE_PCT = 80        # rule width = full row width * pct / 100 (Page_Clock.c rule_visible_w)
CLOCK_DATE_GAP = 3                 # gap between DAY and DATE columns (match Page_Clock.c)
CLOCK_DATE_BOTTOM_OFFSET = 36      # lv_obj_align(BOTTOM_MID, 0, -36)
CLOCK_DATE_CAPTION_PAD_ROW = 4    # CLOCK_CAPTION_PAD_ROW in Page_Clock.c

# Firmware uses font_ddin_115 (lv_font_conv) for HH:MM; ~+20% vs 96px.
FONT_SIZE_TIME = 115
FONT_SIZE_VALUE = 28  # font_ddin_28
FONT_SIZE_CAPTION = 18  # font_ddin_18 (DAY / DATE)


# --- Paths -----------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
TTF_PATH = REPO_ROOT / "tools" / "fonts" / "D-DINCondensed-Bold.ttf"
DEFAULT_OUT = REPO_ROOT / "build" / "preview_clock.png"


# --- Helpers ---------------------------------------------------------------

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)


def load_font(size: int) -> ImageFont.FreeTypeFont:
    if not TTF_PATH.exists():
        raise SystemExit(
            f"Missing font: {TTF_PATH}\n"
            "Place D-DINCondensed-Bold.ttf there (SIL OFL, "
            "https://github.com/amcchord/datto-d-din)."
        )
    return ImageFont.truetype(str(TTF_PATH), size)


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
    # top, right, bottom, left
    d.rectangle([cx - half_tw, cy - r, cx - half_tw + tw, cy - r + tlen], fill=WHITE)
    d.rectangle([cx + (r - tlen), cy - half_tw, cx + r, cy - half_tw + tw], fill=WHITE)
    d.rectangle([cx - half_tw, cy + (r - tlen), cx - half_tw + tw, cy + r], fill=WHITE)
    d.rectangle([cx - r, cy - half_tw, cx - r + tlen, cy - half_tw + tw], fill=WHITE)


def text_size(font: ImageFont.FreeTypeFont, text: str) -> Tuple[int, int, int, int]:
    """Return (left, top, right, bottom) of inked bbox at origin."""
    return font.getbbox(text)


def max_time_row_rule_width() -> int:
    """Match `clock_max_time_row_width()` in Page_Clock.c: max(12:00,00:00) + pad + max(AM,PM)."""
    font_time = load_font(FONT_SIZE_TIME)
    font_ampm = load_font(FONT_SIZE_VALUE)
    w12 = text_size(font_time, "12:00")[2] - text_size(font_time, "12:00")[0]
    w00 = text_size(font_time, "00:00")[2] - text_size(font_time, "00:00")[0]
    w_am = text_size(font_ampm, "AM")[2] - text_size(font_ampm, "AM")[0]
    w_pm = text_size(font_ampm, "PM")[2] - text_size(font_ampm, "PM")[0]
    return max(w12, w00) + CLOCK_TIME_PAD_COLUMN + max(w_am, w_pm)


def render_time_band(canvas: Image.Image, time_text: str, ampm_text: str) -> None:
    """White rule above + below, with HH:MM and AM/PM bottom-aligned in between."""
    d = ImageDraw.Draw(canvas)
    font_time = load_font(FONT_SIZE_TIME)
    font_ampm = load_font(FONT_SIZE_VALUE)

    bbox_time = text_size(font_time, time_text)
    bbox_ampm = text_size(font_ampm, ampm_text)

    time_w = bbox_time[2] - bbox_time[0]
    time_h = bbox_time[3] - bbox_time[1]
    ampm_w = bbox_ampm[2] - bbox_ampm[0]

    row_w = time_w + CLOCK_TIME_PAD_COLUMN + ampm_w
    rule_w         = max_time_row_rule_width()
    rule_visible_w = rule_w * CLOCK_RULE_VISIBLE_PCT // 100
    band_total_h = time_h + 2 * (CLOCK_RULE_PAD_ROW + CLOCK_RULE_THICK)

    cx = GAUGE_PIXEL_SIZE // 2
    cy = GAUGE_PIXEL_SIZE // 2 + CLOCK_TIME_BAND_Y_OFFSET

    band_top = cy - band_total_h // 2
    band_bottom = band_top + band_total_h

    # Rules (visible width = max case per firmware * CLOCK_RULE_VISIBLE_PCT, centered)
    rule_top_y = band_top + CLOCK_RULE_THICK // 2
    rule_bot_y = band_bottom - CLOCK_RULE_THICK // 2 - CLOCK_RULE_THICK
    d.rectangle(
        [cx - rule_visible_w // 2, rule_top_y,
         cx + rule_visible_w // 2, rule_top_y + CLOCK_RULE_THICK],
        fill=WHITE,
    )
    d.rectangle(
        [cx - rule_visible_w // 2, rule_bot_y,
         cx + rule_visible_w // 2, rule_bot_y + CLOCK_RULE_THICK],
        fill=WHITE,
    )

    # Bottom-align labels: pin both bbox bottoms to the same baseline_y.
    baseline_y = band_bottom - CLOCK_RULE_PAD_ROW - CLOCK_RULE_THICK

    row_left = cx - row_w // 2
    time_x = row_left - bbox_time[0]
    time_y = baseline_y - bbox_time[3]
    d.text((time_x, time_y), time_text, font=font_time, fill=WHITE)

    ampm_x = row_left + time_w + CLOCK_TIME_PAD_COLUMN - bbox_ampm[0]
    ampm_y = baseline_y - bbox_ampm[3]
    d.text((ampm_x, ampm_y), ampm_text, font=font_ampm, fill=WHITE)


def render_date_columns(canvas: Image.Image, day_text: str, date_text: str) -> None:
    """DAY + weekday · DATE + month-day (caption_column in Page_Clock.c)."""
    d = ImageDraw.Draw(canvas)
    font_caption = load_font(FONT_SIZE_CAPTION)
    font_value = load_font(FONT_SIZE_VALUE)

    def column_metrics(caption: str, value: str) -> Tuple[int, int, Tuple, Tuple]:
        bb_c = text_size(font_caption, caption)
        bb_v = text_size(font_value, value)
        col_w = max(bb_c[2] - bb_c[0], bb_v[2] - bb_v[0])
        col_h = (bb_c[3] - bb_c[1]) + CLOCK_DATE_CAPTION_PAD_ROW + (bb_v[3] - bb_v[1])
        return col_w, col_h, bb_c, bb_v

    day_w, day_h, day_bb_c, day_bb_v = column_metrics("DAY", day_text)
    dat_w, dat_h, dat_bb_c, dat_bb_v = column_metrics("DATE", date_text)

    row_w = day_w + CLOCK_DATE_GAP + dat_w
    row_h = max(day_h, dat_h)

    cx = GAUGE_PIXEL_SIZE // 2
    row_top = GAUGE_PIXEL_SIZE - CLOCK_DATE_BOTTOM_OFFSET - row_h
    row_left = cx - row_w // 2

    def draw_column(left_x: int, col_w: int, caption: str, value: str,
                    bb_c: Tuple, bb_v: Tuple) -> None:
        cap_w = bb_c[2] - bb_c[0]
        val_w = bb_v[2] - bb_v[0]
        cap_x = left_x + (col_w - cap_w) // 2 - bb_c[0]
        cap_y = row_top - bb_c[1]
        d.text((cap_x, cap_y), caption, font=font_caption, fill=WHITE)

        val_x = left_x + (col_w - val_w) // 2 - bb_v[0]
        val_y = row_top + (bb_c[3] - bb_c[1]) + CLOCK_DATE_CAPTION_PAD_ROW - bb_v[1]
        d.text((val_x, val_y), value, font=font_value, fill=WHITE)

    draw_column(row_left, day_w, "DAY", day_text, day_bb_c, day_bb_v)
    draw_column(row_left + day_w + CLOCK_DATE_GAP, dat_w, "DATE", date_text, dat_bb_c, dat_bb_v)


def render(time_text: str, ampm_text: str, day_text: str, date_text: str,
           out_path: Path) -> Path:
    canvas = Image.new("RGBA", (GAUGE_PIXEL_SIZE, GAUGE_PIXEL_SIZE), BLACK)
    draw_gauge_background(canvas)
    render_time_band(canvas, time_text, ampm_text)
    render_date_columns(canvas, day_text, date_text)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(out_path, format="PNG", optimize=True)
    return out_path


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--time", default="12:00", help="HH:MM (default %(default)s)")
    p.add_argument("--ampm", default="AM", choices=["AM", "PM"], help="AM/PM badge")
    p.add_argument("--day", default="SUN", help="3-letter weekday (default %(default)s)")
    p.add_argument("--date", default="JAN 00", help="Month + day (default '%(default)s')")
    p.add_argument("--out", default=str(DEFAULT_OUT), help="Output PNG path")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    out = render(args.time, args.ampm, args.day, args.date, Path(args.out))
    rel = os.path.relpath(out, REPO_ROOT)
    print(f"Wrote preview: {rel}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
