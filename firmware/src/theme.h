#pragma once
#include <lvgl.h>

// Design tokens. Neutral dark surfaces, one blue accent, colour reserved for
// data and status.
//
// Every value survives the RGB565 round trip unchanged. That matters on this
// 16-bit panel: an arbitrary dark grey such as #131416 quantises to unequal
// R/G/B steps and comes out visibly green- or blue-tinted, and two "close"
// greys can collapse onto the same colour.
#define C_BG        lv_color_hex(0x101010)   // page
#define C_CARD      lv_color_hex(0x181818)   // cards
#define C_CARD_HI   lv_color_hex(0x212021)   // pressed rows, keypad keys
#define C_BORDER    lv_color_hex(0x292829)   // outlines, dividers, bar track
#define C_GRID      lv_color_hex(0x212021)   // chart gridlines
#define C_TEXT      lv_color_hex(0xEFEFEF)   // primary text, live numbers
#define C_TEXT_2    lv_color_hex(0xA5A6A5)   // secondary text, units
#define C_TEXT_3    lv_color_hex(0x6B696B)   // captions, axis labels
#define C_DOWN      lv_color_hex(0x006DFF)   // download series, accent
#define C_UP        lv_color_hex(0xA58AF7)   // upload series
#define C_GREEN     lv_color_hex(0x39CB63)   // healthy
#define C_AMBER     lv_color_hex(0xF7A621)   // slow ping
#define C_RED       lv_color_hex(0xEF4539)   // alert, offline

LV_FONT_DECLARE(font_inter_12);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_inter_md_14);
LV_FONT_DECLARE(font_inter_md_16);
LV_FONT_DECLARE(font_inter_md_20);
LV_FONT_DECLARE(font_inter_sb_20);
LV_FONT_DECLARE(font_inter_num_48);
