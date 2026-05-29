#include "Page_Clock.h"

#include <stdbool.h>
#include <stdio.h>

#include "Fonts.h"
#include "PCF85063.h"
#include "Boot_Logo_Api.h"
#include "Storage_Manager.h"
#include "UI_Navigation.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gauge_bg_lvgl.h"
#include "lvgl.h"

static const char *const PAGE_CLOCK_LOG_TAG = "Page_Clock";

static const char k_cobra_clock_logo_spiffs[] = "/Logos/Cobra_text.bin";

static uint8_t     *s_cobra_text_pixels;
static lv_img_dsc_t s_cobra_text_dsc;

static void clock_cobra_text_release(void)
{
    if (s_cobra_text_pixels != NULL) {
        heap_caps_free(s_cobra_text_pixels);
        s_cobra_text_pixels = NULL;
    }
    lv_memset_00(&s_cobra_text_dsc, sizeof(s_cobra_text_dsc));
}

/** Load .bin into PSRAM and use LV_IMG_SRC_VARIABLE (same as last pushed firmware). */
static bool clock_cobra_text_load(void)
{
    clock_cobra_text_release();

    if (!storage_file_exists(k_cobra_clock_logo_spiffs)) {
        return false;
    }

    return lvgl_bin_load_to_dsc_from_spiffs(k_cobra_clock_logo_spiffs,
                                           &s_cobra_text_dsc,
                                           &s_cobra_text_pixels) == ESP_OK;
}

/*
 * Absolute layout on root (0,0) = top-left; panel matches GAUGE_PIXEL_SIZE (360×360).
 * Tune only these numbers — no lv_obj_align / align_to between blocks.
 */
#define CLOCK_ABS_TIME_ROW_CY_PX 160 /* target screen Y for vertical center of HH:MM + AM row */
#define CLOCK_ABS_DATE_OVERLAP_PX 25 /* date_row top Y = time_strip bottom − this (overlap) */

#define CLOCK_DATE_GAP        3 /* was 4; -20 % gap between DAY | DATE columns */
#define CLOCK_TIME_PAD_COLUMN 8 /* gap between HH:MM and AM/PM (match time_row pad_column) */
/** Gap (px) between top rule and time, time and bottom rule inside `time_strip`. */
#define CLOCK_TIME_BAND_PAD_ROW 15
/**
 * Y gap (px) between the DAY / DATE caption labels and the values (weekday, month+day) below.
 * (Flex `pad_row` on each caption column; tune for vertical spacing only.)
 */
#define CLOCK_DATE_CAPTION_VALUE_GAP_PX 10

/** Cobra logo width as % of the top time-strip rule (`rule_visible_w`). */
#define CLOCK_COBRA_IMG_WIDTH_PCT_OF_RULE 64

/** Bottom dial credit (D-DIN Condensed Regular 15 px); gap above lower cardinal tick. */
#define CLOCK_CREDIT_LINE_SPACE_PX      3 /* extra Y gap between title line and "2026" */
#define CLOCK_CREDIT_GAP_ABOVE_TICK 8
#define CLOCK_CREDIT_TICK_TOP_PX \
    ((lv_coord_t)((GAUGE_PIXEL_SIZE) / 2 + (GAUGE_RING_RADIUS) - (GAUGE_TICK_LENGTH)))

/** Root Y of bottom edge of 12 o'clock tick (matches `gauge_bg_lvgl.c` canvas geometry). */
#define CLOCK_UPPER_TICK_BOTTOM_Y \
    ((lv_coord_t)((GAUGE_PIXEL_SIZE) / 2 - (GAUGE_RING_RADIUS) + (GAUGE_TICK_LENGTH)))

/** LVGL text letter-space (px) for clock labels — keep in sync with `lv_txt_get_size` below. */
#define CLOCK_TIME_LETTER_SPACE         5
#define CLOCK_AMPM_LETTER_SPACE         0
#define CLOCK_DATE_VALUE_LETTER_SPACE   0

