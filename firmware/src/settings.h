#pragma once
#include <stddef.h>
#include <stdint.h>

// User settings, persisted in NVS (survive power cycles and firmware updates
// that don't erase flash). Editable on the touchscreen or from the PC with
// `netmeter config ...`.

enum Lang : uint8_t { LANG_ES = 0, LANG_EN = 1 };

#define ALERT_OFF 0   // alert_*_pct value meaning "never alert"

struct Settings {
    uint32_t plan_down_mbps;   // contracted download speed
    uint32_t plan_up_mbps;     // contracted upload speed
    uint8_t  alert_down_pct;   // alert when download >= this % of plan; 0 = off
    uint8_t  alert_up_pct;     // same for upload
    uint8_t  brightness;       // backlight 5..100 %
    Lang     lang;
    bool     plan_set;         // false until the user has entered a plan once
};

#define PLAN_MIN_MBPS      1
#define PLAN_MAX_MBPS      100000   // 100 Gbps; anything above is a typo
#define ALERT_MIN_PCT      10
#define ALERT_MAX_PCT      100
#define BRIGHTNESS_MIN     5

void            settings_load(void);
const Settings& settings(void);
void            settings_save(const Settings& s);   // clamps, persists, applies brightness

// JSON in the shape the PC app uses: {"pd":600,"pu":600,"ad":80,"au":80,"br":90,"lang":"es"}
// apply_json accepts any subset of the keys and returns false on a parse error.
bool settings_apply_json(const char* json);
void settings_to_json(char* out, size_t len);
