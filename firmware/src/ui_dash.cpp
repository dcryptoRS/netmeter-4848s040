#include "ui.h"
#include "theme.h"
#include "i18n.h"
#include "settings.h"
#include "alerts.h"
#include "board/board.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

// Dashboard: live download/upload against the contracted plan, a throughput
// history chart, latency and totals.
//
// The chart is real history. Every sample from the PC (~2 Hz) lands in a
// one-slot-per-second ring covering 30 minutes, and the chart is redrawn from
// it a few times a second at most. It is rasterised by hand into an RGB565
// canvas because LVGL has no area chart, and to keep it crisp on a 16-bit
// panel:
//   * lines are anti-aliased from true distance-to-segment coverage, gathered
//     in a coverage buffer first so overlapping segments never double-blend;
//   * fills are flat and translucent. A vertical gradient quantises into
//     visible bands at 16 bits per pixel, and LVGL 9 no longer dithers.

// ---- Layout (px), 480x480 ----
#define PAD         18
#define HEADER_H    56
#define HERO_TOP    74
#define HERO_H      112
#define FOOTER_H    40
#define CARD_PAD    14
#define CARD_HEAD   42      // chart card title row
#define AXIS_W      66      // y-axis label gutter, left of the plot
#define PLOT_RM     7       // right margin so the "now" dot is never clipped
#define PLOT_TOP    8.5f    // y of the top gridline (pixel centre)
#define PLOT_BOT    5.5f    // zero line sits this far above the canvas bottom
#define LINE_HW     1.0f    // stroke half-width -> 2 px lines
#define BANNER_MS   10000

// ---- History ----
#define HIST_LEN    1800    // 30 min, one slot per second
#define CARRY_SEC   3       // bridge sample jitter; break the line on longer gaps
#define MIN_SCALE   1e6f    // 1 Mbps floor, so idle chatter stays a ripple
#define MAX_PTS     512     // >= plot width

static const uint16_t kRangeSec[]  = {120, 600, 1800};
static const char*    kRangeName[] = {"2 min", "10 min", "30 min"};
#define RANGE_COUNT 3

// ============================================================================
// State
// ============================================================================
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

static lv_obj_t* scr = NULL;
static lv_obj_t* lbl_title = NULL;
static lv_obj_t* lbl_src = NULL;
static lv_obj_t* lbl_clock = NULL;
static lv_obj_t* pill = NULL;
static lv_obj_t* pill_dot = NULL;
static lv_obj_t* pill_txt = NULL;

struct DirUi {
    lv_obj_t* name;
    lv_obj_t* val;
    lv_obj_t* unit;
    lv_obj_t* pct;
    lv_obj_t* bar;
    lv_obj_t* mark;      // alert threshold tick on the bar
    lv_obj_t* peak;
    bool      down;
};
static lv_obj_t* hero = NULL;
static DirUi s_dl = {}, s_ul = {};

static lv_obj_t* card = NULL;
static lv_obj_t* lbl_traffic = NULL;
static lv_obj_t* range_chip = NULL;
static lv_obj_t* range_txt = NULL;
static lv_obj_t* leg_dl = NULL;
static lv_obj_t* leg_ul = NULL;
static lv_obj_t* canvas = NULL;
static lv_obj_t* lbl_axis[3] = {};      // top, middle, zero
static lv_obj_t* lbl_empty = NULL;
static lv_obj_t* onboard = NULL;        // "install the PC app" panel
static lv_obj_t* onboard_title = NULL;
static lv_obj_t* onboard_body = NULL;

static lv_obj_t* footer = NULL;
static lv_obj_t* ft_key[4] = {};
static lv_obj_t* ft_ping = NULL;
static lv_obj_t* ft_rx = NULL;
static lv_obj_t* ft_tx = NULL;
static lv_obj_t* ft_plan = NULL;

static lv_obj_t* banner = NULL;
static lv_obj_t* banner_txt = NULL;
static uint32_t  s_banner_until = 0;

static uint16_t* s_cv_buf = NULL;       // RGB565 canvas, sized for the expanded chart
static uint8_t*  s_cov = NULL;          // stroke coverage scratch, same size
static int s_cv_w = 0, s_cv_h = 0, s_cv_h_max = 0;

static uint16_t c_card, c_grid, c_axis, c_down, c_up;

static bool     s_expanded = false;
static uint8_t  s_range = 0;
static bool     s_online = false;       // last state shown
static float    s_dl_bits = 0, s_ul_bits = 0;
static NetSample s_last = {};
static bool     s_dirty = true;
static uint32_t s_last_render_ms = 0;
static int      s_last_clock_min = -1;
static AlertState s_alert_dl = {}, s_alert_ul = {};

// History ring: running mean of the samples received within each second.
static float    s_h_dl[HIST_LEN];
static float    s_h_ul[HIST_LEN];
static uint32_t s_h_sec[HIST_LEN];      // which second the slot holds; 0 = never
static uint8_t  s_h_n[HIST_LEN];

static float s_sec_dl[HIST_LEN], s_sec_ul[HIST_LEN];
static float s_px[MAX_PTS], s_py_dl[MAX_PTS], s_py_ul[MAX_PTS];

static inline uint32_t now_sec(void) { return millis() / 1000 + 1; }   // never 0

static float plan_bits(bool down) {
    const Settings& st = settings();
    return (down ? st.plan_down_mbps : st.plan_up_mbps) * 1e6f;
}

static uint8_t alert_pct(bool down) {
    const Settings& st = settings();
    if (!st.plan_set) return ALERT_OFF;   // a % of an unknown plan means nothing
    return down ? st.alert_down_pct : st.alert_up_pct;
}

