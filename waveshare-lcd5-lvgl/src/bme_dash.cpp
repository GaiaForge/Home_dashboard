/**
 * BME280 environment dashboard. See bme_dash.h.
 */
#include "bme_dash.h"
#include "bme280.h"

#include <lvgl.h>
#include <Arduino.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * WIRING — the sensor shares the board's I2C connector (the touch bus).
 *
 * BME280        board I2C connector
 *   VCC/VIN  ->  3V3
 *   GND      ->  GND
 *   SDA      ->  SDA  (GPIO 8)
 *   SCL      ->  SCL  (GPIO 9)
 * Address (0x76 / 0x77) is auto-detected. See bme280.cpp for why this shares
 * the panel's legacy I2C driver rather than opening its own bus.
 * ------------------------------------------------------------------ */
#define BME_I2C_PORT  0

/* ------------------------------------------------------------------ */
#define C_BG      0x081018
#define C_CARD    0x122130
#define C_INNER   0x1C2E42
#define C_TEXT    0xEAF2FB
#define C_MUTED   0x7C93AC
#define C_TEMP    0xF97316
#define C_HUM     0x22D3EE
#define C_PRES    0xA855F7
#define C_OK      0x22C55E
#define C_BAD     0xEF4444
#define C_WARN    0xF59E0B

#define CHART_POINTS 60

static Bme280 s_bme;
static bool   s_connected;

static lv_obj_t *s_status;
static lv_obj_t *s_temp_val, *s_hum_val, *s_pres_val;
static lv_obj_t *s_dew_val, *s_vpd_val, *s_min_val, *s_max_val;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;

static float s_tmin = 999, s_tmax = -999;
static uint32_t s_reprobe_ms;

static lv_obj_t *s_sub;          /* subtitle — shows bus + address */

/** Probe the sensor on the shared touch I2C bus (port 0). */
static bool scan_for_sensor(void)
{
    if (s_bme.begin(BME_I2C_PORT)) {
        Serial.printf("BME280 found on I2C port %d addr=0x%02X\n",
                      BME_I2C_PORT, s_bme.address());
        return true;
    }
    return false;
}

static void update_subtitle(void)
{
    if (s_bme.ok()) {
        lv_label_set_text_fmt(s_sub, "BME280  ·  shared I2C (GPIO 8/9)  ·  0x%02X",
                              s_bme.address());
    } else {
        lv_label_set_text(s_sub, "BME280  ·  no answer on the I2C bus — check wiring");
    }
}

/* ------------------------------------------------------------------ */

static lv_obj_t *card(int x, int y, int w, int h, const char *caption)
{
    lv_obj_t *c = lv_obj_create(lv_screen_active());
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(c);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(C_MUTED), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 16, 12);
    return c;
}

/** Big value label inside a card, plus a unit suffix. */
static lv_obj_t *big_value(lv_obj_t *c, const char *unit, uint32_t color, int yoff)
{
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 18, yoff);

    lv_obj_t *u = lv_label_create(c);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), 0);
    lv_obj_align(u, LV_ALIGN_LEFT_MID, 20, yoff + 40);
    return v;
}

static lv_obj_t *small_value(lv_obj_t *c, const char *unit, uint32_t color)
{
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 16, 6);

    lv_obj_t *u = lv_label_create(c);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), 0);
    lv_obj_align(u, LV_ALIGN_BOTTOM_LEFT, 16, -10);
    return v;
}

/* LVGL's built-in printf has no %f support (LV_SPRINTF_USE_FLOAT off), so we
 * format floats with newlib snprintf and set the text as a plain string. */
static void set_num(lv_obj_t *l, const char *fmt, float v)
{
    char b[16];
    snprintf(b, sizeof b, fmt, v);
    lv_label_set_text(l, b);
}

static void set_status(bool live)
{
    lv_label_set_text(s_status, live ? LV_SYMBOL_OK "  LIVE"
                                     : LV_SYMBOL_WARNING "  NO SENSOR");
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_status),
                              lv_color_hex(live ? C_OK : C_BAD), 0);
}

/* ------------------------------------------------------------------ */

