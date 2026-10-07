#include "ui.h"
#include <stdio.h>

lv_obj_t* ui_box(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t* ui_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

// Put `b` to the right of `a` with their text baselines level, for labels in
// different font sizes (a big number and its unit).
void ui_align_baseline_right(lv_obj_t* b, lv_obj_t* a, int gap) {
    const lv_font_t* fa = lv_obj_get_style_text_font(a, LV_PART_MAIN);
    const lv_font_t* fb = lv_obj_get_style_text_font(b, LV_PART_MAIN);
    int dy = (fa->line_height - fa->base_line) - (fb->line_height - fb->base_line);
    lv_obj_align_to(b, a, LV_ALIGN_OUT_RIGHT_TOP, gap, dy);
}

// Three significant digits in the largest unit that keeps the value >= 1,
// the way network gear reports throughput (bits, decimal prefixes).
const char* ui_fmt_rate(float bits, char* out, size_t len) {
    static const char* const units[] = {"Kbps", "Mbps", "Gbps"};
    float v = bits / 1000.0f;
    int u = 0;
    while (v >= 999.5f && u < 2) { v /= 1000.0f; u++; }
    if (v < 0.005f)       snprintf(out, len, "0");
    else if (v < 9.995f)  snprintf(out, len, "%.2f", v);
    else if (v < 99.95f)  snprintf(out, len, "%.1f", v);
    else                  snprintf(out, len, "%.0f", v);
    return units[u];
}
