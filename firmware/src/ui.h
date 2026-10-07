#pragma once
#include <lvgl.h>
#include "link.h"

#define REPO_URL "https://github.com/dcryptoRS/netmeter-4848s040"

// Dashboard (ui_dash.cpp)
void ui_init(void);
void ui_on_sample(const NetSample& s);
void ui_tick(void);                 // call every loop
void ui_show_dashboard(void);
void ui_apply_settings(void);       // plan / alerts / language changed
void ui_toggle_expanded(void);
void ui_cycle_range(void);

// Settings (ui_settings.cpp)
void ui_settings_open(void);
void ui_settings_open_keypad(bool down);   // QA: console `keypad`

// Shared helpers (ui_common.cpp)
lv_obj_t* ui_box(lv_obj_t* parent);
lv_obj_t* ui_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text);
void      ui_align_baseline_right(lv_obj_t* b, lv_obj_t* a, int gap);
const char* ui_fmt_rate(float bits, char* out, size_t len);   // returns the unit
