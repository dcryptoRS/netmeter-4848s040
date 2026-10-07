#pragma once
#include <stdint.h>

// Serial protocol between the screen and the NetMeter PC app (115200 8N1 over
// the board's CH340). Lines starting with '#' are protocol; anything else is a
// console command for humans (`screenshot`).
//
//   device -> PC   #NETMETER <version>        on boot and in reply to "#?"
//   PC -> device   #N {json}                  a traffic sample, ~2 per second
//                    d, u    download / upload, bytes per second
//                    rx, tx  totals in MB since the counter's origin (optional)
//                    p       latency in ms, -1 if unknown
//                    src     what is being measured, e.g. "eno1", "Wi-Fi", "UCG Ultra"
//                    t       local wall-clock time, seconds since epoch
//   PC -> device   #C {json} / #C?            set / read settings (see settings.h)
//   device -> PC   #CFG {json}                current settings, after either
//   PC -> device   #E gw|net <t>              the PC app is running but has nothing
//                                             to measure: the UniFi gateway isn't
//                                             answering (gw) or the PC is offline (net)
//   device -> PC   #ALERT down|up <pct> <bits_per_s>, #CLEAR down|up

struct NetSample {
    float down_bps;     // bits per second
    float up_bps;
    float rx_mb;        // < 0 when the PC did not send totals
    float tx_mb;
    int   ping_ms;      // < 0 when unknown
    char  src[24];
};

#define LINK_TIMEOUT_MS 6000   // samples arrive every ~500 ms

void link_poll(void);                       // read Serial, dispatch lines
bool link_take_sample(NetSample* out);      // true once per new sample
bool link_alive(void);                      // heard from the PC app recently
bool link_ever_seen(void);                  // heard from the PC app since boot
char link_source_error(void);               // 'g' gateway / 'n' PC offline / 0 = none
bool link_local_time(int* hour, int* min);  // from the PC's clock

void link_send_alert(bool down, int pct, float bits);
void link_send_clear(bool down);

// Set by main.cpp: non-protocol console lines, and "settings changed from the PC".
extern void (*link_console_handler)(const char* line);
extern void (*link_settings_handler)(void);
