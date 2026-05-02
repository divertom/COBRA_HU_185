#include "gauge_bg_lvgl.h"

#include "esp_heap_caps.h"

#define GAUGE_CANVAS_SIDE GAUGE_PIXEL_SIZE

static lv_color_t *gauge_canvas_buf_alloc(void)
{
    static lv_color_t *buf;
    if (buf != NULL) {
        return buf;
    }

    const size_t bytes = (size_t)GAUGE_CANVAS_SIDE * (size_t)GAUGE_CANVAS_SIDE * sizeof(lv_color_t);
    buf = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buf;
}

lv_obj_t *create_gauge_background(lv_obj_t *parent)
{
    lv_color_t *canvas_buf = gauge_canvas_buf_alloc();
    if (canvas_buf == NULL) {
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, canvas_buf, GAUGE_CANVAS_SIDE, GAUGE_CANVAS_SIDE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, GAUGE_CANVAS_SIDE, GAUGE_CANVAS_SIDE);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t white = lv_color_white();
    lv_color_t black = lv_color_black();

    lv_canvas_fill_bg(canvas, black, LV_OPA_COVER);

    const lv_coord_t cx = GAUGE_CANVAS_SIDE / 2;
    const lv_coord_t cy = GAUGE_CANVAS_SIDE / 2;
    const lv_coord_t r = GAUGE_RING_RADIUS;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = white;
    arc_dsc.width = 3;
    arc_dsc.opa = LV_OPA_COVER;
    lv_canvas_draw_arc(canvas, cx, cy, r, 0, 360, &arc_dsc);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = white;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.border_width = 0;

    const int tick_len = GAUGE_TICK_LENGTH;
    const int tick_w = 4;

    lv_canvas_draw_rect(canvas, cx - tick_w / 2, cy - r, tick_w, tick_len, &rect_dsc);
    lv_canvas_draw_rect(canvas, cx + (r - tick_len), cy - tick_w / 2, tick_len, tick_w, &rect_dsc);
    lv_canvas_draw_rect(canvas, cx - tick_w / 2, cy + (r - tick_len), tick_w, tick_len, &rect_dsc);
    lv_canvas_draw_rect(canvas, cx - r, cy - tick_w / 2, tick_len, tick_w, &rect_dsc);

    return canvas;
}
