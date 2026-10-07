#pragma once

// UI strings in Spanish and English. The language is a setting; changing it
// rebuilds the visible text in place (ui_apply_language()).

enum StrId {
    S_INTERNET,
    S_DOWNLOAD, S_UPLOAD, S_PEAK, S_TRAFFIC,
    S_LATENCY, S_RECEIVED, S_SENT, S_PLAN_MBPS, S_SET_UP,
    S_ST_WAITING, S_ST_ONLINE, S_ST_OFFLINE, S_ST_ALERT,
    S_WAIT_TITLE, S_WAIT_BODY, S_NO_DATA,
    S_BANNER_DOWN, S_BANNER_UP, S_OF_PLAN,
    S_SETTINGS, S_DONE,
    S_SEC_PLAN, S_SEC_ALERTS, S_SEC_DISPLAY,
    S_PLAN_DOWN, S_PLAN_UP, S_ALERT_DOWN, S_ALERT_UP,
    S_BRIGHTNESS, S_LANGUAGE, S_OFF,
    S_KP_DOWN, S_KP_UP, S_CANCEL, S_SAVE,
    S_COUNT
};

const char* tr(StrId id);