static void tick(lv_timer_t *t)
{
    LV_UNUSED(t);

    if (!s_connected) {
        /* Re-probe every 3 s so the board comes alive when wired. */
        s_reprobe_ms += 1000;
        if (s_reprobe_ms >= 3000) {
            s_reprobe_ms = 0;
            s_connected = scan_for_sensor();
            if (s_connected) { set_status(true); update_subtitle(); }
        }
        return;
    }

    float tC, rh, hPa;
    if (!s_bme.read(tC, rh, hPa)) {
        s_connected = false;
        set_status(false);
        return;
    }

    Serial.printf("BME280: %.2f C   %.1f %%RH   %.1f hPa\n", tC, rh, hPa);

    set_num(s_temp_val, "%.1f", tC);
    set_num(s_hum_val,  "%.0f", rh);
    set_num(s_pres_val, "%.0f", hPa);

    /* Derived: dew point (Magnus) and VPD (vapour-pressure deficit). */
    double g   = log(rh / 100.0) + 17.62 * tC / (243.12 + tC);
    double dew = 243.12 * g / (17.62 - g);
    double svp = 0.6108 * exp(17.27 * tC / (tC + 237.3));   /* kPa */
    double vpd = svp * (1.0 - rh / 100.0);

    set_num(s_dew_val, "%.1f", (float)dew);
    set_num(s_vpd_val, "%.2f", (float)vpd);

    if (tC < s_tmin) s_tmin = tC;
    if (tC > s_tmax) s_tmax = tC;
    set_num(s_min_val, "%.1f", s_tmin);
    set_num(s_max_val, "%.1f", s_tmax);

    /* Push temperature (tenths of a degree) and keep the Y range snug. */
    lv_chart_set_next_value(s_chart, s_series, (int32_t)(tC * 10));
    int lo = (int)(s_tmin * 10) - 5;
    int hi = (int)(s_tmax * 10) + 5;
    if (hi - lo < 20) hi = lo + 20;
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
}

/* ------------------------------------------------------------------ */

void bme_dash_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Environment");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 16);

    s_sub = lv_label_create(scr);
    lv_label_set_text(s_sub, "BME280  ·  temp / humidity / pressure");
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_LEFT, 22, 52);

    lv_obj_t *pill = lv_obj_create(scr);
    lv_obj_set_size(pill, 200, 44);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_bg_color(pill, lv_color_hex(C_BAD), 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_radius(pill, 22, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    s_status = lv_label_create(pill);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x081018), 0);
    lv_obj_center(s_status);
    lv_label_set_text(s_status, LV_SYMBOL_WARNING "  NO SENSOR");

    /* Primary tiles */
    lv_obj_t *tc = card(16,  84, 316, 150, "TEMPERATURE");
    s_temp_val = big_value(tc, "\xC2\xB0""C", C_TEMP, -6);
    lv_obj_t *hc = card(354, 84, 316, 150, "HUMIDITY");
    s_hum_val = big_value(hc, "%RH", C_HUM, -6);
    lv_obj_t *pc = card(692, 84, 316, 150, "PRESSURE");
    s_pres_val = big_value(pc, "hPa", C_PRES, -6);

    /* Temperature trend */
    lv_obj_t *chc = card(16, 246, 992, 200, "TEMPERATURE TREND  (last 60 samples)");
    s_chart = lv_chart_create(chc);
    lv_obj_set_size(s_chart, 956, 130);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 4, 8);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 8, 0);
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(C_TEMP), LV_CHART_AXIS_PRIMARY_Y);

    /* Derived + min/max */
    lv_obj_t *dc = card(16,  458, 238, 126, "DEW POINT");
    s_dew_val = small_value(dc, "\xC2\xB0""C", C_HUM);
    lv_obj_t *vc = card(266, 458, 238, 126, "VPD");
    s_vpd_val = small_value(vc, "kPa", C_WARN);
    lv_obj_t *mnc = card(516, 458, 238, 126, "TEMP MIN");
    s_min_val = small_value(mnc, "\xC2\xB0""C", C_MUTED);
    lv_obj_t *mxc = card(766, 458, 238, 126, "TEMP MAX");
    s_max_val = small_value(mxc, "\xC2\xB0""C", C_MUTED);

    /* Scan for the sensor now; the timer keeps retrying if it's not found. */
    s_connected = scan_for_sensor();
    set_status(s_connected);
    update_subtitle();

    lv_timer_create(tick, 1000, NULL);
}
