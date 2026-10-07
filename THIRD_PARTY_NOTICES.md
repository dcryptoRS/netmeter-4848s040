# Third-party notices

| Component | Used for | License |
|---|---|---|
| [Inter](https://github.com/rsms/inter) by Rasmus Andersson | All UI text. `firmware/src/fonts/font_inter_*.c` are 4-bit bitmap renderings generated with `tools/gen_inter_fonts.sh` (tabular figures baked in). | SIL Open Font License 1.1 — `firmware/src/fonts/Inter-LICENSE.txt` |
| [LVGL](https://github.com/lvgl/lvgl) 9.5 | UI toolkit (fetched at build time) | MIT |
| [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) | ST7701 RGB panel driver (fetched at build time) | BSD |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | Serial protocol parsing (fetched at build time) | MIT |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) via [pioarduino](https://github.com/pioarduino/platform-espressif32) | Framework | LGPL-2.1 / Apache-2.0 |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) | Browser installer page (loaded from unpkg) | Apache-2.0 |
| [pyserial](https://github.com/pyserial/pyserial), [psutil](https://github.com/giampaolo/psutil) | PC app | BSD-3-Clause |
| [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) and its fork | Origin of the Guition 4848S040 board port | MIT (source code) |

"UniFi" and "Ubiquiti" are trademarks of Ubiquiti Inc. This project is not
affiliated with or endorsed by Ubiquiti or Guition; it only talks to a UniFi
gateway's local API when you configure it to.
