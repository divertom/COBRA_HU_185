#pragma once

#include "lvgl.h"

/**
 * Canonical gauge canvas matches the LCD (360).
 * Usable UX circle: radius GAUGE_RING_RADIUS from center, cardinal ticks inward
 * GAUGE_TICK_LENGTH, then GAUGE_INSIDE_MARGIN clearance inside tick inner corners.
 */

#define GAUGE_PIXEL_SIZE               360
#define GAUGE_RING_RADIUS              170
#define GAUGE_TICK_LENGTH              14
#define GAUGE_INSIDE_MARGIN            3

#define GAUGE_USABLE_RADIUS            ((lv_coord_t)((GAUGE_RING_RADIUS) - (GAUGE_TICK_LENGTH) - (GAUGE_INSIDE_MARGIN)))

/*
 * Largest axis-aligned square strictly inside usable circle (~ sidelength = √2 × R).
 * Use for vertically stacked content so diagonal corners cannot cross the bezel.
 */
#define GAUGE_USABLE_SQUARE_SIDE                                                                          \
    ((lv_coord_t)(((int32_t)GAUGE_USABLE_RADIUS * (int32_t)14142) / (int32_t)10000))

lv_obj_t *create_gauge_background(lv_obj_t *parent);