// ============================================================================
// Formatting
// ============================================================================
static void fmt_axis(float bits, char* out, size_t len) {
    if (bits <= 0.0f) { snprintf(out, len, "0"); return; }
    static const char* const units[] = {"Kbps", "Mbps", "Gbps"};
    float v = bits / 1000.0f;
    int u = 0;
    while (v >= 1000.0f && u < 2) { v /= 1000.0f; u++; }
    if (fabsf(v - roundf(v)) < 0.01f) snprintf(out, len, "%.0f %s", v, units[u]);
    else                              snprintf(out, len, "%.1f %s", v, units[u]);
}

// Smallest 1/2/5 x 10^n >= v.
static float nice_ceil(float v) {
    float e = powf(10.0f, floorf(log10f(v)));
    float f = v / e;
    float n = f <= 1.0f ? 1.0f : f <= 2.0f ? 2.0f : f <= 5.0f ? 5.0f : 10.0f;
    return n * e;
}

static void fmt_total(float mb, char* out, size_t len) {
    if (mb < 0.0f)            snprintf(out, len, "—");
    else if (mb < 1000.0f)    snprintf(out, len, "%.0f MB", mb);
    else if (mb < 1000000.0f) snprintf(out, len, "%.1f GB", mb / 1000.0f);
    else                      snprintf(out, len, "%.2f TB", mb / 1000000.0f);
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a) {
    if (a == 0) return bg;
    if (a >= 255) return fg;
    uint32_t inv = 255 - a;
    uint32_t r = ((bg >> 11) * inv + (fg >> 11) * a + 127) / 255;
    uint32_t g = (((bg >> 5) & 0x3F) * inv + ((fg >> 5) & 0x3F) * a + 127) / 255;
    uint32_t b = ((bg & 0x1F) * inv + (fg & 0x1F) * a + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// ============================================================================
// History
// ============================================================================
static void hist_add(uint32_t sec, float dl, float ul) {
    uint32_t i = sec % HIST_LEN;
    if (s_h_sec[i] != sec) {
        s_h_sec[i] = sec;
        s_h_n[i] = 0;
        s_h_dl[i] = 0.0f;
        s_h_ul[i] = 0.0f;
    }
    if (s_h_n[i] < 255) s_h_n[i]++;
    s_h_dl[i] += (dl - s_h_dl[i]) / s_h_n[i];
    s_h_ul[i] += (ul - s_h_ul[i]) / s_h_n[i];
}

// ============================================================================
// Rasteriser
// ============================================================================
static void hline_dashed(int row, uint16_t col, int on, int off, uint32_t alpha) {
    if (row < 0 || row >= s_cv_h) return;
    uint16_t* p = s_cv_buf + row * s_cv_w;
    for (int x = 0; x < s_cv_w - PLOT_RM; x++)
        if ((x % (on + off)) < on) p[x] = blend565(p[x], col, alpha);
}

// Flat translucent fill between the polyline and the zero line. The top pixel
// of each column gets fractional coverage so the fill edge is as smooth as the
// stroke that sits on it.
static void fill_area(const float* ys, int n, uint16_t col, uint32_t alpha, float ybase) {
    int seg = 0;
    int zero_row = (int)ybase;
    for (int x = 0; x < s_cv_w; x++) {
        float xc = x + 0.5f;
        if (xc < s_px[0] || xc > s_px[n - 1]) continue;
        while (seg < n - 2 && s_px[seg + 1] < xc) seg++;
        float y0 = ys[seg], y1 = ys[seg + 1];
        if (isnan(y0) || isnan(y1)) continue;
        float x0 = s_px[seg], x1 = s_px[seg + 1];
        float t = x1 > x0 ? (xc - x0) / (x1 - x0) : 0.0f;
        float y = y0 + (y1 - y0) * t;
        int r0 = (int)y;
        for (int r = r0 < 0 ? 0 : r0; r < zero_row; r++) {
            float cov = (r == r0) ? (r + 1 - y) : 1.0f;
            uint16_t* p = s_cv_buf + r * s_cv_w + x;
            *p = blend565(*p, col, (uint32_t)(alpha * cov + 0.5f));
        }
    }
}

static inline float seg_dist(float px, float py, float x0, float y0, float x1, float y1) {
    float vx = x1 - x0, vy = y1 - y0;
    float wx = px - x0, wy = py - y0;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0.0f ? (wx * vx + wy * vy) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

static void cov_segment(float x0, float y0, float x1, float y1, float hw) {
    int minx = (int)floorf(fminf(x0, x1) - hw - 1.0f);
    int maxx = (int)ceilf(fmaxf(x0, x1) + hw + 1.0f);
    int miny = (int)floorf(fminf(y0, y1) - hw - 1.0f);
    int maxy = (int)ceilf(fmaxf(y0, y1) + hw + 1.0f);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > s_cv_w - 1) maxx = s_cv_w - 1;
    if (maxy > s_cv_h - 1) maxy = s_cv_h - 1;
    for (int y = miny; y <= maxy; y++) {
        uint8_t* row = s_cov + y * s_cv_w;
        for (int x = minx; x <= maxx; x++) {
            float c = hw + 0.5f - seg_dist(x + 0.5f, y + 0.5f, x0, y0, x1, y1);
            if (c <= 0.0f) continue;
            uint8_t a = c >= 1.0f ? 255 : (uint8_t)(c * 255.0f);
            if (a > row[x]) row[x] = a;
        }
    }
}

static void stroke_series(const float* ys, int n, uint16_t col) {
    memset(s_cov, 0, (size_t)s_cv_w * s_cv_h);
    for (int i = 0; i < n; i++) {
        if (isnan(ys[i])) continue;
        bool next = i + 1 < n && !isnan(ys[i + 1]);
        bool prev = i > 0 && !isnan(ys[i - 1]);
        if (next)       cov_segment(s_px[i], ys[i], s_px[i + 1], ys[i + 1], LINE_HW);
        else if (!prev) cov_segment(s_px[i], ys[i], s_px[i], ys[i], LINE_HW);  // lone sample
    }
    const int total = s_cv_w * s_cv_h;
    for (int i = 0; i < total; i++)
        if (s_cov[i]) s_cv_buf[i] = blend565(s_cv_buf[i], col, s_cov[i]);
}

static void disk(float cx, float cy, float r, uint16_t col) {
    int minx = (int)floorf(cx - r - 1), maxx = (int)ceilf(cx + r + 1);
    int miny = (int)floorf(cy - r - 1), maxy = (int)ceilf(cy + r + 1);
    for (int y = miny; y <= maxy; y++) {
        if (y < 0 || y >= s_cv_h) continue;
        for (int x = minx; x <= maxx; x++) {
            if (x < 0 || x >= s_cv_w) continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float c = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (c <= 0.0f) continue;
            uint16_t* p = s_cv_buf + y * s_cv_w + x;
            *p = blend565(*p, col, c >= 1.0f ? 255 : (uint32_t)(c * 255.0f));
        }
    }
}

static void set_peak(DirUi& d, float bits, bool any) {
    char num[16], buf[40];
    if (!any) {
        snprintf(buf, sizeof(buf), "%s  —", tr(S_PEAK));
    } else {
        const char* unit = ui_fmt_rate(bits, num, sizeof(num));
        snprintf(buf, sizeof(buf), "%s  %s %s", tr(S_PEAK), num, unit);
    }
    lv_label_set_text(d.peak, buf);
}

static void render_chart(void) {
    s_last_render_ms = millis();
    s_dirty = false;
    if (!s_cv_buf || !s_cov || !canvas) return;

    const int CW = s_cv_w, CH = s_cv_h;
    const int N = kRangeSec[s_range];
    const int plot_w = CW - PLOT_RM;
    const uint32_t now = now_sec();

    // 1. Per-second series for the window. Gaps up to CARRY_SEC hold the last
    //    value (2 Hz samples jitter across second boundaries); anything longer
    //    is a real outage and breaks the line.
    float last_dl = NAN, last_ul = NAN;
    int stale = CARRY_SEC + 1;
    float peak_dl = 0.0f, peak_ul = 0.0f;
    bool any = false;
    for (int k = 0; k < N; k++) {
        int32_t t = (int32_t)now - (N - 1) + k;
        if (t > 0 && s_h_sec[t % HIST_LEN] == (uint32_t)t) {
            last_dl = s_h_dl[t % HIST_LEN];
            last_ul = s_h_ul[t % HIST_LEN];
            stale = 0;
        } else {
            stale++;
        }
        bool ok = stale <= CARRY_SEC && !isnan(last_dl);
        s_sec_dl[k] = ok ? last_dl : NAN;
        s_sec_ul[k] = ok ? last_ul : NAN;
        if (ok) {
            any = true;
            if (last_dl > peak_dl) peak_dl = last_dl;
            if (last_ul > peak_ul) peak_ul = last_ul;
        }
    }

    // 2. At most one point per column (max, so spikes survive down-sampling).
    int n;
    if (N <= plot_w) {
        n = N;
        for (int k = 0; k < N; k++) {
            s_px[k] = 0.5f + (float)k * (plot_w - 1) / (N - 1);
            s_py_dl[k] = s_sec_dl[k];
            s_py_ul[k] = s_sec_ul[k];
        }
    } else {
        n = plot_w;
        for (int c = 0; c < plot_w; c++) {
            int k0 = c * N / plot_w, k1 = (c + 1) * N / plot_w;
            float md = NAN, mu = NAN;
            for (int k = k0; k < k1; k++) {
                if (isnan(s_sec_dl[k])) continue;
                if (isnan(md) || s_sec_dl[k] > md) md = s_sec_dl[k];
                if (isnan(mu) || s_sec_ul[k] > mu) mu = s_sec_ul[k];
            }
            s_px[c] = c + 0.5f;
            s_py_dl[c] = md;
            s_py_ul[c] = mu;
        }
    }

    // 3. Scale and value -> y.
    float scale = nice_ceil(fmaxf(fmaxf(peak_dl, peak_ul) * 1.1f, MIN_SCALE));
    const float ybase = CH - PLOT_BOT;
    const float span = ybase - PLOT_TOP;
    for (int i = 0; i < n; i++) {
        if (!isnan(s_py_dl[i])) s_py_dl[i] = ybase - fminf(s_py_dl[i] / scale, 1.0f) * span;
        if (!isnan(s_py_ul[i])) s_py_ul[i] = ybase - fminf(s_py_ul[i] / scale, 1.0f) * span;
    }

    // 4. Paint: background, grid, alert thresholds, fills, zero line, strokes,
    //    "now" dots.
    for (int i = 0; i < CW * CH; i++) s_cv_buf[i] = c_card;
    const int row_top = (int)PLOT_TOP;
    const int row_mid = (int)((PLOT_TOP + ybase) * 0.5f);
    const int row_zero = (int)ybase;
    hline_dashed(row_top, c_grid, 3, 3, 255);
    hline_dashed(row_mid, c_grid, 3, 3, 255);

    // Alert thresholds, drawn only when they fall inside the current scale.
    const bool dirs[2] = {true, false};
    for (bool down : dirs) {
        uint8_t pct = alert_pct(down);
        if (pct == ALERT_OFF) continue;
        float thr = plan_bits(down) * pct / 100.0f;
        if (thr > scale) continue;
        int row = (int)(ybase - thr / scale * span);
        hline_dashed(row, down ? c_down : c_up, 8, 5, 150);
    }

    fill_area(s_py_ul, n, c_up, 40, ybase);
    fill_area(s_py_dl, n, c_down, 56, ybase);
    for (int x = 0; x < CW - PLOT_RM; x++) s_cv_buf[row_zero * CW + x] = c_axis;

    stroke_series(s_py_ul, n, c_up);
    stroke_series(s_py_dl, n, c_down);

    if (!isnan(s_py_ul[n - 1])) {
        disk(s_px[n - 1], s_py_ul[n - 1], 4.5f, c_card);
        disk(s_px[n - 1], s_py_ul[n - 1], 2.5f, c_up);
    }
    if (!isnan(s_py_dl[n - 1])) {
        disk(s_px[n - 1], s_py_dl[n - 1], 4.5f, c_card);
        disk(s_px[n - 1], s_py_dl[n - 1], 2.5f, c_down);
    }
    lv_obj_invalidate(canvas);

    // 5. Labels that depend on the window.
    char buf[24];
    const int rows[3] = {row_top, row_mid, row_zero};
    const float vals[3] = {scale, scale * 0.5f, 0.0f};
    for (int i = 0; i < 3; i++) {
        fmt_axis(vals[i], buf, sizeof(buf));
        lv_label_set_text(lbl_axis[i], buf);
        lv_obj_set_y(lbl_axis[i], CARD_HEAD + rows[i] - 8);
    }
    bool show_empty = !any && link_ever_seen();
    if (show_empty) lv_obj_clear_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    set_peak(s_dl, peak_dl, any);
    set_peak(s_ul, peak_ul, any);
}

// ============================================================================
// Readouts
// ============================================================================
static void refresh_legend(void) {
    if (!s_expanded || !link_alive()) {
        lv_label_set_text(leg_dl, tr(S_DOWNLOAD));
        lv_label_set_text(leg_ul, tr(S_UPLOAD));
        return;
    }
    // Expanded mode hides the big readouts, so the legend carries the live
    // values instead of the names (the dot colour already says which is which).
    char num[16], buf[40];
    const char* u = ui_fmt_rate(s_dl_bits, num, sizeof(num));
    snprintf(buf, sizeof(buf), "↓ %s %s", num, u);
    lv_label_set_text(leg_dl, buf);
    u = ui_fmt_rate(s_ul_bits, num, sizeof(num));
    snprintf(buf, sizeof(buf), "↑ %s %s", num, u);
    lv_label_set_text(leg_ul, buf);
}

static void refresh_status(void) {
    bool alive = link_alive();
    char src_err = link_source_error();
    lv_color_t col;
    char txt[40];
    if (!link_ever_seen()) {
        col = C_TEXT_3;
        snprintf(txt, sizeof(txt), "%s", tr(S_ST_WAITING));
    } else if (!alive) {
        col = C_RED;
        snprintf(txt, sizeof(txt), "%s", tr(S_ST_OFFLINE));
    } else if (src_err) {
        col = C_AMBER;
        snprintf(txt, sizeof(txt), "%s", tr(src_err == 'n' ? S_ST_NET_ERR : S_ST_GW_ERR));
    } else if (s_alert_dl.active || s_alert_ul.active) {
        col = C_RED;
        snprintf(txt, sizeof(txt), "%s %s%s", tr(S_ST_ALERT),
                 s_alert_dl.active ? "↓" : "", s_alert_ul.active ? "↑" : "");
    } else {
        col = C_GREEN;
        snprintf(txt, sizeof(txt), "%s", tr(S_ST_ONLINE));
    }
    lv_obj_set_style_bg_color(pill, lv_color_mix(col, C_BG, 40), 0);
    lv_obj_set_style_bg_color(pill_dot, col, 0);
    lv_obj_set_style_text_color(pill_txt, link_ever_seen() ? col : C_TEXT_2, 0);
    lv_label_set_text(pill_txt, txt);

    // Stale numbers stay readable but stop looking live.
    lv_color_t vcol = (alive && !src_err) ? C_TEXT : C_TEXT_3;
    lv_obj_set_style_text_color(s_dl.val, vcol, 0);
    lv_obj_set_style_text_color(s_ul.val, vcol, 0);
}

static void refresh_dir(DirUi& d, float bits, bool have) {
    char num[16], buf[24];
    if (!have) {
        lv_label_set_text(d.val, "—");
        lv_label_set_text(d.unit, "");
    } else {
        const char* unit = ui_fmt_rate(bits, num, sizeof(num));
        lv_label_set_text(d.val, num);
        lv_label_set_text(d.unit, unit);
    }
    ui_align_baseline_right(d.unit, d.val, 6);

    const Settings& st = settings();
    bool alert = (d.down ? s_alert_dl : s_alert_ul).active;
    lv_color_t accent = alert ? C_RED : (d.down ? C_DOWN : C_UP);
    lv_obj_set_style_bg_color(d.bar, accent, LV_PART_INDICATOR);

    if (!st.plan_set || !have) {
        lv_label_set_text(d.pct, "");
        lv_bar_set_value(d.bar, 0, LV_ANIM_OFF);
    } else {
        float pct = bits / plan_bits(d.down) * 100.0f;
        if (pct > 999.0f) pct = 999.0f;
        snprintf(buf, sizeof(buf), pct < 9.95f ? "%s%.1f%%" : "%s%.0f%%",
                 alert ? "⚠ " : "", pct);
        lv_label_set_text(d.pct, buf);
        lv_bar_set_value(d.bar, (int32_t)(fminf(pct, 100.0f) * 10.0f + 0.5f), LV_ANIM_ON);
    }
    lv_obj_set_style_text_color(d.pct, alert ? C_RED : C_TEXT_3, 0);

    uint8_t thr = alert_pct(d.down);
    if (thr == ALERT_OFF) {
        lv_obj_add_flag(d.mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(d.mark, LV_OBJ_FLAG_HIDDEN);
        int32_t bw = lv_obj_get_width(d.bar);
        lv_obj_set_x(d.mark, lv_obj_get_x(d.bar) + bw * thr / 100 - 1);
    }
}

static void refresh_footer(void) {
    const Settings& st = settings();
    char buf[24];
    if (!st.plan_set) {
        lv_label_set_text(ft_plan, tr(S_SET_UP));
        lv_obj_set_style_text_color(ft_plan, C_DOWN, 0);
    } else {
        if (st.plan_down_mbps == st.plan_up_mbps)
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.plan_down_mbps);
        else
            snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)st.plan_down_mbps,
                     (unsigned long)st.plan_up_mbps);
        lv_label_set_text(ft_plan, buf);
        lv_obj_set_style_text_color(ft_plan, C_TEXT, 0);
    }
    if (!link_ever_seen()) return;

    if (s_last.ping_ms >= 0) {
        lv_label_set_text_fmt(ft_ping, "%d ms", s_last.ping_ms);
        lv_obj_set_style_text_color(ft_ping,
            s_last.ping_ms >= 150 ? C_RED : s_last.ping_ms >= 60 ? C_AMBER : C_TEXT, 0);
    } else {
        lv_label_set_text(ft_ping, "—");
    }
    fmt_total(s_last.rx_mb, buf, sizeof(buf));
    lv_label_set_text(ft_rx, buf);
    fmt_total(s_last.tx_mb, buf, sizeof(buf));
    lv_label_set_text(ft_tx, buf);
}

