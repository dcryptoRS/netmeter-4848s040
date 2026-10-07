#include "ui.h"
#include "theme.h"
#include "i18n.h"
#include "settings.h"
#include "board/board.h"
#include <Arduino.h>
#include <string.h>

// Settings screen: contracted plan (download / upload), alert thresholds,
// brightness and language. Rebuilt from scratch each time it opens, so a
// language change only has to rebuild this screen.
//
// Sliders save on release, not while dragging: NVS flash has finite write
// endurance and a drag produces dozens of value changes. Brightness is still
// applied live while dragging so the user sees what they're choosing.

#define PAD        18
#define HEADER_H   56
#define ROW_H      52
#define ALERT_STEP 5
#define ALERT_OFF_POS 105   // slider position past 100 % that means "off"

static lv_obj_t* scr = NULL;
static Settings  s_edit;                 // working copy

static lv_obj_t* plan_val[2] = {};       // 0 = down, 1 = up
static lv_obj_t* alert_val[2] = {};
static lv_obj_t* bright_val = NULL;
static lv_obj_t* lang_btn[2] = {};

// Keypad dialog
static lv_obj_t* kp_overlay = NULL;
static lv_obj_t* kp_value = NULL;
static lv_obj_t* kp_unit = NULL;
static lv_obj_t* kp_ok = NULL;
static char      kp_buf[8];
static bool      kp_fresh = true;        // first digit replaces the shown value
static bool      kp_down = true;

// ============================================================================
// Small builders
// ============================================================================
static lv_obj_t* section(lv_obj_t* parent, StrId caption) {
    lv_obj_t* cap = ui_label(parent, &font_inter_12, C_TEXT_3, tr(caption));
    lv_obj_set_style_text_letter_space(cap, 1, 0);
    lv_obj_set_style_pad_left(cap, 4, 0);
    lv_obj_t* card = ui_box(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, C_BORDER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static lv_obj_t* row(lv_obj_t* card, bool divider) {
    lv_obj_t* r = ui_box(card);
    lv_obj_set_size(r, lv_pct(100), ROW_H);
    lv_obj_set_style_pad_hor(r, 14, 0);
    if (divider) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, C_BORDER, 0);
    }
    return r;
}

static lv_obj_t* row_label(lv_obj_t* r, bool down, StrId text) {
    lv_obj_t* arrow = ui_label(r, &font_inter_md_16, down ? C_DOWN : C_UP, down ? "↓" : "↑");
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* l = ui_label(r, &font_inter_md_16, C_TEXT, tr(text));
    lv_obj_align_to(l, arrow, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    return l;
}

static lv_obj_t* slider(lv_obj_t* r, int32_t min, int32_t max, int32_t val, int x, int w) {
    lv_obj_t* s = lv_slider_create(r);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, w, 4);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, x, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 18);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_DOWN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);   // knob = 4 + 2*8 = 20 px
    return s;
}

static lv_obj_t* value_label(lv_obj_t* r) {
    lv_obj_t* l = ui_label(r, &font_inter_md_14, C_TEXT_2, "");
    lv_obj_set_width(l, 52);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
    return l;
}

// ============================================================================
// Values
// ============================================================================
static void show_plan(int i) {
    uint32_t v = i == 0 ? s_edit.plan_down_mbps : s_edit.plan_up_mbps;
    if (s_edit.plan_set) lv_label_set_text_fmt(plan_val[i], "%lu Mbps  ›", (unsigned long)v);
    else                 lv_label_set_text(plan_val[i], tr(S_SET_UP));
    lv_obj_set_style_text_color(plan_val[i], s_edit.plan_set ? C_TEXT_2 : C_DOWN, 0);
}

static void show_alert(int i) {
    uint8_t v = i == 0 ? s_edit.alert_down_pct : s_edit.alert_up_pct;
    if (v == ALERT_OFF) lv_label_set_text(alert_val[i], tr(S_OFF));
    else                lv_label_set_text_fmt(alert_val[i], "%u %%", v);
}

