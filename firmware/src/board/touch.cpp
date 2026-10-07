#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// GT911 reader, written inline because the common GT911 libraries are GPL and
// the protocol is one register burst.
//
//   0x8140  product ID, reads "911"
//   0x814E  status: bit7 = new data, bits[3:0] = number of touch points
//   0x814F  point 0: [track_id, x_lo, x_hi, y_lo, y_hi, ...]
//
// The status byte must be written back to 0 after every read, or the
// controller stops producing samples. INT and RST are not broken out on this
// board, so it is polled once per LVGL input read.

#define GT911_REG_PRODUCT_ID  0x8140
#define GT911_REG_STATUS      0x814E
#define GT911_REG_POINT0      0x814F

static uint8_t  gt911_addr = TP_ADDR;
static uint16_t touch_x = 0, touch_y = 0;
static bool     touch_pressed = false;

static bool gt911_read(uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)gt911_addr, (int)len) != (int)len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static void gt911_clear_status(void) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(GT911_REG_STATUS >> 8));
    Wire.write((uint8_t)(GT911_REG_STATUS & 0xFF));
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();
}

void touch_init(void) {
    const uint8_t candidates[] = {TP_ADDR, TP_ADDR_ALT};
    for (uint8_t addr : candidates) {
        gt911_addr = addr;
        uint8_t id[4] = {0};
        if (gt911_read(GT911_REG_PRODUCT_ID, id, sizeof(id)) &&
            id[0] == '9' && id[1] == '1' && id[2] == '1') {
            Serial.printf("touch: GT911 at 0x%02X\n", addr);
            gt911_clear_status();
            return;
        }
    }
    Serial.println("touch: GT911 not found");
}

void touch_read(uint16_t* x, uint16_t* y, bool* pressed) {
    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) && (status & 0x80)) {
        uint8_t points = status & 0x0F;
        if (points == 1) {
            uint8_t p[4] = {0};
            if (gt911_read(GT911_REG_POINT0 + 1, p, sizeof(p))) {
                touch_x = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                touch_y = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
                touch_pressed = true;
            }
        } else {
            // Lifted, or a palm/multi-finger contact: report released rather
            // than guess which finger the user meant.
            touch_pressed = false;
        }
        gt911_clear_status();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
