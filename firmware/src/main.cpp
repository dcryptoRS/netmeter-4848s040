#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "board/board.h"
#include "link.h"
#include "settings.h"
#include "ui.h"

// NetMeter: a desk display for your internet connection. The PC app measures
// traffic and streams it over USB; this firmware draws it and raises alerts
// against the plan the user configured.

#define BUF_LINES 40   // LVGL partial-render strip height (two strips, PSRAM)

static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t tick_cb(void) { return millis(); }

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    board_draw_bitmap(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                      (const uint16_t*)px);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    uint16_t x, y;
    bool pressed;
    touch_read(&x, &y, &pressed);
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// `screenshot`: dump the active screen as raw RGB565 for the PC app
// (`netmeter screenshot out.png`). Renders into its own PSRAM buffer, so it
// shows what LVGL drew, not what the panel displays — fine for layout QA.
static void send_screenshot(void) {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT, size = w * h * 2;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }
    lv_draw_buf_t db;
    lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB565, w * 2, sbuf, size);
    if (lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &db) != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h,
                  (unsigned long)size);
    Serial.flush();
    Serial.write(sbuf, size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void console(const char* line) {
    if (strcmp(line, "screenshot") == 0)      send_screenshot();
    else if (strcmp(line, "settings") == 0)   ui_settings_open();
    else if (strcmp(line, "dashboard") == 0)  ui_show_dashboard();
    else if (strcmp(line, "expand") == 0)     ui_toggle_expanded();
    else if (strcmp(line, "range") == 0)      ui_cycle_range();
    else if (strcmp(line, "keypad") == 0)     ui_settings_open_keypad(true);
}

void setup() {
    Serial.setRxBufferSize(2048);
    Serial.begin(115200);
    delay(200);

    settings_load();
    board_init();
    touch_init();

    lv_init();
    lv_tick_set_cb(tick_cb);
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    link_console_handler = console;
    link_settings_handler = ui_apply_settings;
    ui_init();

    // First frame into the framebuffer before the backlight comes on, so the
    // panel never shows power-on garbage.
    lv_timer_handler();
    lv_refr_now(NULL);
    board_set_backlight(settings().brightness);

    Serial.printf("NetMeter %s ready, %dx%d\n", FW_VERSION, LCD_WIDTH, LCD_HEIGHT);
}

void loop() {
    link_poll();
    NetSample s;
    if (link_take_sample(&s)) ui_on_sample(s);
    ui_tick();
    lv_timer_handler();
    delay(5);
}
