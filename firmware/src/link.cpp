#include "link.h"
#include "settings.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>

void (*link_console_handler)(const char* line) = nullptr;
void (*link_settings_handler)(void) = nullptr;

static NetSample s_sample = {};
static bool      s_new = false;
static bool      s_seen = false;
static uint32_t  s_last_rx_ms = 0;
static long      s_epoch = 0;          // PC clock at s_epoch_ms
static uint32_t  s_epoch_ms = 0;

// Sized for the largest line we accept; samples are ~150 bytes.
#define LINE_MAX 512
static char s_line[LINE_MAX];
static int  s_len = 0;
static bool s_overflow = false;

static void send_hello(void) {
    Serial.printf("#NETMETER %s\n", FW_VERSION);
}

static void send_cfg(void) {
    char buf[160];
    settings_to_json(buf, sizeof(buf));
    Serial.printf("#CFG %s\n", buf);
}

static void parse_sample(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;

    NetSample s = {};
    // Rates and totals are read as float even when the PC sends an integer;
    // as<float>() converts, whereas `doc["d"] | 0.0f` would silently return
    // 0 for an integer payload (ArduinoJson's `|` is strictly typed).
    s.down_bps = doc["d"].as<float>() * 8.0f;
    s.up_bps   = doc["u"].as<float>() * 8.0f;
    s.rx_mb    = doc["rx"].is<float>() || doc["rx"].is<long>() ? doc["rx"].as<float>() : -1.0f;
    s.tx_mb    = doc["tx"].is<float>() || doc["tx"].is<long>() ? doc["tx"].as<float>() : -1.0f;
    s.ping_ms  = doc["p"].is<long>() ? doc["p"].as<int>() : -1;
    strlcpy(s.src, doc["src"] | "", sizeof(s.src));
    if (s.down_bps < 0) s.down_bps = 0;
    if (s.up_bps < 0) s.up_bps = 0;

    if (doc["t"].is<long>()) {
        s_epoch = doc["t"].as<long>();
        s_epoch_ms = millis();
    }
    s_sample = s;
    s_new = true;
    s_seen = true;
    s_last_rx_ms = millis();
}

static void handle_line(char* line) {
    if (line[0] != '#') {
        if (link_console_handler) link_console_handler(line);
        return;
    }
    if (line[1] == 'N' && line[2] == ' ') {
        parse_sample(line + 3);
    } else if (line[1] == '?') {
        send_hello();
        send_cfg();
    } else if (line[1] == 'C') {
        if (line[2] == ' ') {
            if (!settings_apply_json(line + 3)) {
                Serial.println("#ERR config");
                return;
            }
            if (link_settings_handler) link_settings_handler();
        }
        send_cfg();
    }
}

void link_poll(void) {
    static bool hello_sent = false;
    if (!hello_sent) { send_hello(); hello_sent = true; }

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (s_len > 0 && !s_overflow) {
                s_line[s_len] = '\0';
                handle_line(s_line);
            }
            s_len = 0;
            s_overflow = false;
        } else if (s_len < LINE_MAX - 1) {
            s_line[s_len++] = c;
        } else {
            s_overflow = true;   // drop the whole line rather than parse half of it
        }
    }
}

bool link_take_sample(NetSample* out) {
    if (!s_new) return false;
    *out = s_sample;
    s_new = false;
    return true;
}

bool link_alive(void) { return s_seen && millis() - s_last_rx_ms < LINK_TIMEOUT_MS; }
bool link_ever_seen(void) { return s_seen; }

bool link_local_time(int* hour, int* min) {
    if (s_epoch <= 0) return false;
    time_t now = (time_t)(s_epoch + (long)((millis() - s_epoch_ms) / 1000));
    struct tm tmv;
    gmtime_r(&now, &tmv);   // the PC already applied its timezone
    *hour = tmv.tm_hour;
    *min = tmv.tm_min;
    return true;
}

void link_send_alert(bool down, int pct, float bits) {
    Serial.printf("#ALERT %s %d %.0f\n", down ? "down" : "up", pct, bits);
}

void link_send_clear(bool down) {
    Serial.printf("#CLEAR %s\n", down ? "down" : "up");
}