static void refresh_onboarding(void) {
    if (link_ever_seen()) {
        lv_obj_add_flag(onboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* l : lbl_axis) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(onboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* l : lbl_axis) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_banner(bool down) {
    char num[16], buf[96];
    float bits = down ? s_dl_bits : s_ul_bits;
    const char* unit = ui_fmt_rate(bits, num, sizeof(num));
    int pct = (int)(bits / plan_bits(down) * 100.0f + 0.5f);
    snprintf(buf, sizeof(buf), "⚠  %s · %s %s (%d %% %s)",
             tr(down ? S_BANNER_DOWN : S_BANNER_UP), num, unit, pct, tr(S_OF_PLAN));
    lv_label_set_text(banner_txt, buf);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner);
    s_banner_until = millis() + BANNER_MS;
}

static void handle_alert_event(bool down, AlertEvent ev) {
    if (ev == ALERT_FIRED) {
        float bits = down ? s_dl_bits : s_ul_bits;
        link_send_alert(down, (int)(bits / plan_bits(down) * 100.0f + 0.5f), bits);
        show_banner(down);
    } else if (ev == ALERT_CLEARED) {
        link_send_clear(down);
    }
}

// ============================================================================
// Build
// ============================================================================
static void build_dir(DirUi& d, lv_obj_t* parent, int x, int w, bool down) {
    d.down = down;
    lv_obj_t* col = ui_box(parent);
    lv_obj_set_pos(col, x, 0);
    lv_obj_set_size(col, w, HERO_H);
    lv_obj_add_flag(col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   // the -2 px nudge below

    lv_color_t accent = down ? C_DOWN : C_UP;
    lv_obj_t* arrow = ui_label(col, &font_inter_md_14, accent, down ? "↓" : "↑");
    lv_obj_set_pos(arrow, 0, 0);
    d.name = ui_label(col, &font_inter_md_14, C_TEXT_2, "");
    lv_obj_align_to(d.name, arrow, LV_ALIGN_OUT_RIGHT_TOP, 5, 0);

    d.pct = ui_label(col, &font_inter_14, C_TEXT_3, "");
    lv_obj_align(d.pct, LV_ALIGN_TOP_RIGHT, 0, 0);

    d.val = ui_label(col, &font_inter_num_48, C_TEXT, "—");
    lv_obj_set_pos(d.val, -2, 24);   // cancel the 48 px side bearing so digits line up with the arrow
    d.unit = ui_label(col, &font_inter_md_16, C_TEXT_2, "");
    ui_align_baseline_right(d.unit, d.val, 6);

    d.bar = lv_bar_create(col);
    lv_obj_remove_style_all(d.bar);
    lv_obj_set_pos(d.bar, 0, 78);
    lv_obj_set_size(d.bar, w, 4);
    lv_bar_set_range(d.bar, 0, 1000);
    lv_obj_set_style_bg_opa(d.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d.bar, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(d.bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(d.bar, accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(d.bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(d.bar, 400, LV_PART_MAIN);

    // Threshold tick: shows where on the bar the alert will fire.
    d.mark = ui_box(col);
    lv_obj_set_size(d.mark, 2, 10);
    lv_obj_set_y(d.mark, 75);
    lv_obj_set_style_bg_opa(d.mark, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d.mark, C_TEXT_2, 0);
    lv_obj_set_style_radius(d.mark, 1, 0);

    d.peak = ui_label(col, &font_inter_14, C_TEXT_3, "");
    lv_obj_set_pos(d.peak, 0, 92);
}

static lv_obj_t* build_stat(lv_obj_t* parent, int x, lv_obj_t** key) {
    *key = ui_label(parent, &font_inter_14, C_TEXT_3, "");
    lv_obj_set_pos(*key, x, 0);
    lv_obj_t* v = ui_label(parent, &font_inter_md_16, C_TEXT, "—");
    lv_obj_set_pos(v, x, 20);
    return v;
}

static lv_obj_t* build_legend_item(lv_obj_t* parent, lv_color_t col) {
    lv_obj_t* item = ui_box(parent);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 6, 0);
    lv_obj_t* dot = ui_box(item);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, col, 0);
    return ui_label(item, &font_inter_14, C_TEXT_2, "");
}

static void card_clicked_cb(lv_event_t* e) { (void)e; ui_toggle_expanded(); }
static void range_clicked_cb(lv_event_t* e) { (void)e; ui_cycle_range(); }
static void settings_clicked_cb(lv_event_t* e) { (void)e; ui_settings_open(); }
static void banner_clicked_cb(lv_event_t* e) {
    (void)e;
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    s_banner_until = 0;
}

static void layout_chart(void) {
    const int footer_top = H - PAD - FOOTER_H;
    const int top    = s_expanded ? HEADER_H + 12 : HERO_TOP + HERO_H + 14;
    const int bottom = s_expanded ? H - PAD       : footer_top - 16;
    lv_obj_set_pos(card, PAD, top);
    lv_obj_set_size(card, W - 2 * PAD, bottom - top);

    s_cv_h = bottom - top - CARD_HEAD - CARD_PAD;
    if (s_cv_h > s_cv_h_max) s_cv_h = s_cv_h_max;
    if (canvas) lv_canvas_set_buffer(canvas, s_cv_buf, s_cv_w, s_cv_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_y(lbl_empty, CARD_HEAD + s_cv_h / 2 - 9);
    lv_obj_set_size(onboard, W - 2 * PAD - 2 * CARD_PAD, bottom - top - CARD_HEAD - CARD_PAD);

    if (s_expanded) {
        lv_obj_add_flag(hero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(hero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(footer, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_legend();
    render_chart();   // now, so the resized canvas never shows stale rows
}

static void apply_texts(void) {
    lv_label_set_text(lbl_title, tr(S_INTERNET));
    lv_label_set_text(s_dl.name, tr(S_DOWNLOAD));
    lv_label_set_text(s_ul.name, tr(S_UPLOAD));
    lv_label_set_text(lbl_traffic, tr(S_TRAFFIC));
    lv_obj_align_to(range_chip, lbl_traffic, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_label_set_text(ft_key[0], tr(S_LATENCY));
    lv_label_set_text(ft_key[1], tr(S_RECEIVED));
    lv_label_set_text(ft_key[2], tr(S_SENT));
    lv_label_set_text(ft_key[3], tr(S_PLAN_MBPS));
    lv_label_set_text(lbl_empty, tr(S_NO_DATA));
    lv_label_set_text(onboard_title, tr(S_WAIT_TITLE));
    lv_label_set_text(onboard_body, tr(S_WAIT_BODY));
    refresh_legend();
}

static void build_onboarding(lv_obj_t* parent) {
    onboard = ui_box(parent);
    lv_obj_set_pos(onboard, CARD_PAD, CARD_HEAD);

    // Dark modules on a light field with a quiet zone: phones read that far
    // more reliably than an inverted code on a dark card.
    lv_obj_t* qr = lv_qrcode_create(onboard);
    lv_qrcode_set_size(qr, 116);
    lv_qrcode_set_dark_color(qr, C_BG);
    lv_qrcode_set_light_color(qr, C_TEXT);
    lv_qrcode_update(qr, REPO_URL, strlen(REPO_URL));
    lv_obj_set_style_border_color(qr, C_TEXT, 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_set_style_radius(qr, 4, 0);
    lv_obj_align(qr, LV_ALIGN_LEFT_MID, 0, -4);

    const int text_x = 116 + 12 + 18;
    const int text_w = W - 2 * PAD - 2 * CARD_PAD - text_x;
    onboard_title = ui_label(onboard, &font_inter_md_16, C_TEXT, "");
    lv_obj_set_width(onboard_title, text_w);
    lv_obj_align(onboard_title, LV_ALIGN_LEFT_MID, text_x, -44);
    onboard_body = ui_label(onboard, &font_inter_14, C_TEXT_2, "");
    lv_obj_set_width(onboard_body, text_w);
    lv_label_set_long_mode(onboard_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align_to(onboard_body, onboard_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_t* url = ui_label(onboard, &font_inter_12, C_DOWN, REPO_URL + 8);  // drop "https://"
    lv_obj_set_width(url, text_w);
    lv_label_set_long_mode(url, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align_to(url, onboard_body, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 52);
}

void ui_init(void) {
    c_card = lv_color_to_u16(C_CARD);
    c_grid = lv_color_to_u16(C_GRID);
    c_axis = lv_color_to_u16(C_BORDER);
    c_down = lv_color_to_u16(C_DOWN);
    c_up   = lv_color_to_u16(C_UP);

    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ---- Header: title, source, status | clock, settings ----
    lbl_title = ui_label(scr, &font_inter_sb_20, C_TEXT, "");
    lv_obj_set_pos(lbl_title, PAD, 15);
    lbl_src = ui_label(scr, &font_inter_14, C_TEXT_3, "");
    lv_label_set_long_mode(lbl_src, LV_LABEL_LONG_MODE_DOTS);

    pill = ui_box(scr);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_radius(pill, 12, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(pill, 10, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 6, 0);
    pill_dot = ui_box(pill);
    lv_obj_set_size(pill_dot, 6, 6);
    lv_obj_set_style_radius(pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pill_dot, LV_OPA_COVER, 0);
    pill_txt = ui_label(pill, &font_inter_md_14, C_TEXT_2, "");

    lv_obj_t* gear = ui_box(scr);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_set_pos(gear, W - PAD - 36, 6);
    lv_obj_set_style_radius(gear, 22, 0);
    lv_obj_set_style_bg_color(gear, C_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(gear, settings_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* gear_icon = ui_label(gear, &lv_font_montserrat_20, C_TEXT_2, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_icon);

    lbl_clock = ui_label(scr, &font_inter_md_20, C_TEXT, "");   // filled once the PC sends its clock
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -(PAD + 44), 15);

    lv_obj_t* rule = ui_box(scr);
    lv_obj_set_pos(rule, 0, HEADER_H - 1);
    lv_obj_set_size(rule, W, 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, C_BORDER, 0);

    // ---- Hero: download | upload ----
    hero = ui_box(scr);
    lv_obj_set_pos(hero, 0, HERO_TOP);
    lv_obj_set_size(hero, W, HERO_H);
    const int col_w = W / 2 - 2 * PAD;
    build_dir(s_dl, hero, PAD, col_w, true);
    build_dir(s_ul, hero, W / 2 + PAD, col_w, false);
    lv_obj_t* vrule = ui_box(hero);
    lv_obj_set_pos(vrule, W / 2, 2);
    lv_obj_set_size(vrule, 1, HERO_H - 4);
    lv_obj_set_style_bg_opa(vrule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(vrule, C_BORDER, 0);

    // ---- Chart card ----
    card = ui_box(scr);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, C_BORDER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, NULL);

    lbl_traffic = ui_label(card, &font_inter_md_14, C_TEXT, "");
    lv_obj_set_pos(lbl_traffic, CARD_PAD, 13);

    // Range chip: a visible, tappable control rather than a hidden swipe.
    range_chip = ui_box(card);
    lv_obj_add_flag(range_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(range_chip, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_pad_hor(range_chip, 10, 0);
    lv_obj_set_style_radius(range_chip, 12, 0);
    lv_obj_set_style_border_width(range_chip, 1, 0);
    lv_obj_set_style_border_color(range_chip, C_BORDER, 0);
    lv_obj_set_style_bg_color(range_chip, C_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(range_chip, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(range_chip, 10);
    lv_obj_add_event_cb(range_chip, range_clicked_cb, LV_EVENT_CLICKED, NULL);
    range_txt = ui_label(range_chip, &font_inter_12, C_TEXT_2, kRangeName[s_range]);
    lv_obj_center(range_txt);

    lv_obj_t* legend = ui_box(card);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 16, 0);
    lv_obj_align(legend, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 13);
    leg_dl = build_legend_item(legend, C_DOWN);
    leg_ul = build_legend_item(legend, C_UP);

    s_cv_w = W - 2 * PAD - 2 * CARD_PAD - AXIS_W;
    s_cv_h_max = (H - PAD) - (HEADER_H + 12) - CARD_HEAD - CARD_PAD;
    const size_t px_count = (size_t)s_cv_w * s_cv_h_max;
    s_cv_buf = (uint16_t*)heap_caps_malloc(px_count * 2, MALLOC_CAP_SPIRAM);
    s_cov    = (uint8_t*)heap_caps_malloc(px_count, MALLOC_CAP_SPIRAM);
    if (s_cv_buf && s_cov) {
        canvas = lv_canvas_create(card);
        lv_obj_set_pos(canvas, CARD_PAD + AXIS_W, CARD_HEAD);
    } else {
        Serial.println("ui: chart buffers allocation failed");
    }

    for (int i = 0; i < 3; i++) {
        lbl_axis[i] = ui_label(card, &font_inter_12, C_TEXT_3, "");
        lv_obj_set_width(lbl_axis[i], AXIS_W - 10);
        lv_obj_set_style_text_align(lbl_axis[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(lbl_axis[i], LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_x(lbl_axis[i], CARD_PAD);
    }
    lbl_empty = ui_label(card, &font_inter_14, C_TEXT_3, "");
    lv_obj_set_width(lbl_empty, s_cv_w);
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(lbl_empty, CARD_PAD + AXIS_W);
    build_onboarding(card);

    // ---- Footer stats ----
    footer = ui_box(scr);
    lv_obj_set_pos(footer, 0, H - PAD - FOOTER_H);
    lv_obj_set_size(footer, W, FOOTER_H);
    const int stat_w = (W - 2 * PAD) / 4;
    ft_ping = build_stat(footer, PAD,              &ft_key[0]);
    ft_rx   = build_stat(footer, PAD + stat_w,     &ft_key[1]);
    ft_tx   = build_stat(footer, PAD + 2 * stat_w, &ft_key[2]);
    ft_plan = build_stat(footer, PAD + 3 * stat_w, &ft_key[3]);
    lv_obj_t* plan_hit = ui_box(footer);   // tap the plan to change it
    lv_obj_add_flag(plan_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(plan_hit, PAD + 3 * stat_w - 6, 0);
    lv_obj_set_size(plan_hit, stat_w + 6, FOOTER_H);
    lv_obj_add_event_cb(plan_hit, settings_clicked_cb, LV_EVENT_CLICKED, NULL);

    // ---- Alert banner (hidden until an alert fires) ----
    // Over the footer, not the top: the readouts it is warning about (the
    // red bars and percentages) must stay visible while it shows.
    banner = ui_box(scr);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(banner, PAD, H - PAD - 46);
    lv_obj_set_size(banner, W - 2 * PAD, 44);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(banner, lv_color_mix(C_RED, C_BG, 70), 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_set_style_border_color(banner, C_RED, 0);
    lv_obj_set_style_radius(banner, 10, 0);
    lv_obj_add_event_cb(banner, banner_clicked_cb, LV_EVENT_CLICKED, NULL);
    banner_txt = ui_label(banner, &font_inter_md_14, C_TEXT, "");
    lv_obj_set_width(banner_txt, W - 2 * PAD - 28);
    lv_label_set_long_mode(banner_txt, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(banner_txt, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    apply_texts();
    refresh_dir(s_dl, 0, false);
    refresh_dir(s_ul, 0, false);
    refresh_footer();
    refresh_status();
    ui_align_baseline_right(lbl_src, lbl_title, 8);
    lv_obj_align_to(pill, lbl_title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_set_y(pill, 16);
    layout_chart();
    refresh_onboarding();

    lv_screen_load(scr);
}

// ============================================================================
// Public API
// ============================================================================
void ui_show_dashboard(void) {
    lv_screen_load(scr);
}

void ui_apply_settings(void) {
    apply_texts();
    refresh_dir(s_dl, s_dl_bits, link_ever_seen());
    refresh_dir(s_ul, s_ul_bits, link_ever_seen());
    refresh_footer();
    refresh_status();
    s_dirty = true;
}

void ui_toggle_expanded(void) {
    s_expanded = !s_expanded;
    layout_chart();
}

void ui_cycle_range(void) {
    s_range = (s_range + 1) % RANGE_COUNT;
    lv_label_set_text(range_txt, kRangeName[s_range]);
    render_chart();
}

void ui_on_sample(const NetSample& s) {
    s_last = s;
    s_dl_bits = s.down_bps;
    s_ul_bits = s.up_bps;
    hist_add(now_sec(), s_dl_bits, s_ul_bits);

    uint32_t now = millis();
    handle_alert_event(true, alert_update(&s_alert_dl,
        s_dl_bits / plan_bits(true) * 100.0f, alert_pct(true), now));
    handle_alert_event(false, alert_update(&s_alert_ul,
        s_ul_bits / plan_bits(false) * 100.0f, alert_pct(false), now));

    refresh_dir(s_dl, s_dl_bits, true);
    refresh_dir(s_ul, s_ul_bits, true);
    refresh_status();
    refresh_legend();
    refresh_footer();
    if (!lv_obj_has_flag(onboard, LV_OBJ_FLAG_HIDDEN)) refresh_onboarding();

    // Header: what is being measured, then re-anchor the pill behind it.
    if (strcmp(lv_label_get_text(lbl_src), s.src) != 0) {
        lv_label_set_text(lbl_src, s.src);
        // Gateway names are user-defined ("Cloud Gateway Ultra - Office"): keep
        // the label to one line of at most SRC_MAX_W px, ending in "…", so the
        // status pill after it can never run into the clock.
        const int SRC_MAX_W = 96;
        lv_point_t sz;
        lv_text_get_size(&sz, s.src, &font_inter_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_size(lbl_src, sz.x > SRC_MAX_W ? SRC_MAX_W : sz.x, font_inter_14.line_height);
        ui_align_baseline_right(lbl_src, lbl_title, 8);
        lv_obj_align_to(pill, lbl_src, LV_ALIGN_OUT_RIGHT_MID, s.src[0] ? 12 : 4, 0);
        lv_obj_set_y(pill, 16);
    }
    s_online = true;
    s_dirty = true;
}

void ui_tick(void) {
    uint32_t ms = millis();

    int ch, cm;
    if (link_local_time(&ch, &cm) && cm != s_last_clock_min) {
        s_last_clock_min = cm;
        lv_label_set_text_fmt(lbl_clock, "%02d:%02d", ch, cm);
    }

    // The PC app is here now (a sample or an error report): drop the
    // "install the app" panel even if no sample has arrived yet.
    if (link_ever_seen() && !lv_obj_has_flag(onboard, LV_OBJ_FLAG_HIDDEN)) refresh_onboarding();

    // Source trouble reported by the PC: show it as soon as it starts/stops.
    static char s_err_shown = 0;
    char src_err = link_source_error();
    if (src_err != s_err_shown) {
        s_err_shown = src_err;
        refresh_status();
    }

    // Link lost or no data from the source: clear alerts (a stale reading
    // can't justify one) and show it.
    bool alive = link_alive() && !src_err;
    if (s_online && !alive) {
        s_online = false;
        handle_alert_event(true, alert_reset(&s_alert_dl));
        handle_alert_event(false, alert_reset(&s_alert_ul));
        refresh_status();
        refresh_dir(s_dl, s_dl_bits, true);
        refresh_dir(s_ul, s_ul_bits, true);
        refresh_legend();
    }

    if (s_banner_until && (int32_t)(ms - s_banner_until) >= 0) {
        s_banner_until = 0;
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }

    // Redraw promptly after new data (capped at ~6 Hz), and once a second
    // regardless so an outage visibly opens a gap instead of freezing.
    if (lv_screen_active() != scr) return;
    uint32_t since = ms - s_last_render_ms;
    if ((s_dirty && since >= 150) || since >= 1000) render_chart();
}
