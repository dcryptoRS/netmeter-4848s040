#pragma once
#include <stdint.h>

// Per-direction high-usage alert with debounce, so a one-second burst (a page
// load, a speed-test blip) doesn't fire it and a flow hovering on the line
// doesn't make it flicker:
//
//   fires  after usage stays >= threshold                for ALERT_HOLD_MS
//   clears after usage stays <  threshold - ALERT_HYST_PCT for ALERT_CLEAR_MS

#define ALERT_HOLD_MS   3000
#define ALERT_CLEAR_MS  5000
#define ALERT_HYST_PCT  5

struct AlertState {
    bool     active;
    uint32_t above_since;   // 0 = not currently above
    uint32_t below_since;   // 0 = not currently below the clear line
};

enum AlertEvent { ALERT_NONE, ALERT_FIRED, ALERT_CLEARED };

// threshold_pct 0 disables the alert (and clears it if active).
AlertEvent alert_update(AlertState* a, float usage_pct, uint8_t threshold_pct, uint32_t now_ms);
AlertEvent alert_reset(AlertState* a);   // e.g. link lost: clear without debounce