static void show_lang(void) {
    for (int i = 0; i < 2; i++) {
        bool on = (int)s_edit.lang == i;
        lv_obj_set_style_bg_opa(lang_btn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t* l = lv_obj_get_child(lang_btn[i], 0);
        lv_obj_set_style_text_color(l, on ? C_TEXT : C_TEXT_2, 0);
    }
}

static void save(void) {
    settings_save(s_edit);
    s_edit = settings();
    ui_apply_settings();
}

// ============================================================================
// Events
// ============================================================================
static void done_cb(lv_event_t* e) {
    (void)e;
    ui_show_dashboard();
    // The dashboard is the active screen now; drop this one on the next tick.
    lv_obj_delete_async(scr);
    scr = NULL;
}

static void alert_slider_cb(lv_event_t* e) {
    lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t v = lv_slider_get_value(s);
    v = (v + ALERT_STEP / 2) / ALERT_STEP * ALERT_STEP;
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    uint8_t pct = v >= ALERT_OFF_POS ? ALERT_OFF : (uint8_t)v;
    if (i == 0) s_edit.alert_down_pct = pct; else s_edit.alert_up_pct = pct;
    show_alert(i);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) save();
}

static void bright_slider_cb(lv_event_t* e) {
    lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
    s_edit.brightness = (uint8_t)lv_slider_get_value(s);
    lv_label_set_text_fmt(bright_val, "%u %%", s_edit.brightness);
    board_set_backlight(s_edit.brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) save();
}

static void rebuild_async(void* arg) {
    (void)arg;
    ui_settings_open();
}

static void lang_cb(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if ((int)s_edit.lang == i) return;
    s_edit.lang = (Lang)i;
    save();
    // Every label on this screen is in the old language: rebuild it, after
    // this event returns (the button handling it is about to be deleted).
    lv_async_call(rebuild_async, NULL);
}

