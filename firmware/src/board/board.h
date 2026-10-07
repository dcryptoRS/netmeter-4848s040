#pragma once
#include <stdint.h>

// Guition ESP32-4848S040 (manufacturer Jingcai; some units carry only a batch
// code such as "TF042"). ESP32-S3-WROOM-1 N16R8, 4" 480x480 IPS, ST7701 on a
// 16-bit RGB parallel bus, GT911 capacitive touch, CH340 USB-UART bridge.
//
// Pin map from the ESP32_Display_Panel board config for this panel
// (BOARD_JINGCAI_ESP32_4848S040C_I_Y_3.h), the library the stock firmware uses.

#define LCD_WIDTH            480
#define LCD_HEIGHT           480

// ---- ST7701 init channel: 3-wire SPI (no MISO, no DC; bit 9 carries D/C) ----
#define LCD_SPI_CS           39
#define LCD_SPI_SCK          48
#define LCD_SPI_SDA          47

// ---- RGB565 data bus ----
// The vendor header lists a flat DATA0..15. Under esp_lcd's convention the low
// lines are blue and the high lines red. Swapping these turns red into blue.
#define LCD_R0               11
#define LCD_R1               12
#define LCD_R2               13
#define LCD_R3               14
#define LCD_R4               0
#define LCD_G0               8
#define LCD_G1               20
#define LCD_G2               3
#define LCD_G3               46
#define LCD_G4               9
#define LCD_G5               10
#define LCD_B0               4
#define LCD_B1               5
#define LCD_B2               6
#define LCD_B3               7
#define LCD_B4               15

#define LCD_HSYNC            16
#define LCD_VSYNC            17
#define LCD_DE               18
#define LCD_PCLK             21

// ---- RGB timing (Arduino_GFX values) ----
// Do not copy the porches from ESP32_Display_Panel: those are for ESP-IDF's
// esp_lcd stack and mis-register every line under Arduino_GFX.
//
// 12 MHz is Arduino_GFX's octal-PSRAM default. The framebuffer is streamed out
// of PSRAM continuously and cannot stall; 26 MHz starves it and smears.
#define LCD_PCLK_HZ          12000000L
#define LCD_HSYNC_PULSE      8
#define LCD_HSYNC_BACK       50
#define LCD_HSYNC_FRONT      10
#define LCD_VSYNC_PULSE      8
#define LCD_VSYNC_BACK       20
#define LCD_VSYNC_FRONT      10

// Bounce buffer (pixels) for the LCD DMA: two internal-SRAM buffers refilled
// from PSRAM ahead of the beam, so PSRAM latency jitter doesn't show up as
// lines shimmering. 30 lines = 2 x 28.8 KB of internal RAM.
#define LCD_BOUNCE_BUFFER_PX (LCD_WIDTH * 30)

// ---- Backlight: plain GPIO, active high, LEDC PWM ----
#define LCD_BL_GPIO          38
#define LCD_BL_LEDC_FREQ     5000
#define LCD_BL_LEDC_BITS     8
// Below this duty the backlight gives almost no light (measured by sweeping
// duty over a white screen), so the user's 1..100 % maps onto MIN..255.
#define LCD_BL_MIN_DUTY      120

// ---- I2C (touch only) ----
#define IIC_SDA              19
#define IIC_SCL              45

// ---- Touch: GT911, polled (neither INT nor RST is broken out) ----
#define TP_ADDR              0x5D
#define TP_ADDR_ALT          0x14   // where a GT911 lands if INT was high at reset

// There is no usable button: BOOT sits on GPIO 0, which the panel uses as R4.
// Everything is driven from the touchscreen.

// ---- Board API (board.cpp / touch.cpp) ----
void board_init(void);                       // I2C, panel, backlight
void board_set_backlight(uint8_t percent);   // 0 = off, 1..100
void board_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* px);

void touch_init(void);
// Latest single-finger sample. Multi-finger contacts report "not pressed".
void touch_read(uint16_t* x, uint16_t* y, bool* pressed);
