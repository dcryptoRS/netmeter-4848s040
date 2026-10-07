#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "st7701_init.h"

// An RGB panel has no frame memory: the ESP32-S3's LCD_CAM peripheral streams
// a framebuffer out of PSRAM on every frame, forever. Arduino_RGB_Display owns
// that framebuffer (480*480*2 = 460 KB), and drawing is a memcpy into it. The
// ST7701 only listens to its 3-wire SPI channel during the power-on sequence.

static Arduino_DataBus*       spi_bus  = nullptr;
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_RGB_Display*   gfx      = nullptr;

void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL, 400000);

    spi_bus = new Arduino_SWSPI(GFX_NOT_DEFINED /* dc */, LCD_SPI_CS, LCD_SPI_SCK,
                                LCD_SPI_SDA, GFX_NOT_DEFINED /* miso */);
    rgbpanel = new Arduino_ESP32RGBPanel(
        LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
        LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
        LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
        LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
        1 /* hsync_polarity */, LCD_HSYNC_FRONT, LCD_HSYNC_PULSE, LCD_HSYNC_BACK,
        1 /* vsync_polarity */, LCD_VSYNC_FRONT, LCD_VSYNC_PULSE, LCD_VSYNC_BACK,
        0 /* pclk_active_neg */, LCD_PCLK_HZ, false /* useBigEndian */,
        0 /* de_idle_high */, 0 /* pclk_idle_high */, LCD_BOUNCE_BUFFER_PX);
    gfx = new Arduino_RGB_Display(
        LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */, true /* auto_flush */,
        spi_bus, GFX_NOT_DEFINED /* rst: panel has its own power-on reset */,
        st7701_guition_4848s040_init_operations,
        sizeof(st7701_guition_4848s040_init_operations));

    if (!gfx->begin()) {
        Serial.println("board: display begin FAILED (PSRAM framebuffer?)");
        return;
    }
    gfx->fillScreen(0x0000);

    // Backlight stays off until the first frame is drawn (main.cpp), so the
    // panel never flashes uninitialised framebuffer contents.
    ledcAttach(LCD_BL_GPIO, LCD_BL_LEDC_FREQ, LCD_BL_LEDC_BITS);
    ledcWrite(LCD_BL_GPIO, 0);
}

void board_set_backlight(uint8_t percent) {
    // The PWM is filtered to an analog LED drive level, so light output sits
    // near the LED's knee: almost nothing below LCD_BL_MIN_DUTY, and most of
    // the visible change crowded into the top of the range (75 % duty already
    // reads as dim). Mapping the slider onto that band through a square root
    // spends more of its travel where the eye notices, so equal slider steps
    // look roughly like equal brightness steps.
    uint32_t duty = 0;
    if (percent > 0) {
        if (percent > 100) percent = 100;
        duty = LCD_BL_MIN_DUTY +
               (uint32_t)((255 - LCD_BL_MIN_DUTY) * sqrtf(percent / 100.0f) + 0.5f);
    }
    ledcWrite(LCD_BL_GPIO, duty);
}

void board_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* px) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)px, w, h);
}