// ---- Keypad ----
static void kp_refresh(void) {
    lv_label_set_text(kp_value, kp_buf[0] ? kp_buf : "0");
    lv_obj_set_style_text_color(kp_value, kp_fresh ? C_TEXT_2 : C_TEXT, 0);
    ui_align_baseline_right(kp_unit, kp_value, 8);
    bool valid = atol(kp_buf) >= PLAN_MIN_MBPS;
    lv_obj_set_style_bg_opa(kp_ok, valid ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void kp_close(void) {
    if (kp_overlay) lv_obj_delete_async(kp_overlay);
    kp_overlay = NULL;
}

static void kp_key_cb(lv_event_t* e) {
    const char* key = (const char*)lv_event_get_user_data(e);
    size_t len = strlen(kp_buf);
    if (strcmp(key, "⌫") == 0) {
        if (kp_fresh) { kp_buf[0] = '\0'; kp_fresh = false; }
        else if (len) kp_buf[len - 1] = '\0';
    } else if (strcmp(key, "OK") == 0) {
        long v = atol(kp_buf);
        if (v < PLAN_MIN_MBPS) return;
        if (v > PLAN_MAX_MBPS) v = PLAN_MAX_MBPS;
        if (kp_down) s_edit.plan_down_mbps = v; else s_edit.plan_up_mbps = v;
        // Entering one direction counts as setting the plan; mirror it to the
        // other on first setup, since most home plans are symmetric or the
        // user will fix the other one next.
        if (!s_edit.plan_set) {
            s_edit.plan_down_mbps = s_edit.plan_up_mbps = v;
            s_edit.plan_set = true;
        }
        save();
        show_plan(0);
        show_plan(1);
        kp_close();
        return;
    } else {
        if (kp_fresh) { kp_buf[0] = '\0'; len = 0; kp_fresh = false; }
        if (len == 0 && key[0] == '0') return;          // no leading zeros
        if (len < 6) { kp_buf[len] = key[0]; kp_buf[len + 1] = '\0'; }
    }
    kp_refresh();
}

static void kp_cancel_cb(lv_event_t* e) { (void)e; kp_close(); }

static void open_keypad(bool down) {
    kp_down = down;
    uint32_t cur = down ? s_edit.plan_down_mbps : s_edit.plan_up_mbps;
    snprintf(kp_buf, sizeof(kp_buf), "%lu", (unsigned long)(s_edit.plan_set ? cur : 0));
    if (!s_edit.plan_set) kp_buf[0] = '\0';
    kp_fresh = s_edit.plan_set;

    kp_overlay = ui_box(scr);
    lv_obj_add_flag(kp_overlay, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind the dialog
    lv_obj_set_size(kp_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(kp_overlay, C_BG, 0);
    lv_obj_set_style_bg_opa(kp_overlay, LV_OPA_80, 0);

    lv_obj_t* dlg = ui_box(kp_overlay);
    lv_obj_set_size(dlg, 340, 398);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dlg, C_CARD, 0);
    lv_obj_set_style_border_width(dlg, 1, 0);
    lv_obj_set_style_border_color(dlg, C_BORDER, 0);
    lv_obj_set_style_radius(dlg, 14, 0);

    lv_obj_t* title = ui_label(dlg, &font_inter_md_16, C_TEXT, tr(down ? S_KP_DOWN : S_KP_UP));
    lv_obj_set_pos(title, 18, 18);
    lv_obj_t* cancel = ui_box(dlg);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cancel, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(cancel, 8, 0);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(cancel, kp_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cl = ui_label(cancel, &font_inter_md_14, C_DOWN, tr(S_CANCEL));
    lv_obj_center(cl);

    kp_value = ui_label(dlg, &font_inter_num_48, C_TEXT, "");
    lv_obj_set_pos(kp_value, 18, 52);
    kp_unit = ui_label(dlg, &font_inter_md_16, C_TEXT_2, "Mbps");

    static const char* const keys[12] = {"1", "2", "3", "4", "5", "6",
                                         "7", "8", "9", "⌫", "0", "OK"};
    const int kw = 96, kh = 60, gap = 8, x0 = 18, y0 = 112;
    for (int i = 0; i < 12; i++) {
        lv_obj_t* k = ui_box(dlg);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(k, kw, kh);
        lv_obj_set_pos(k, x0 + (i % 3) * (kw + gap), y0 + (i / 3) * (kh + gap));
        lv_obj_set_style_radius(k, 10, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        bool ok = i == 11;
        lv_obj_set_style_bg_color(k, ok ? C_DOWN : C_CARD_HI, 0);
        lv_obj_set_style_bg_color(k, ok ? lv_color_mix(C_DOWN, C_BG, 200) : C_BORDER,
                                  LV_STATE_PRESSED);
        lv_obj_add_event_cb(k, kp_key_cb, LV_EVENT_CLICKED, (void*)keys[i]);
        lv_obj_t* l = ui_label(k, &font_inter_md_20, C_TEXT, keys[i]);
        lv_obj_center(l);
        if (ok) kp_ok = k;
    }
    kp_refresh();
}

void ui_settings_open_keypad(bool down) {
    if (!scr || lv_screen_active() != scr) ui_settings_open();
    open_keypad(down);
}

static void plan_row_cb(lv_event_t* e) {
    open_keypad((int)(intptr_t)lv_event_get_user_data(e) == 0);
}

// ============================================================================
// Build
// ============================================================================
void ui_settings_open(void) {
    s_edit = settings();
    lv_obj_t* old = scr;
    kp_overlay = NULL;

    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ---- Header ----
    lv_obj_t* title = ui_label(scr, &font_inter_sb_20, C_TEXT, tr(S_SETTINGS));
    lv_obj_set_pos(title, PAD, 15);
    lv_obj_t* done = ui_box(scr);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(done, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(done, 12, 0);
    lv_obj_set_style_radius(done, 22, 0);
    lv_obj_set_style_bg_color(done, C_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -(PAD - 12), 6);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* done_l = ui_label(done, &font_inter_md_16, C_DOWN, tr(S_DONE));
    lv_obj_center(done_l);

    lv_obj_t* rule = ui_box(scr);
    lv_obj_set_pos(rule, 0, HEADER_H - 1);
    lv_obj_set_size(rule, LCD_WIDTH, 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, C_BORDER, 0);

    // ---- Scrollable body ----
    lv_obj_t* body = ui_box(scr);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(body, 0, HEADER_H);
    lv_obj_set_size(body, LCD_WIDTH, LCD_HEIGHT - HEADER_H);
    lv_obj_set_style_pad_all(body, PAD, 0);
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);

    const int ctrl_x = 112;                                    // sliders start here
    const int ctrl_w = LCD_WIDTH - 2 * PAD - 28 - ctrl_x - 64; // leave room for the value

    // Plan
    lv_obj_t* c = section(body, S_SEC_PLAN);
    for (int i = 0; i < 2; i++) {
        lv_obj_t* r = row(c, i == 1);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, C_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, plan_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        row_label(r, i == 0, i == 0 ? S_PLAN_DOWN : S_PLAN_UP);
        plan_val[i] = ui_label(r, &font_inter_md_14, C_TEXT_2, "");
        lv_obj_align(plan_val[i], LV_ALIGN_RIGHT_MID, 0, 0);
        show_plan(i);
    }
    lv_obj_t* gap1 = ui_box(body);
    lv_obj_set_size(gap1, 1, 6);

    // Alerts
    c = section(body, S_SEC_ALERTS);
    for (int i = 0; i < 2; i++) {
        lv_obj_t* r = row(c, i == 1);
        row_label(r, i == 0, i == 0 ? S_ALERT_DOWN : S_ALERT_UP);
        uint8_t v = i == 0 ? s_edit.alert_down_pct : s_edit.alert_up_pct;
        lv_obj_t* s = slider(r, ALERT_MIN_PCT, ALERT_OFF_POS, v == ALERT_OFF ? ALERT_OFF_POS : v,
                             ctrl_x, ctrl_w);
        lv_obj_add_event_cb(s, alert_slider_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(s, alert_slider_cb, LV_EVENT_RELEASED, (void*)(intptr_t)i);
        alert_val[i] = value_label(r);
        show_alert(i);
    }
    lv_obj_t* gap2 = ui_box(body);
    lv_obj_set_size(gap2, 1, 6);

    // Display
    c = section(body, S_SEC_DISPLAY);
    lv_obj_t* r = row(c, false);
    lv_obj_t* bl = ui_label(r, &font_inter_md_16, C_TEXT, tr(S_BRIGHTNESS));
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* bs = slider(r, BRIGHTNESS_MIN, 100, s_edit.brightness, ctrl_x, ctrl_w);
    lv_obj_add_event_cb(bs, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bs, bright_slider_cb, LV_EVENT_RELEASED, NULL);
    bright_val = value_label(r);
    lv_label_set_text_fmt(bright_val, "%u %%", s_edit.brightness);

    r = row(c, true);
    lv_obj_t* ll = ui_label(r, &font_inter_md_16, C_TEXT, tr(S_LANGUAGE));
    lv_obj_align(ll, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* seg = ui_box(r);
    lv_obj_set_size(seg, 196, 34);
    lv_obj_align(seg, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(seg, 10, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(seg, C_BG, 0);
    lv_obj_set_style_pad_all(seg, 3, 0);
    static const char* const langs[2] = {"Español", "English"};
    for (int i = 0; i < 2; i++) {
        lang_btn[i] = ui_box(seg);
        lv_obj_add_flag(lang_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(lang_btn[i], 95, 28);
        lv_obj_set_pos(lang_btn[i], i * 95, 0);
        lv_obj_set_style_radius(lang_btn[i], 8, 0);
        lv_obj_set_style_bg_color(lang_btn[i], C_BORDER, 0);
        lv_obj_add_event_cb(lang_btn[i], lang_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* l = ui_label(lang_btn[i], &font_inter_md_14, C_TEXT_2, langs[i]);
        lv_obj_center(l);
    }
    show_lang();

    lv_obj_t* ver = ui_label(body, &font_inter_12, C_TEXT_3, "");
    lv_label_set_text_fmt(ver, "NetMeter %s  ·  %s", FW_VERSION, REPO_URL + 8);
    lv_obj_set_width(ver, lv_pct(100));
    lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(ver, 8, 0);

    lv_screen_load(scr);
    if (old) lv_obj_delete_async(old);
}
