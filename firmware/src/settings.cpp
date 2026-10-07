#include "settings.h"
#include "board/board.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static const char* NVS_NS = "netmeter";

// Defaults for a fresh device. The plan is a placeholder until the user sets
// theirs; the dashboard says so instead of pretending 100 Mbps is real.
static Settings s_cur = {
    .plan_down_mbps = 100,
    .plan_up_mbps   = 100,
    .alert_down_pct = 80,
    .alert_up_pct   = 80,
    .brightness     = 100,
    .lang           = LANG_ES,
    .plan_set       = false,
};

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static Settings sanitized(Settings s) {
    s.plan_down_mbps = clamp_u32(s.plan_down_mbps, PLAN_MIN_MBPS, PLAN_MAX_MBPS);
    s.plan_up_mbps   = clamp_u32(s.plan_up_mbps, PLAN_MIN_MBPS, PLAN_MAX_MBPS);
    if (s.alert_down_pct != ALERT_OFF)
        s.alert_down_pct = (uint8_t)clamp_u32(s.alert_down_pct, ALERT_MIN_PCT, ALERT_MAX_PCT);
    if (s.alert_up_pct != ALERT_OFF)
        s.alert_up_pct = (uint8_t)clamp_u32(s.alert_up_pct, ALERT_MIN_PCT, ALERT_MAX_PCT);
    s.brightness = (uint8_t)clamp_u32(s.brightness, BRIGHTNESS_MIN, 100);
    if (s.lang != LANG_ES && s.lang != LANG_EN) s.lang = LANG_ES;
    return s;
}

void settings_load(void) {
    Preferences p;
    if (p.begin(NVS_NS, true)) {
        Settings s = s_cur;
        s.plan_down_mbps = p.getULong("pd", s.plan_down_mbps);
        s.plan_up_mbps   = p.getULong("pu", s.plan_up_mbps);
        s.alert_down_pct = p.getUChar("ad", s.alert_down_pct);
        s.alert_up_pct   = p.getUChar("au", s.alert_up_pct);
        s.brightness     = p.getUChar("br", s.brightness);
        s.lang           = (Lang)p.getUChar("lang", s.lang);
        s.plan_set       = p.getBool("set", false);
        p.end();
        s_cur = sanitized(s);
    }
}

const Settings& settings(void) { return s_cur; }

void settings_save(const Settings& in) {
    Settings s = sanitized(in);
    bool brightness_changed = s.brightness != s_cur.brightness;
    s_cur = s;

    Preferences p;
    if (p.begin(NVS_NS, false)) {
        p.putULong("pd", s.plan_down_mbps);
        p.putULong("pu", s.plan_up_mbps);
        p.putUChar("ad", s.alert_down_pct);
        p.putUChar("au", s.alert_up_pct);
        p.putUChar("br", s.brightness);
        p.putUChar("lang", s.lang);
        p.putBool("set", s.plan_set);
        p.end();
    }
    if (brightness_changed) board_set_backlight(s.brightness);
}

bool settings_apply_json(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    Settings s = s_cur;
    // Every field is read through an integer and only when present: a missing
    // key must keep the current value, and ArduinoJson's `|` is strictly
    // typed (a float default against an int payload silently returns the
    // default), so no defaults are relied on here.
    if (doc["pd"].is<long>()) { s.plan_down_mbps = doc["pd"].as<long>(); s.plan_set = true; }
    if (doc["pu"].is<long>()) { s.plan_up_mbps   = doc["pu"].as<long>(); s.plan_set = true; }
    if (doc["ad"].is<long>()) s.alert_down_pct = (uint8_t)clamp_u32(doc["ad"].as<long>(), 0, 100);
    if (doc["au"].is<long>()) s.alert_up_pct   = (uint8_t)clamp_u32(doc["au"].as<long>(), 0, 100);
    if (doc["br"].is<long>()) s.brightness     = (uint8_t)clamp_u32(doc["br"].as<long>(), 0, 100);
    if (doc["lang"].is<const char*>()) {
        const char* l = doc["lang"];
        s.lang = (l[0] == 'e' && l[1] == 'n') ? LANG_EN : LANG_ES;
    }
    settings_save(s);
    return true;
}

void settings_to_json(char* out, size_t len) {
    snprintf(out, len,
             "{\"pd\":%lu,\"pu\":%lu,\"ad\":%u,\"au\":%u,\"br\":%u,\"lang\":\"%s\",\"set\":%d}",
             (unsigned long)s_cur.plan_down_mbps, (unsigned long)s_cur.plan_up_mbps,
             s_cur.alert_down_pct, s_cur.alert_up_pct, s_cur.brightness,
             s_cur.lang == LANG_EN ? "en" : "es", s_cur.plan_set ? 1 : 0);
}
