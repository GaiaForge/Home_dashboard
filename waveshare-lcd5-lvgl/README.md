# Waveshare ESP32-S3-Touch-LCD-5 — LVGL v9 Starter (PlatformIO)

A known-good, fully self-contained PlatformIO project for the Waveshare
**ESP32-S3-Touch-LCD-5** (800×480) and **-5B** (1024×600) boards.

Everything is vendored and version-pinned — no library registry roulette:

| Piece | Version | Where it came from |
|---|---|---|
| ESP32_Display_Panel | 1.0.0 | Waveshare's official demo repo |
| ESP32_IO_Expander (CH422G driver) | 1.0.1 | Waveshare's official demo repo |
| esp-lib-utils | 0.1.2 | Waveshare's official demo repo |
| LVGL | 9.5.0 | Waveshare's official demo repo |
| arduino-esp32 core | 3.x (pioarduino) | pinned in `platformio.ini` |

The board pin map, RGB timings, GT911 touch, and CH422G backlight/reset
handling all come from Waveshare's own tested configuration
(`include/esp_panel_board_custom_conf.h`) — this is the file that was almost
certainly wrong in whatever you tried before.

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE**
   extension (or `pip install platformio` for the CLI).
2. Open this folder in VS Code. PlatformIO will pick up `platformio.ini`.
3. Connect the board via USB (use the **USB** port, or the UART port — both work
   for flashing; the UART port is more reliable for first flashes).
4. Build & upload: click the → arrow in the PlatformIO toolbar, or run:

   ```
   pio run -t upload
   ```

   First build downloads the toolchain (~10 min); later builds take seconds.
5. Open the serial monitor at 115200 baud to see boot logs:

   ```
   pio device monitor
   ```

You should see a dark blue screen: **"It's alive!"**, a counter button, a
slider, and a live touch-coordinate readout at the bottom.

## Panel variant — THIS BOARD IS A 5B (1024×600)

Verified on hardware 2026-07-25: the board on the bench is the **-5B**, so
`include/esp_panel_board_custom_conf.h` line 18 is set to:

```c
#define ESP_PANEL_USE_1024_600_LCD  (1)   // 0: 800x480, 1: 1024x600
```

Do not "fix" this back to 0. With 0 the screen shows a mostly-white image with
vertical stripe bands and hard block edges — no trace of the UI — even though
the firmware boots fine and the serial log reaches `Setup done`. That symptom
is a pure RGB timing mismatch, not a crash.

The switch changes both resolution and the RGB clock/porch timings, and the
horizontal porches differ a lot between variants (HBP 8 → 145, HFP 8 → 170,
clock 16 → 21 MHz), which is why a mismatch garbles the image rather than
merely shifting it.

## Run the full LVGL widgets demo

In `src/main.cpp`, set:

```c
#define RUN_LVGL_WIDGETS_DEMO 1
```

## Project layout

```
platformio.ini                        board + build config (16MB flash, OPI PSRAM)
include/
  esp_panel_board_custom_conf.h       ALL hardware config: pins, timings, touch, backlight
  lv_conf.h                           LVGL configuration
src/
  main.cpp                            your app — start hacking here
  lvgl_v9_port.cpp/.h                 LVGL <-> display/touch glue (FreeRTOS task, vsync sync)
lib/
  lvgl/                               LVGL 9.5.0 (vendored)
  ESP32_Display_Panel/                Espressif display driver framework (vendored)
  ESP32_IO_Expander/                  CH422G IO expander driver (vendored)
  esp-lib-utils/                      support library (vendored)
```

## Troubleshooting

- **Upload fails / port not found**: hold the **BOOT** button while pressing
  **RESET**, release RESET then BOOT — this forces download mode. Then retry.
- **White screen**: backlight is on but no pixel data — usually means the
  firmware crashed before LVGL started. Check the serial monitor.
- **Shifted / rolling image or noise**: wrong variant selected (see -5B note
  above). This is the single most likely cause — check it before anything else.
- **Driver logs appear but your own `Serial.println` does not**: on the ESP32-S3
  the Arduino `Serial` object defaults to UART0 on the pin header, while
  ESP-IDF's `ESP_LOG` output goes out the native USB port. So the board looks
  half-silent — you see `[I][Panel]...` lines but none of your own. Fixed here
  by `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`.
- **Touch works but display glitches during animations**: lower the RGB clock:
  in `esp_panel_board_custom_conf.h` reduce `ESP_PANEL_BOARD_LCD_RGB_CLK_HZ`
  (e.g. from 16 MHz to 14 MHz).
- **Build errors about missing lv_conf.h**: make sure you opened *this folder*
  (the one containing `platformio.ini`) as the project root, not a parent.

## Where the config came from

- Board repo: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-5
- Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-5