/** Max width of "12:00" / "00:00" (whichever is wider) + pad + max(AM,PM) for rule length. */
static lv_coord_t clock_max_time_row_width(void)
{
    const lv_coord_t ls_time = (lv_coord_t)CLOCK_TIME_LETTER_SPACE;
    const lv_coord_t ls_ampm = (lv_coord_t)CLOCK_AMPM_LETTER_SPACE;
    lv_point_t sz_12, sz_00, sz_am, sz_pm;
    lv_txt_get_size(&sz_12, "12:00", &font_ddin_115, ls_time, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&sz_00, "00:00", &font_ddin_115, ls_time, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&sz_am, "AM", &font_ddin_reg_32, ls_ampm, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&sz_pm, "PM", &font_ddin_reg_32, ls_ampm, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t tw  = LV_MAX(sz_12.x, sz_00.x);
    lv_coord_t asz = LV_MAX(sz_am.x, sz_pm.x);
    return tw + (lv_coord_t)CLOCK_TIME_PAD_COLUMN + asz;
}

static lv_obj_t  *s_time_lbl;
static lv_obj_t  *s_ampm_lbl;
static lv_obj_t  *s_day_val_lbl;
static lv_obj_t  *s_date_val_lbl;
static lv_timer_t *s_clock_timer;
static bool        s_clock_is_24h;

static const char *const MONTHS_3[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

/* PCF85063: dotw 0 = Sunday */
static const char *const WEEKDAYS_3[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

static lv_obj_t *white_rule_create(lv_obj_t *parent, lv_coord_t w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, 2);
    lv_obj_set_style_bg_color(r, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(r, 0, LV_PART_MAIN);
    return r;
}

/** Place time_strip and date_row using CLOCK_ABS_* constants (root pixel coordinates). */
static void clock_place_blocks_absolute(lv_obj_t *root, lv_obj_t *time_strip, lv_obj_t *time_row,
                                        lv_obj_t *date_row, lv_coord_t rule_w)
{
    lv_obj_update_layout(root);

    lv_coord_t tr_y = lv_obj_get_y(time_row);
    lv_coord_t tr_h = lv_obj_get_height(time_row);
    lv_coord_t mid_tr = tr_y + tr_h / 2;

    lv_coord_t strip_y = (lv_coord_t)CLOCK_ABS_TIME_ROW_CY_PX - mid_tr;
    lv_coord_t strip_x = (GAUGE_PIXEL_SIZE - rule_w) / 2;
    lv_obj_set_pos(time_strip, strip_x, strip_y);

    lv_obj_update_layout(root);

    lv_coord_t strip_h = lv_obj_get_height(time_strip);
    lv_coord_t date_y  = strip_y + strip_h - (lv_coord_t)CLOCK_ABS_DATE_OVERLAP_PX;
    lv_coord_t date_w  = lv_obj_get_width(date_row);
    lv_obj_set_pos(date_row, (GAUGE_PIXEL_SIZE - date_w) / 2, date_y);
    lv_obj_update_layout(root);
}

/** Caption + value column ("DAY" / "DATE" + weekday or calendar text). */
static void caption_column(lv_obj_t *parent, const char *caption_text, lv_obj_t **value_out)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_flex_grow(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, (lv_coord_t)CLOCK_DATE_CAPTION_VALUE_GAP_PX, LV_PART_MAIN);

    lv_obj_t *cap = lv_label_create(col);
    lv_obj_set_style_pad_all(cap, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &font_ddin_reg_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(cap, caption_text);

    lv_obj_t *val = lv_label_create(col);
    lv_obj_set_style_pad_all(val, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &font_ddin_reg_44, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(val, (lv_coord_t)CLOCK_DATE_VALUE_LETTER_SPACE, LV_PART_MAIN);
    if (value_out != NULL) {
        *value_out = val;
    }
}

static void clock_clear_handles(void)
{
    s_time_lbl      = NULL;
    s_ampm_lbl      = NULL;
    s_day_val_lbl   = NULL;
    s_date_val_lbl = NULL;
}

static void clock_refresh(lv_timer_t *t)
{
    (void)t;

    if (ux_navigation_get_active_page() != UX_PAGE_CLOCK) {
        if (s_clock_timer != NULL) {
            lv_timer_del(s_clock_timer);
            s_clock_timer = NULL;
        }
        clock_clear_handles();
        return;
    }

    if (s_time_lbl == NULL) {
        return;
    }

    PCF85063_Read_Time(&datetime);

    uint8_t h24 = datetime.hour;
    uint8_t h12 = (uint8_t)(h24 % 12U);
    if (h12 == 0U) {
        h12 = 12U;
    }

    char tbuf[8];
    if (s_clock_is_24h) {
        (void)snprintf(tbuf, sizeof(tbuf), "%02u:%02u",
                       (unsigned)h24, (unsigned)datetime.minute);
    } else {
        (void)snprintf(tbuf, sizeof(tbuf), "%u:%02u",
                       (unsigned)h12, (unsigned)datetime.minute);
    }
    lv_label_set_text(s_time_lbl, tbuf);
    if (!s_clock_is_24h && s_ampm_lbl != NULL) {
        lv_label_set_text(s_ampm_lbl, (h24 < 12U) ? "AM" : "PM");
    }

    uint8_t wday = datetime.dotw;
    if (wday > 6U) {
        wday = 0U;
    }
    lv_label_set_text(s_day_val_lbl, WEEKDAYS_3[wday]);

    uint8_t m = datetime.month;
    if (m < 1U || m > 12U) {
        m = 1U;
    }
    char mbuf[14];
    (void)snprintf(mbuf, sizeof(mbuf), "%s %u",
                   MONTHS_3[m - 1U], (unsigned)datetime.day);
    lv_label_set_text(s_date_val_lbl, mbuf);
}

esp_err_t page_clock_render(uint8_t subpage_index, lv_obj_t *root)
{
    s_clock_is_24h = (subpage_index == 1U);

    if (s_clock_timer != NULL) {
        lv_timer_del(s_clock_timer);
        s_clock_timer = NULL;
    }
    clock_clear_handles();
    clock_cobra_text_release();

    lv_obj_t *gauge = create_gauge_background(root);
    if (gauge != NULL) {
        /* Screen flex/theme must not resize or reposition the dial canvas. */
        lv_obj_add_flag(gauge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }

    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_layout(root, 0); /* no flex/grid on screen — avoids stretched gaps between widgets */

    /* Fixed rule length: always size for 12h row (HH:MM + AM/PM), even in 24h mode. */
    const lv_coord_t rule_w         = clock_max_time_row_width();
    const lv_coord_t rule_visible_w = (lv_coord_t)((int32_t)rule_w * 90 / 100); /* 90% of rule width */

    /*
     * Time strip: top rule, HH:MM+AM, bottom rule — flex only *inside* the strip.
     * Final root positions are set in clock_place_blocks_absolute() from CLOCK_ABS_*.
     */
    lv_obj_t *time_strip = lv_obj_create(root);
    lv_obj_remove_style_all(time_strip);
    lv_obj_set_width(time_strip, rule_w);
    lv_obj_set_height(time_strip, LV_SIZE_CONTENT);
    lv_obj_set_layout(time_strip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_strip, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(time_strip, CLOCK_TIME_BAND_PAD_ROW, LV_PART_MAIN);

    lv_obj_t *top_rule = white_rule_create(time_strip, rule_visible_w);

    /* Flex row: HH:MM and AM share the row's bottom edge (label bbox bottom = baseline+descent). */
    lv_obj_t *time_row = lv_obj_create(time_strip);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(time_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row,
                          LV_FLEX_ALIGN_CENTER, /* main: HH:MM + AM centered as a group   */
                          LV_FLEX_ALIGN_END,    /* cross: bottoms aligned                 */
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(time_row, CLOCK_TIME_PAD_COLUMN, LV_PART_MAIN);

    s_time_lbl = lv_label_create(time_row);
    /* D-DIN Condensed Bold 115 px; avoid transform_zoom on ESP32-S3. */
    lv_obj_set_style_text_font(s_time_lbl, &font_ddin_115, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_time_lbl, (lv_coord_t)CLOCK_TIME_LETTER_SPACE, LV_PART_MAIN);

    s_ampm_lbl = lv_label_create(time_row);
    lv_obj_set_style_text_font(s_ampm_lbl, &font_ddin_reg_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ampm_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_ampm_lbl, (lv_coord_t)CLOCK_AMPM_LETTER_SPACE, LV_PART_MAIN);
    if (s_clock_is_24h) {
        lv_obj_add_flag(s_ampm_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    (void)white_rule_create(time_strip, rule_visible_w);

    lv_obj_t *date_row = lv_obj_create(root);
    lv_obj_remove_style_all(date_row);
    lv_obj_set_style_pad_all(date_row, 0, LV_PART_MAIN);
    lv_obj_set_size(date_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(date_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(date_row, CLOCK_DATE_GAP, LV_PART_MAIN);
    lv_obj_set_style_flex_grow(date_row, 0, LV_PART_MAIN);

    caption_column(date_row, "DAY", &s_day_val_lbl);
    caption_column(date_row, "DATE", &s_date_val_lbl);

    lv_obj_t *credit_lbl = lv_label_create(root);
    lv_obj_remove_style_all(credit_lbl);
    lv_obj_set_style_pad_all(credit_lbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(credit_lbl, &font_ddin_reg_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(credit_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(credit_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(credit_lbl, (lv_coord_t)CLOCK_CREDIT_LINE_SPACE_PX, LV_PART_MAIN);
    lv_label_set_long_mode(credit_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(credit_lbl, GAUGE_PIXEL_SIZE);
    lv_label_set_text(credit_lbl, "STEIN CLOCK DESIGN\n2026"); /* width set before text for WRAP */

    lv_obj_set_pos(time_strip, 0, 0);
    lv_obj_set_pos(date_row, 0, 0);

    clock_place_blocks_absolute(root, time_strip, time_row, date_row, rule_w);

    lv_obj_update_layout(credit_lbl);
    {
        lv_coord_t ch = lv_obj_get_height(credit_lbl);
        lv_coord_t cy  = CLOCK_CREDIT_TICK_TOP_PX - (lv_coord_t)CLOCK_CREDIT_GAP_ABOVE_TICK - ch;
        lv_obj_set_pos(credit_lbl, 0, cy);
    }

    if (!storage_file_exists(k_cobra_clock_logo_spiffs)) {
        ESP_LOGW(PAGE_CLOCK_LOG_TAG,
                 "Clock logo missing on SPIFFS: %s (flash userdata partition, e.g. full `idf.py flash`)",
                 k_cobra_clock_logo_spiffs);
    } else if (!clock_cobra_text_load()) {
        ESP_LOGW(PAGE_CLOCK_LOG_TAG, "Failed to load %s from SPIFFS", k_cobra_clock_logo_spiffs);
    } else {
        const lv_coord_t cobra_w = s_cobra_text_dsc.header.w;
        const lv_coord_t cobra_h = s_cobra_text_dsc.header.h;

        lv_obj_t *logo = lv_img_create(root);
        if (logo != NULL) {
            lv_obj_set_style_opa(logo, LV_OPA_COVER, 0);
            lv_obj_set_style_clip_corner(logo, false, 0);
            lv_obj_add_flag(logo, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_img_set_src(logo, &s_cobra_text_dsc);
            lv_img_set_pivot(logo, (lv_coord_t)(cobra_w / 2U), (lv_coord_t)(cobra_h / 2U));

            const lv_coord_t cobra_target_w = (lv_coord_t)((int32_t)rule_visible_w *
                                                           CLOCK_COBRA_IMG_WIDTH_PCT_OF_RULE /
                                                           100);
            uint16_t cobra_zoom = (uint16_t)((uint32_t)cobra_target_w * 256U / (uint32_t)cobra_w);
            if (cobra_zoom < 1U) {
                cobra_zoom = 1U;
            }
            lv_img_set_zoom(logo, cobra_zoom);

            lv_obj_update_layout(root);
            lv_obj_update_layout(logo);

            lv_coord_t y_rule_top = lv_obj_get_y(time_strip) + lv_obj_get_y(top_rule);
            lv_coord_t y_mid      = (CLOCK_UPPER_TICK_BOTTOM_Y + y_rule_top) / 2;

            const lv_coord_t iw = (lv_coord_t)((int32_t)cobra_w * (int32_t)cobra_zoom / 256);
            const lv_coord_t ih = (lv_coord_t)((int32_t)cobra_h * (int32_t)cobra_zoom / 256);
            const lv_coord_t y_top  = y_mid - ih / 2;
            const lv_coord_t x_left = (GAUGE_PIXEL_SIZE - iw) / 2;

            lv_obj_set_pos(logo, x_left, y_top);
            lv_obj_move_foreground(logo);

            ESP_LOGI(PAGE_CLOCK_LOG_TAG,
                     "clock Cobra text: cf=%u rule_w=%d target_w=%d zoom=%u y_mid=%d x=%d y=%d w=%d h=%d",
                     (unsigned)s_cobra_text_dsc.header.cf, (int)rule_visible_w, (int)cobra_target_w,
                     (unsigned)cobra_zoom, (int)y_mid, (int)x_left, (int)y_top, (int)iw, (int)ih);
        }
    }

    clock_refresh(NULL);
    s_clock_timer = lv_timer_create(clock_refresh, 1000, NULL);

    return ESP_OK;
}

uint8_t page_clock_get_subpage_count(void)
{
    return 2;
}
