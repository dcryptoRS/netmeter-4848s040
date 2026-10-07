#include "alerts.h"

AlertEvent alert_reset(AlertState* a) {
    bool was = a->active;
    *a = {};
    return was ? ALERT_CLEARED : ALERT_NONE;
}

AlertEvent alert_update(AlertState* a, float usage_pct, uint8_t threshold_pct, uint32_t now_ms) {
    if (threshold_pct == 0) return alert_reset(a);
    // 0 is the "not timing" sentinel, so never store it as a timestamp.
    if (now_ms == 0) now_ms = 1;

    if (!a->active) {
        if (usage_pct >= threshold_pct) {
            if (!a->above_since) a->above_since = now_ms;
            if (now_ms - a->above_since >= ALERT_HOLD_MS) {
                a->active = true;
                a->above_since = 0;
                a->below_since = 0;
                return ALERT_FIRED;
            }
        } else {
            a->above_since = 0;
        }
        return ALERT_NONE;
    }

    float clear_line = (float)threshold_pct - ALERT_HYST_PCT;
    if (usage_pct < clear_line) {
        if (!a->below_since) a->below_since = now_ms;
        if (now_ms - a->below_since >= ALERT_CLEAR_MS) {
            a->active = false;
            a->below_since = 0;
            return ALERT_CLEARED;
        }
    } else {
        a->below_since = 0;
    }
    return ALERT_NONE;
}
