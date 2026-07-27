/**
 * Waveshare ESP32-S3-Touch-LCD-5 — LVGL v9 starter project (PlatformIO)
 *
 * Hardware config lives in include/esp_panel_board_custom_conf.h.
 *   - If you have the 1024x600 variant (-5B), set
 *     ESP_PANEL_USE_1024_600_LCD to (1) at the top of that file.
 *
 * Set RUN_LVGL_WIDGETS_DEMO to 1 to run LVGL's full built-in widgets demo
 * instead of the simple starter UI below.
 */
/* ------------------------------------------------------------------ *
 * Pick ONE. First match wins, so set the others to 0.
 *
 *   MC_DASH    MicroClimate quick-dashboard (WiFi: indoor BME280 + field hubs)
 *   BME_DASH   BME280 environment dashboard (temp / humidity / pressure)
 *   NET_TOOL   2.4 GHz Recon — passive WiFi + BLE scanner
 *   SHOWCASE   live telemetry dashboard (motion, gauges, FPS meter)
 *   GALLERY    tabbed tour of LVGL widgets
 *   DIAG       GaiaForge Field Diagnostics mockup (synthetic fleet data)
 *   WIDGETS    LVGL's own built-in widgets demo
 *   (none)     the plain starter UI: title / button / slider / touch
 * ------------------------------------------------------------------ */
#define RUN_MC_DASH           1
#define RUN_BME_DASH          0
#define RUN_NET_TOOL          0
#define RUN_SHOWCASE_UI       0
#define RUN_GALLERY_UI        0
#define RUN_DIAG_UI           0
#define RUN_LVGL_WIDGETS_DEMO 0

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#if RUN_LVGL_WIDGETS_DEMO
#include <demos/lv_demos.h>
#endif

#include "lvgl_v9_port.h"
#if RUN_DIAG_UI
#include "diag_ui.h"
#endif
#if RUN_GALLERY_UI
#include "gallery_ui.h"
#endif
#if RUN_SHOWCASE_UI
#include "showcase_ui.h"
#endif
#if RUN_NET_TOOL
#include "net_tool.h"
#endif
#if RUN_BME_DASH
#include "bme_dash.h"
#endif
#if RUN_MC_DASH
#include "mc_dash.h"
#endif

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* ------------------------------------------------------------------ */
/* A simple starter UI: title, a counter button, a slider, and a      */
/* label that follows your finger — proof that display + touch work.  */
/* ------------------------------------------------------------------ */

static lv_obj_t *touch_label;

static void btn_event_cb(lv_event_t *e)
{
    static int count = 0;
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    count++;
    lv_label_set_text_fmt(label, "Pressed %d time%s", count, count == 1 ? "" : "s");
}

static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text_fmt(label, "Brightness-ish: %d%%", (int)lv_slider_get_value(slider));
}

static void screen_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_label_set_text_fmt(touch_label, "Touch: %d, %d", (int)p.x, (int)p.y);
}

static void create_starter_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101828), LV_PART_MAIN);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "It's alive!");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Waveshare ESP32-S3 5\" + LVGL v9");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 105);

    /* Counter button */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 260, 80);
    lv_obj_align(btn, LV_ALIGN_CENTER, -170, 30);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Press me");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(btn_label);

    lv_obj_t *count_label = lv_label_create(scr);
    lv_label_set_text(count_label, "Not pressed yet");
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_align(count_label, LV_ALIGN_CENTER, -170, 105);

    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, count_label);

    /* Slider */
    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_width(slider, 280);
    lv_obj_align(slider, LV_ALIGN_CENTER, 170, 30);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);

    lv_obj_t *slider_label = lv_label_create(scr);
    lv_label_set_text(slider_label, "Brightness-ish: 50%");
    lv_obj_set_style_text_font(slider_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(slider_label, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_align(slider_label, LV_ALIGN_CENTER, 170, 105);

    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, slider_label);

    /* Touch coordinate readout */
    touch_label = lv_label_create(scr);
    lv_label_set_text(touch_label, "Touch the screen...");
    lv_obj_set_style_text_font(touch_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(touch_label, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_align(touch_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, nullptr);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Initializing board");

    Board *board = new Board();
    board->init();

    auto lcd = board->getLCD();
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lcd->configFrameBufferNumber(LVGL_V9_PORT_FRAME_BUFFER_NUM);
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }

    assert(board->begin());

    Serial.println("Initializing LVGL v9");
    lvgl_v9_port_init(lcd, board->getTouch());

    Serial.println("Creating UI");
    lvgl_v9_port_lock(-1);
#if RUN_MC_DASH
    mc_dash_create();
#elif RUN_BME_DASH
    bme_dash_create();
#elif RUN_NET_TOOL
    net_tool_create();
#elif RUN_SHOWCASE_UI
    showcase_ui_create();
#elif RUN_GALLERY_UI
    gallery_ui_create();
#elif RUN_DIAG_UI
    diag_ui_create();
#elif RUN_LVGL_WIDGETS_DEMO
    lv_demo_widgets();
#else
    create_starter_ui();
#endif
    lvgl_v9_port_unlock();

    Serial.println("Setup done");
}

void loop()
{
#if RUN_NET_TOOL
    /* The scanner's blocking WiFi/BLE calls run here, off the LVGL task. */
    net_tool_loop();
    delay(20);
#elif RUN_MC_DASH
    /* WiFi connect + blocking hub fetch run here, off the LVGL task. */
    mc_dash_loop();
    delay(50);
#else
    delay(1000);
#endif
}
