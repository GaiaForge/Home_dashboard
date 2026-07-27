/**
 * MicroClimate quick-dashboard. See mc_dash.h.
 */
#include "mc_dash.h"
#include "bme280.h"
#include "wifi_store.h"
#include "pump_client.h"
#include "lvgl_v9_port.h"
#include "secrets.h"

#include <lvgl.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

/* Weather Icons font (generated: src/wi_font.c) + its glyph codepoints. */
extern const lv_font_t wi_font_30;

/* Full-color weather condition icons (generated from OpenMoji; src/wx_*.c). */
LV_IMAGE_DECLARE(wx_clear);
LV_IMAGE_DECLARE(wx_partly);
LV_IMAGE_DECLARE(wx_cloud);
LV_IMAGE_DECLARE(wx_fog);
LV_IMAGE_DECLARE(wx_rain);
LV_IMAGE_DECLARE(wx_snow);
LV_IMAGE_DECLARE(wx_storm);
#define WI_TEMP "\xEF\x81\x95"   /* 0xF055 thermometer */
#define WI_HUM  "\xEF\x81\xBA"   /* 0xF07A humidity    */
#define WI_PRES "\xEF\x81\xB9"   /* 0xF079 barometer   */
#define WI_WIND "\xEF\x81\x90"   /* 0xF050 strong wind */

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
#define C_ACCENT  0x3B82F6

#define HUB_REFRESH_MS 30000
#define WEATHER_REFRESH_MS 900000    /* 15 min — weather changes slowly */
#define MAX_HUBS 16

/* The garden valve controller — a separate standalone ESP32 on the same
 * WiFi (see ~/Projects/waveshar_ESP32_experiment/valve-controller). Reached
 * by mDNS name so it survives a DHCP lease change. */
/* MEASURED 2026-07-26: pointed at the bench 2-relay test unit for now.
 * Switch back to "gaiaforge-valve.local" when testing the real garden box —
 * see valve-controller/src/secrets.h for why unique hostnames matter here. */
#define VALVE_HOST "gaiaforge-valve-bench.local"
#define VALVE_REFRESH_MS 5000
static bool     s_valve_online;
static uint32_t s_last_valve_poll;
static volatile bool s_valve_dirty;

/* ---- indoor sensor ---- */
static Bme280 s_bme;
static bool s_bme_ok;
static lv_obj_t *s_in_temp, *s_in_hum, *s_in_pres, *s_in_dew, *s_in_vpd, *s_in_stat;

/* ---- header ---- */
static lv_obj_t *s_wifi_lbl, *s_clock;

/* ---- hubs ---- */
typedef struct {
    char  name[24];
    char  dev_eui[24];
    float temp, hum, pres, vpd, soil, batt, rssi;
    char  last_seen[28];
    int   severity;         /* 0 ok, 1 frost caution, 2 active alert */
    char  warn[56];         /* warning text when severity > 0 */
} hub_t;
static hub_t s_hubs[MAX_HUBS];
static int   s_hub_count;
static int   s_alert_count;
static lv_obj_t *s_hub_list, *s_hub_status;

/* ---- outside weather (Open-Meteo) ---- */
static lv_obj_t *s_out_temp, *s_out_hum, *s_out_pres, *s_out_wind;
static lv_obj_t *s_out_cond, *s_out_status, *s_out_img;
static lv_obj_t *s_fc_temp[3], *s_fc_cond[3];
static double   s_lat, s_lng;
static bool     s_have_loc;
static uint32_t s_last_weather;
static int      s_dialog_mode;       /* 0 = wifi password, 1 = location entry, 2 = pump host */
static lv_obj_t *s_loc_lbl;          /* shows current location in settings */
static lv_obj_t *s_pump_lbl;         /* shows current pump strip address in settings */

/* ---- cross-task WiFi/fetch state (loop side writes, timers read) ---- */
static volatile bool s_wifi_up;
static volatile bool s_hubs_dirty;      /* new data waiting to render */
static uint32_t s_last_fetch;
static bool s_wifi_started;
static bool s_fetch_failed;
static int  s_last_http;

/* ---- WiFi settings: store + scan + connect ---- */
static WifiStore s_store;

typedef struct { char ssid[33]; int rssi; bool secured; } ap_t;
static ap_t s_scan[24];
static volatile int s_scan_count;

typedef enum { WA_NONE, WA_SCAN, WA_CONNECT } wifi_action_t;
static volatile wifi_action_t s_action = WA_NONE;
static char s_pending_ssid[33];
static char s_pending_pass[65];
static volatile bool s_scan_ready;      /* loop -> render available list */
static volatile int  s_connect_result;  /* 0 none, 1 ok, -1 fail (to render) */
static uint32_t s_last_autoconnect;

/* settings overlay widgets */
static lv_obj_t *s_set_overlay;
static lv_obj_t *s_avail_list, *s_saved_list, *s_set_status, *s_scan_spin;
static lv_obj_t *s_pw_panel, *s_pw_ta, *s_pw_ta2, *s_pw_title, *s_kb;
static char s_sel_ssid[33];

static void render_available(void);
static void render_saved(void);
static void set_status2(const char *msg, uint32_t color);

/* ---- garden valve controller client (gaiaforge-valve.local) ---- */
typedef enum { VA_NONE, VA_WATER, VA_STOP, VA_SAVE, VA_DELETE } valve_action_t;
static volatile valve_action_t s_valve_action = VA_NONE;
static int  s_valve_action_zone;
static int  s_valve_action_minutes;
static bool s_mdns_started;
static void fetch_valve_status(void);
static void rebuild_zones(void);

/* ---- node trendline (sparkline) ---- */
#define SPARK_MAX 300
static float s_spark[4][SPARK_MAX];
static int   s_spark_n;
static int   s_spark_metric;
static int   s_spark_hours = 24;
static char  s_detail_eui[24], s_detail_name[24];
static volatile bool s_spark_request;
static lv_obj_t *s_node_overlay, *s_node_title, *s_node_chart, *s_node_status;
static lv_obj_t *s_metric_btns[4], *s_range_btns[2];
static lv_chart_series_t *s_node_series;
static const uint32_t METRIC_COL[4]  = {C_TEMP, C_HUM, C_OK, C_PRES};
static const char *METRIC_NAME[4]     = {"Temperature", "Humidity", "Soil moisture", "Battery"};
static void render_node_chart(void);
static void node_card_cb(lv_event_t *e);
static bool http_get_json(const char *path, JsonDocument &doc);

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
/* ------------------------------------------------------------------ */

/* Deep vertical background gradient (radial hangs the renderer here). */
static void bg_grad(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0C1728), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x05090F), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* Frosted-glass card: top-lit gradient, soft shadow, faint light rim. */
static void style_glass(lv_obj_t *o, int radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1B2942), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x121C30), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0x93A9CC), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_20, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_shadow_width(o, 16, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_y(o, 5, 0);
}

static lv_obj_t *card(lv_obj_t *parent, int w, int h, const char *caption)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    style_glass(c, 16);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (caption) {
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, caption);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 14, 10);
    }
    return c;
}

static lv_obj_t *tile_value(lv_obj_t *c, const char *unit, uint32_t color)
{
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 16, 2);
    lv_obj_t *u = lv_label_create(c);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), 0);
    lv_obj_align(u, LV_ALIGN_LEFT_MID, 18, 42);
    return v;
}

/* LVGL's printf has no %f; format floats with newlib snprintf. */
static void set_num(lv_obj_t *l, const char *fmt, float v)
{
    char b[16];
    snprintf(b, sizeof b, fmt, v);
    lv_label_set_text(l, b);
}

/* Weather-icon glyph in the top-left of a card, colored. */
static void card_icon(lv_obj_t *card, const char *glyph, uint32_t color)
{
    lv_obj_t *ic = lv_label_create(card);
    lv_label_set_text(ic, glyph);
    lv_obj_set_style_text_font(ic, &wi_font_30, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(color), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 14, 8);
}

/* ------------------------------------------------------------------ */
/* Indoor tab                                                          */
/* ------------------------------------------------------------------ */

static void build_indoor(lv_obj_t *tab)
{
    bg_grad(tab);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tc = card(tab, 300, 150, NULL);
    lv_obj_align(tc, LV_ALIGN_TOP_LEFT, 0, 0);
    card_icon(tc, WI_TEMP, C_TEMP);
    s_in_temp = tile_value(tc, "\xC2\xB0""C", C_TEMP);

    lv_obj_t *hc = card(tab, 300, 150, NULL);
    lv_obj_align(hc, LV_ALIGN_TOP_LEFT, 320, 0);
    card_icon(hc, WI_HUM, C_HUM);
    s_in_hum = tile_value(hc, "%RH", C_HUM);

    lv_obj_t *pc = card(tab, 300, 150, NULL);
    lv_obj_align(pc, LV_ALIGN_TOP_LEFT, 640, 0);
    card_icon(pc, WI_PRES, C_PRES);
    s_in_pres = tile_value(pc, "hPa", C_PRES);

    lv_obj_t *dc = card(tab, 300, 150, "DEW POINT");
    lv_obj_align(dc, LV_ALIGN_TOP_LEFT, 0, 166);
    s_in_dew = tile_value(dc, "\xC2\xB0""C", C_HUM);

    lv_obj_t *vc = card(tab, 300, 150, "VPD");
    lv_obj_align(vc, LV_ALIGN_TOP_LEFT, 320, 166);
    s_in_vpd = tile_value(vc, "kPa", C_WARN);

    lv_obj_t *sc = card(tab, 300, 150, "SENSOR");
    lv_obj_align(sc, LV_ALIGN_TOP_LEFT, 640, 166);
    s_in_stat = lv_label_create(sc);
    lv_label_set_text(s_in_stat, "probing...");
    lv_obj_set_style_text_font(s_in_stat, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_in_stat, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_in_stat, LV_ALIGN_LEFT_MID, 16, 8);
}

/* ------------------------------------------------------------------ */
/* Field Hubs tab                                                      */
/* ------------------------------------------------------------------ */

static void build_hubs(lv_obj_t *tab)
{
    bg_grad(tab);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_hub_status = lv_label_create(tab);
    lv_label_set_text(s_hub_status, "Waiting for WiFi...");
    lv_obj_set_style_text_font(s_hub_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hub_status, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_hub_status, LV_ALIGN_TOP_LEFT, 4, 4);

    s_hub_list = lv_obj_create(tab);
    lv_obj_set_size(s_hub_list, LV_PCT(100), LV_PCT(100) - 44);
    lv_obj_align(s_hub_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_hub_list, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(s_hub_list, 0, 0);
    lv_obj_set_style_radius(s_hub_list, 10, 0);
    lv_obj_set_style_pad_all(s_hub_list, 10, 0);
    lv_obj_set_style_pad_row(s_hub_list, 8, 0);
    lv_obj_set_flex_flow(s_hub_list, LV_FLEX_FLOW_COLUMN);
}

/* ------------------------------------------------------------------ */
/* Outside weather tab                                                 */
/* ------------------------------------------------------------------ */

/** WMO weather-interpretation code -> short text. */
static const char *wmo_text(int c)
{
    if (c == 0) return "Clear";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Overcast";
    if (c <= 48) return "Fog";
    if (c <= 57) return "Drizzle";
    if (c <= 67) return "Rain";
    if (c <= 77) return "Snow";
    if (c <= 82) return "Showers";
    if (c <= 86) return "Snow showers";
    return "Thunderstorm";
}

/* WMO code -> full-color condition icon. */
static const lv_image_dsc_t *wmo_icon(int c)
{
    if (c == 0) return &wx_clear;
    if (c <= 2) return &wx_partly;
    if (c == 3) return &wx_cloud;
    if (c <= 48) return &wx_fog;
    if (c <= 67) return &wx_rain;
    if (c <= 77) return &wx_snow;
    if (c <= 82) return &wx_rain;
    if (c <= 86) return &wx_snow;
    return &wx_storm;
}

static void build_outside(lv_obj_t *tab)
{
    bg_grad(tab);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_out_status = lv_label_create(tab);
    lv_label_set_text(s_out_status, "Waiting for location from field nodes...");
    lv_obj_set_style_text_font(s_out_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_out_status, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_out_status, LV_ALIGN_TOP_LEFT, 4, 2);

    s_out_img = lv_image_create(tab);
    lv_image_set_src(s_out_img, &wx_partly);
    lv_obj_align(s_out_img, LV_ALIGN_TOP_RIGHT, -8, -10);

    s_out_cond = lv_label_create(tab);
    lv_label_set_text(s_out_cond, "--");
    lv_obj_set_style_text_font(s_out_cond, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_out_cond, lv_color_hex(C_HUM), 0);
    lv_obj_align(s_out_cond, LV_ALIGN_TOP_RIGHT, -92, 12);

    struct { lv_obj_t **v; const char *icon, *unit; uint32_t col; int x; } t[] = {
        {&s_out_temp, WI_TEMP, "\xC2\xB0""C", C_TEMP, 0},
        {&s_out_hum,  WI_HUM,  "%RH",         C_HUM,  246},
        {&s_out_pres, WI_PRES, "hPa",         C_PRES, 492},
        {&s_out_wind, WI_WIND, "km/h",        C_OK,   738},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = card(tab, 230, 150, NULL);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, t[i].x, 40);
        card_icon(c, t[i].icon, t[i].col);
        *t[i].v = tile_value(c, t[i].unit, t[i].col);
    }

    /* 3-day forecast */
    const char *days[3] = {"TODAY", "TOMORROW", "IN 2 DAYS"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = card(tab, 314, 150, days[i]);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, i * 330, 206);
        s_fc_temp[i] = lv_label_create(c);
        lv_label_set_text(s_fc_temp[i], "--");
        lv_obj_set_style_text_font(s_fc_temp[i], &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_fc_temp[i], lv_color_hex(C_TEXT), 0);
        lv_obj_align(s_fc_temp[i], LV_ALIGN_LEFT_MID, 16, 6);
        s_fc_cond[i] = lv_label_create(c);
        lv_label_set_text(s_fc_cond[i], "");
        lv_obj_set_style_text_font(s_fc_cond[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_fc_cond[i], lv_color_hex(C_MUTED), 0);
        lv_obj_align(s_fc_cond[i], LV_ALIGN_BOTTOM_LEFT, 16, -12);
    }
}

/* Fetch Open-Meteo current + 3-day forecast for the field location (HTTPS). */
static void fetch_weather(void)
{
    if (!s_have_loc) return;

    WiFiClientSecure client;
    client.setInsecure();                 /* skip cert check — public read-only */
    HTTPClient http;
    char url[320];
    snprintf(url, sizeof url,
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,surface_pressure,"
        "wind_speed_10m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min,weather_code"
        "&timezone=auto&forecast_days=3", s_lat, s_lng);

    http.begin(client, url);
    http.setTimeout(6000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("weather: HTTP %d\n", code);
        http.end();
        lvgl_v9_port_lock(-1);
        lv_label_set_text_fmt(s_out_status, "Weather fetch failed (HTTP %d)", code);
        lv_obj_set_style_text_color(s_out_status, lv_color_hex(C_BAD), 0);
        lvgl_v9_port_unlock();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Serial.println("weather: parse error"); return; }

    JsonObject cur = doc["current"];
    float t = cur["temperature_2m"] | 0.0;
    float h = cur["relative_humidity_2m"] | 0.0;
    float p = cur["surface_pressure"] | 0.0;
    float w = cur["wind_speed_10m"] | 0.0;
    int   wc = cur["weather_code"] | 0;

    JsonObject daily = doc["daily"];
    JsonArray dmax = daily["temperature_2m_max"];
    JsonArray dmin = daily["temperature_2m_min"];
    JsonArray dcode = daily["weather_code"];
    bool frost = false;

    lvgl_v9_port_lock(-1);
    set_num(s_out_temp, "%.1f", t);
    set_num(s_out_hum, "%.0f", h);
    set_num(s_out_pres, "%.0f", p);
    set_num(s_out_wind, "%.0f", w);
    lv_label_set_text(s_out_cond, wmo_text(wc));
    lv_image_set_src(s_out_img, wmo_icon(wc));

    for (int i = 0; i < 3 && i < (int)dmax.size(); i++) {
        float lo = dmin[i] | 0.0, hi = dmax[i] | 0.0;
        char buf[24];
        snprintf(buf, sizeof buf, "%.0f / %.0f\xC2\xB0", lo, hi);
        lv_label_set_text(s_fc_temp[i], buf);
        lv_label_set_text(s_fc_cond[i], wmo_text(dcode[i] | 0));
        if (lo < 2.0) frost = true;
    }

    if (frost) {
        lv_label_set_text(s_out_status, LV_SYMBOL_WARNING " Frost in the forecast");
        lv_obj_set_style_text_color(s_out_status, lv_color_hex(C_WARN), 0);
    } else {
        char lb[48];
        snprintf(lb, sizeof lb, "Open-Meteo  -  %.3f, %.3f", s_lat, s_lng);
        lv_label_set_text(s_out_status, lb);
        lv_obj_set_style_text_color(s_out_status, lv_color_hex(C_MUTED), 0);
    }
    lvgl_v9_port_unlock();
    Serial.printf("weather: %.1fC %.0f%% %s\n", t, h, wmo_text(wc));
}

/* --- manual location, persisted in NVS --- */

static void load_location(void)
{
    Preferences p;
    p.begin("mcloc", true);
    double la = p.getDouble("lat", MC_LAT);
    double lo = p.getDouble("lng", MC_LNG);
    p.end();
    if (la != 0.0 || lo != 0.0) {
        s_lat = la; s_lng = lo; s_have_loc = true;
        s_last_weather = millis() - WEATHER_REFRESH_MS;   /* fetch as soon as WiFi is up */
        Serial.printf("location loaded: %.4f, %.4f\n", la, lo);
    }
}

static void save_location(double la, double lo)
{
    Preferences p;
    p.begin("mcloc", false);
    p.putDouble("lat", la);
    p.putDouble("lng", lo);
    p.end();
    s_lat = la; s_lng = lo; s_have_loc = true;
    s_last_weather = millis() - WEATHER_REFRESH_MS;   /* fetch on next loop */
    Serial.printf("location set: %.4f, %.4f\n", la, lo);
}

/* ------------------------------------------------------------------ */
/* Irrigation tab (UI PREVIEW — configure zones; backend is future)    */
/* ------------------------------------------------------------------ */

/* Up to 3 named watering PROFILES ("zones") on the garden valve controller
 * (VALVE_HOST above; see ~/Projects/waveshar_ESP32_experiment/valve-controller).
 * A zone is separate from a physical relay — each zone explicitly picks
 * which relay it drives (z.relay, -1 = unassigned = drives nothing), from
 * whatever the connected device actually reports having (s_device_relays).
 * mode ZMODE_OFF means "unconfigured" (the node's default for an unused slot). */
#define MAX_ZONES  3
#define ZMODE_OFF  3     /* matches valve-controller's MODE_OFF exactly */
#define MAX_RELAYS 3     /* upper bound for the relay-picker dropdown */
#define PUMP_OUTLETS 3   /* must match pump_client.cpp's OUTLET_NAME[] count */
static const char *DAY_ABBR[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

typedef struct {
    char    name[24];
    int     mode;                 /* 0 schedule, 1 threshold, 2 manual, 3 off */
    int     device;               /* 0 = valve-controller relay, 1 = pump-strip outlet */
    int     relay;                /* index within `device`'s own list, -1 = unassigned */
    int     s_hour, s_min;        /* schedule: start time */
    int     e_hour, e_min;        /* schedule: end time */
    uint8_t days;                 /* schedule: bitmask, bit N = day N (0=Sun..6=Sat) */
    int     m_dur;                /* manual: duration (min) */
    char    t_node[24];           /* threshold: field node name */
    int     t_soil, t_dur;        /* threshold: soil% and duration (min) */
    bool    open;                 /* live, from the node's /status */
    int     rem;                  /* seconds left, live from node + local ticking */
    char    last_water[20];       /* "YYYY-MM-DD HH:MM" of last run, any mode, from device */
    int     last_water_min;       /* actual duration of that run */
    char    next_run[24];         /* schedule only: e.g. "Tomorrow 08:00" */
} zone_t;
static zone_t s_zones[MAX_ZONES];
static int s_device_relays;       /* how many relays the CONNECTED device actually has */
static lv_obj_t *s_zone_cont;
static lv_obj_t *s_vpill[MAX_ZONES], *s_vcd[MAX_ZONES];   /* NULL = not rendered */
static lv_obj_t *s_vlast[MAX_ZONES], *s_vnext[MAX_ZONES]; /* NULL = not rendered */
static lv_obj_t *s_irr_status;
static int s_edit_zone = -1;
static bool s_edit_is_new;

/* config overlay widgets */
static lv_obj_t *s_zc_overlay, *s_zc_name, *s_zc_kb;
static lv_obj_t *s_zc_mbtn[3], *s_zc_sched, *s_zc_thresh, *s_zc_manual;
static lv_obj_t *s_zc_relay;
static int s_zc_valve_count;      /* dropdown indices < this = valve relay; else pump outlet */
static lv_obj_t *s_zc_hour, *s_zc_min, *s_zc_ehour, *s_zc_emin, *s_zc_day_btn[7];
static lv_obj_t *s_zc_node, *s_zc_soil, *s_zc_tdur, *s_zc_mdur, *s_zc_delete;
static int s_zc_mode;

static void open_zone_config(int idx);

static void zone_summary(zone_t *z, char *out, size_t n)
{
    char relay_s[16] = "no relay";
    if (z->relay >= 0) {
        if (z->device == 1) snprintf(relay_s, sizeof relay_s, "outlet %d", z->relay + 1);
        else                snprintf(relay_s, sizeof relay_s, "relay %d", z->relay + 1);
    }

    if (z->mode == 0) {
        char days_s[32];
        if (z->days == 0x7F) strlcpy(days_s, "everyday", sizeof days_s);
        else if (z->days == 0) strlcpy(days_s, "no days set", sizeof days_s);
        else {
            days_s[0] = '\0';
            for (int d = 0; d < 7; d++) {
                if (!(z->days & (1 << d))) continue;
                if (days_s[0]) strlcat(days_s, ",", sizeof days_s);
                strlcat(days_s, DAY_ABBR[d], sizeof days_s);
            }
        }
        snprintf(out, n, "Schedule - %02d:%02d-%02d:%02d, %s (%s)",
                 z->s_hour, z->s_min, z->e_hour, z->e_min, days_s, relay_s);
    } else if (z->mode == 1) {
        snprintf(out, n, "Threshold - soil < %d%% (%s), %d min (%s)",
                 z->t_soil, z->t_node[0] ? z->t_node : "?", z->t_dur, relay_s);
    } else {
        snprintf(out, n, "Manual - %d min (%s)", z->m_dur, relay_s);
    }
}

/* Push the live open/countdown state into a zone's card, if it has one. */
static void update_zone(int i)
{
    if (!s_vpill[i]) return;         /* zone not currently rendered (OFF) */
    zone_t *z = &s_zones[i];
    if (z->open) {
        lv_label_set_text(s_vpill[i], LV_SYMBOL_TINT " WATERING");
        lv_obj_set_style_text_color(s_vpill[i], lv_color_hex(C_HUM), 0);
        char b[12];
        snprintf(b, sizeof b, "%d:%02d", z->rem / 60, z->rem % 60);
        lv_label_set_text(s_vcd[i], b);
        lv_obj_set_style_text_color(s_vcd[i], lv_color_hex(C_HUM), 0);
    } else {
        lv_label_set_text(s_vpill[i], "IDLE");
        lv_obj_set_style_text_color(s_vpill[i], lv_color_hex(C_OK), 0);
        lv_label_set_text(s_vcd[i], "--");
        lv_obj_set_style_text_color(s_vcd[i], lv_color_hex(C_MUTED), 0);
    }

    if (s_vlast[i]) {
        char b[48];
        if (z->last_water[0])
            snprintf(b, sizeof b, "Last: %s (%d min)", z->last_water, z->last_water_min);
        else
            snprintf(b, sizeof b, "Last: never");
        lv_label_set_text(s_vlast[i], b);
    }
    if (s_vnext[i]) {
        if (z->mode == 0 && z->next_run[0]) {
            char b[40];
            snprintf(b, sizeof b, "Next: %s", z->next_run);
            lv_label_set_text(s_vnext[i], b);
            lv_obj_remove_flag(s_vnext[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_vnext[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void valve_water_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int mins = s_zones[i].mode == 1 ? s_zones[i].t_dur
              : s_zones[i].mode == 0 ? ((s_zones[i].e_hour * 60 + s_zones[i].e_min) -
                                        (s_zones[i].s_hour * 60 + s_zones[i].s_min))
              : s_zones[i].m_dur;
    if (mins <= 0) mins = 10;
    /* Optimistic UI update; the real POST runs on the loop() task and a
     * status poll shortly after reconciles with the node's actual state. */
    s_zones[i].open = true;
    s_zones[i].rem = mins * 60;
    update_zone(i);
    s_valve_action_zone = i;
    s_valve_action_minutes = mins;
    s_valve_action = VA_WATER;
}

static void valve_stop_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_zones[i].open = false;
    s_zones[i].rem = 0;
    update_zone(i);
    s_valve_action_zone = i;
    s_valve_action = VA_STOP;
}

static void zone_edit_cb(lv_event_t *e)
{
    open_zone_config((int)(intptr_t)lv_event_get_user_data(e));
}

static void zone_add_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    for (int i = 0; i < MAX_ZONES; i++) {
        if (s_zones[i].mode == ZMODE_OFF) { open_zone_config(i); return; }
    }
}

/* Rebuild the zone cards (+ the "add" card) from s_zones. */
static void rebuild_zones(void)
{
    lv_obj_clean(s_zone_cont);
    for (int i = 0; i < MAX_ZONES; i++)
        s_vpill[i] = s_vcd[i] = s_vlast[i] = s_vnext[i] = NULL;
    int configured = 0;

    for (int i = 0; i < MAX_ZONES; i++) {
        zone_t *z = &s_zones[i];
        if (z->mode == ZMODE_OFF) continue;      /* unconfigured slot */
        configured++;
        lv_obj_t *c = lv_obj_create(s_zone_cont);
        lv_obj_set_size(c, 316, 300);
        style_glass(c, 16);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);       /* tap card -> edit */
        lv_obj_add_event_cb(c, zone_edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *nm = lv_label_create(c);
        lv_label_set_text(nm, z->name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 16, 14);

        s_vpill[i] = lv_label_create(c);
        lv_obj_set_style_text_font(s_vpill[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_vpill[i], LV_ALIGN_TOP_RIGHT, -16, 18);

        char sum[64];
        zone_summary(z, sum, sizeof sum);
        lv_obj_t *cfg = lv_label_create(c);
        lv_label_set_text(cfg, sum);
        lv_obj_set_width(cfg, 284);
        lv_label_set_long_mode(cfg, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(cfg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cfg, lv_color_hex(C_MUTED), 0);
        lv_obj_align(cfg, LV_ALIGN_TOP_LEFT, 16, 52);

        /* watering history — last run (any mode) and next run (schedule only) */
        s_vlast[i] = lv_label_create(c);
        lv_label_set_text(s_vlast[i], "Last: never");
        lv_obj_set_style_text_font(s_vlast[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_vlast[i], lv_color_hex(C_MUTED), 0);
        lv_obj_align(s_vlast[i], LV_ALIGN_TOP_LEFT, 16, 96);

        s_vnext[i] = lv_label_create(c);
        lv_label_set_text(s_vnext[i], "");
        lv_obj_set_style_text_font(s_vnext[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_vnext[i], lv_color_hex(C_HUM), 0);
        lv_obj_align(s_vnext[i], LV_ALIGN_TOP_LEFT, 16, 118);
        lv_obj_add_flag(s_vnext[i], LV_OBJ_FLAG_HIDDEN);   /* shown only for schedule zones */

        s_vcd[i] = lv_label_create(c);
        lv_obj_set_style_text_font(s_vcd[i], &lv_font_montserrat_32, 0);
        lv_obj_align(s_vcd[i], LV_ALIGN_TOP_MID, 0, 152);

        lv_obj_t *w = lv_button_create(c);
        lv_obj_set_size(w, 150, 50);
        lv_obj_align(w, LV_ALIGN_BOTTOM_LEFT, 12, -12);
        lv_obj_set_style_bg_color(w, lv_color_hex(C_HUM), 0);
        lv_obj_add_event_cb(w, valve_water_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *wl = lv_label_create(w);
        lv_label_set_text(wl, LV_SYMBOL_TINT " Irrigate");
        lv_obj_center(wl);

        lv_obj_t *st = lv_button_create(c);
        lv_obj_set_size(st, 120, 50);
        lv_obj_align(st, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
        lv_obj_set_style_bg_color(st, lv_color_hex(C_INNER), 0);
        lv_obj_add_event_cb(st, valve_stop_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *sl = lv_label_create(st);
        lv_label_set_text(sl, LV_SYMBOL_STOP " Stop");
        lv_obj_center(sl);

        update_zone(i);
    }

    /* "+" add-zone card, for the next unconfigured relay slot */
    if (configured < MAX_ZONES) {
        lv_obj_t *add = lv_obj_create(s_zone_cont);
        lv_obj_set_size(add, 316, 300);
        lv_obj_set_style_bg_color(add, lv_color_hex(0x0E1524), 0);
        lv_obj_set_style_border_width(add, 2, 0);
        lv_obj_set_style_border_color(add, lv_color_hex(C_INNER), 0);
        lv_obj_set_style_radius(add, 12, 0);
        lv_obj_remove_flag(add, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(add, zone_add_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *plus = lv_label_create(add);
        lv_label_set_text(plus, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(plus, lv_color_hex(C_MUTED), 0);
        lv_obj_align(plus, LV_ALIGN_CENTER, 0, -14);
        lv_obj_t *al = lv_label_create(add);
        lv_label_set_text(al, "Add zone");
        lv_obj_set_style_text_color(al, lv_color_hex(C_MUTED), 0);
        lv_obj_align(al, LV_ALIGN_CENTER, 0, 40);
    }
}

static void build_irrigation(lv_obj_t *tab)
{
    bg_grad(tab);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    /* Real connection status to the standalone garden valve controller —
     * makes it explicit this tab talks to separate hardware, not the panel. */
    s_irr_status = lv_label_create(tab);
    lv_label_set_text(s_irr_status, LV_SYMBOL_WARNING "  Connecting to "
                      VALVE_HOST "...");
    lv_obj_set_style_text_font(s_irr_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_irr_status, lv_color_hex(C_WARN), 0);
    lv_obj_align(s_irr_status, LV_ALIGN_TOP_LEFT, 4, 2);

    s_zone_cont = lv_obj_create(tab);
    lv_obj_set_size(s_zone_cont, LV_PCT(100), LV_PCT(100) - 30);
    lv_obj_align(s_zone_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_zone_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zone_cont, 0, 0);
    lv_obj_set_style_pad_all(s_zone_cont, 0, 0);
    lv_obj_set_style_pad_row(s_zone_cont, 12, 0);
    lv_obj_set_style_pad_column(s_zone_cont, 12, 0);
    lv_obj_set_flex_flow(s_zone_cont, LV_FLEX_FLOW_ROW_WRAP);

    /* All 3 relay slots start OFF (unconfigured) until the first real
     * /status fetch from the valve node reports what's actually there. */
    for (int i = 0; i < MAX_ZONES; i++) {
        s_zones[i].mode = ZMODE_OFF;
        s_zones[i].relay = -1;
        s_zones[i].days = 0x7F;      /* everyday, matches the device's own default */
        s_zones[i].m_dur = 10;
    }
    rebuild_zones();
}

/* ------------------------------------------------------------------ */
/* Node trendline                                                      */
/* ------------------------------------------------------------------ */

/* "2026-07-25T13:43:11.95..." -> "07-25 13:43" for compact display. */
static void pretty_time(const char *iso, char *out, size_t n)
{
    /* iso is YYYY-MM-DDTHH:MM:SS...; take MM-DD and HH:MM */
    if (strlen(iso) >= 16 && iso[4] == '-' && iso[10] == 'T') {
        snprintf(out, n, "%.5s %.5s", iso + 5, iso + 11);   /* MM-DD HH:MM */
    } else {
        strlcpy(out, iso, n);
    }
}

/* Downsample one JSON array into s_spark[metric][], returns point count. */
static int fill_spark(JsonArray a, int metric, int stride)
{
    int n = 0, idx = 0;
    for (JsonVariant v : a) {
        if (idx % stride == 0 && n < SPARK_MAX) s_spark[metric][n++] = v.as<float>();
        idx++;
    }
    return n;
}

static void fetch_sparkline(void)
{
    char path[80];
    snprintf(path, sizeof path, "/api/nodes/%s/sparkline?hours=%d",
             s_detail_eui, s_spark_hours);
    JsonDocument doc;
    if (!http_get_json(path, doc)) {
        s_spark_n = 0;
        Serial.printf("sparkline %s: fetch failed\n", s_detail_eui);
        render_node_chart();
        return;
    }
    JsonArray at = doc["temperature"].as<JsonArray>();
    int count = at.size();
    int stride = count > SPARK_MAX ? (count + SPARK_MAX - 1) / SPARK_MAX : 1;
    s_spark_n = fill_spark(doc["temperature"].as<JsonArray>(), 0, stride);
    fill_spark(doc["humidity"].as<JsonArray>(), 1, stride);
    fill_spark(doc["soil_moisture"].as<JsonArray>(), 2, stride);
    fill_spark(doc["battery"].as<JsonArray>(), 3, stride);
    Serial.printf("sparkline %s: %d pts (of %d, stride %d)\n",
                  s_detail_eui, s_spark_n, count, stride);
    render_node_chart();
}

/* Render the chart for the selected metric from the cached arrays. */
static void render_node_chart(void)
{
    lvgl_v9_port_lock(-1);
    int m = s_spark_metric;

    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < s_spark_n; i++) {
        float v = s_spark[m][i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (s_spark_n == 0) { mn = 0; mx = 1; }

    lv_chart_set_series_color(s_node_chart, s_node_series, lv_color_hex(METRIC_COL[m]));
    lv_chart_set_point_count(s_node_chart, s_spark_n > 0 ? s_spark_n : 1);
    lv_chart_set_axis_range(s_node_chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int)(mn * 10) - 3, (int)(mx * 10) + 3);

    static int32_t vals[SPARK_MAX];
    for (int i = 0; i < s_spark_n; i++) vals[i] = (int32_t)(s_spark[m][i] * 10);
    if (s_spark_n > 0)
        lv_chart_set_series_values(s_node_chart, s_node_series, vals, s_spark_n);
    lv_chart_refresh(s_node_chart);

    char st[96];
    if (s_spark_n > 0)
        snprintf(st, sizeof st, "%s  -  %dh  -  min %.1f   max %.1f   now %.1f   (%d pts)",
                 METRIC_NAME[m], s_spark_hours, mn, mx, s_spark[m][s_spark_n - 1], s_spark_n);
    else
        snprintf(st, sizeof st, "No history yet for this node");
    lv_label_set_text(s_node_status, st);

    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(s_metric_btns[i],
            lv_color_hex(i == m ? METRIC_COL[i] : C_INNER), 0);
    for (int i = 0; i < 2; i++) {
        int hrs = i == 0 ? 24 : 168;
        lv_obj_set_style_bg_color(s_range_btns[i],
            lv_color_hex(s_spark_hours == hrs ? C_ACCENT : C_INNER), 0);
    }
    lv_obj_update_layout(s_node_chart);
    lvgl_v9_port_unlock();
}

/** Render the cached hub list. Takes the LVGL lock itself (called from loop). */
static void render_hubs(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_hub_list);

    if (s_hub_count == 0) {
        lv_obj_t *l = lv_label_create(s_hub_list);
        lv_label_set_text(l, s_fetch_failed ? "Fetch failed — see status above"
                                            : "No nodes reported");
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    }

    for (int i = 0; i < s_hub_count; i++) {
        hub_t *h = &s_hubs[i];
        uint32_t edge = h->severity == 2 ? C_BAD :
                        h->severity == 1 ? C_WARN :
                        (h->rssi < -110 ? C_BAD : C_OK);

        lv_obj_t *row = lv_obj_create(s_hub_list);
        lv_obj_set_size(row, LV_PCT(100), 84);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1B2942), 0);
        lv_obj_set_style_bg_grad_color(row, lv_color_hex(0x121C30), 0);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_INNER), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(row, 12, 0);
        lv_obj_set_style_shadow_color(row, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_shadow_offset_y(row, 4, 0);
        lv_obj_set_style_border_width(row, 3, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(edge), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);       /* tap -> trendline */
        lv_obj_add_event_cb(row, node_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, h->name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 16, 10);

        char seen[24];
        pretty_time(h->last_seen, seen, sizeof seen);
        char sub[64];
        snprintf(sub, sizeof sub, "%.0f dBm  -  seen %s  -  tap for trend", h->rssi, seen);
        lv_obj_t *sl = lv_label_create(row);
        lv_label_set_text(sl, sub);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(C_MUTED), 0);
        lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 16, -10);

        /* Readings, right-aligned in a compact strip. */
        char rd[96];
        snprintf(rd, sizeof rd, "%.1f\xC2\xB0""C   %.0f%%   %.2f kPa   soil %.0f%%   bat %.0f%%",
                 h->temp, h->hum, h->vpd, h->soil, h->batt);
        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text(rl, rd);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(C_TEXT), 0);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -16, h->warn[0] ? -14 : 0);

        /* Warning line (frost / alert) under the readings. */
        if (h->warn[0]) {
            lv_obj_t *wl = lv_label_create(row);
            lv_label_set_text(wl, h->warn);
            lv_obj_set_style_text_font(wl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(wl,
                lv_color_hex(h->severity == 2 ? C_BAD : C_WARN), 0);
            lv_obj_align(wl, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
        }
    }

    char st[96];
    if (s_fetch_failed) {
        snprintf(st, sizeof st, "%s  -  last fetch failed (HTTP %d)",
                 MC_SERVER, s_last_http);
        lv_obj_set_style_text_color(s_hub_status, lv_color_hex(C_BAD), 0);
    } else if (s_alert_count > 0) {
        snprintf(st, sizeof st, LV_SYMBOL_WARNING " %d node%s need attention  -  %d nodes total",
                 s_alert_count, s_alert_count == 1 ? "" : "s", s_hub_count);
        lv_obj_set_style_text_color(s_hub_status, lv_color_hex(C_BAD), 0);
    } else {
        snprintf(st, sizeof st, "%s  -  %d nodes  -  all clear  -  refreshing 30s",
                 MC_SERVER, s_hub_count);
        lv_obj_set_style_text_color(s_hub_status, lv_color_hex(C_OK), 0);
    }
    lv_label_set_text(s_hub_status, st);
    lv_obj_update_layout(s_hub_list);      /* rebuilt off-cycle; force layout */
    lvgl_v9_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Timers (LVGL task): sensor + clock + WiFi pill                      */
/* ------------------------------------------------------------------ */

static void tick(lv_timer_t *t)
{
    LV_UNUSED(t);

    /* Smooth local countdown between the real 5s status polls from the
     * valve node; the next poll always overwrites this with ground truth. */
    for (int i = 0; i < MAX_ZONES; i++) {
        if (s_zones[i].open && s_zones[i].rem > 0) {
            if (--s_zones[i].rem == 0) s_zones[i].open = false;
            update_zone(i);
        }
    }

    /* Indoor sensor */
    if (!s_bme_ok) {
        s_bme_ok = s_bme.begin(0);
        if (s_bme_ok) lv_label_set_text(s_in_stat, "BME280 LIVE  -  0x76");
    } else {
        float tC, rh, hPa;
        if (s_bme.read(tC, rh, hPa)) {
            set_num(s_in_temp, "%.1f", tC);
            set_num(s_in_hum, "%.0f", rh);
            set_num(s_in_pres, "%.0f", hPa);
            double g = log(rh / 100.0) + 17.62 * tC / (243.12 + tC);
            set_num(s_in_dew, "%.1f", (float)(243.12 * g / (17.62 - g)));
            double svp = 0.6108 * exp(17.27 * tC / (tC + 237.3));
            set_num(s_in_vpd, "%.2f", (float)(svp * (1.0 - rh / 100.0)));
        }
    }

    /* WiFi pill */
    if (s_wifi_up) {
        lv_label_set_text_fmt(s_wifi_lbl, LV_SYMBOL_WIFI "  %s",
                              WiFi.localIP().toString().c_str());
        lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(C_OK), 0);
    } else {
        lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI "  connecting...");
        lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(C_WARN), 0);
    }

    /* Clock from NTP (blank until synced) */
    struct tm tm;
    if (getLocalTime(&tm, 0)) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d",
                              tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    /* New hub data waiting? render it (we're already on the LVGL task, but
     * render_hubs takes the recursive lock which is safe). */
    if (s_hubs_dirty) {
        s_hubs_dirty = false;
        render_hubs();
    }
}

/* ------------------------------------------------------------------ */
/* loop() side: WiFi + blocking hub fetch                              */
/* ------------------------------------------------------------------ */

/* GET a JSON endpoint into doc. Returns true on HTTP 200 + valid JSON. */
static bool http_get_json(const char *path, JsonDocument &doc)
{
    HTTPClient http;
    http.begin(String(MC_SERVER) + path);
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    int code = http.GET();
    s_last_http = code;
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    return deserializeJson(doc, body) == DeserializationError::Ok;
}

/* ------------------------------------------------------------------ */
/* Garden valve controller client — a separate standalone ESP32 on the same
 * WiFi, reached directly by mDNS name (not via the MicroClimate server).
 * See ~/Projects/waveshar_ESP32_experiment/valve-controller.               */
/* ------------------------------------------------------------------ */

/** POST to the valve node with optional zone/minutes query args. */
static bool valve_post(const char *path, int zone, int minutes)
{
    HTTPClient http;
    String url = String("http://") + VALVE_HOST + path;
    if (zone >= 0) {
        url += "?zone=" + String(zone);
        if (minutes >= 0) url += "&minutes=" + String(minutes);
    }
    http.begin(url);
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    int code = http.POST("");
    http.end();
    Serial.printf("valve POST %s -> %d\n", url.c_str(), code);
    return code == 200;
}

/** Push the full 3-slot config (name/mode/schedule/threshold) to the node. */
static bool valve_push_config(void)
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
        zone_t *z = &s_zones[i];
        JsonObject o = arr.add<JsonObject>();
        o["name"] = z->name;
        o["mode"] = z->mode;
        o["device"] = z->device;
        o["relay"] = z->relay;
        o["s_hour"] = z->s_hour; o["s_min"] = z->s_min;
        o["e_hour"] = z->e_hour; o["e_min"] = z->e_min;
        o["days"] = z->days;
        o["m_dur"] = z->m_dur;
        o["t_node"] = z->t_node; o["t_soil"] = z->t_soil; o["t_dur"] = z->t_dur;
    }
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(String("http://") + VALVE_HOST + "/config");
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    int code = http.POST(body);
    http.end();
    Serial.printf("valve config push -> %d\n", code);
    return code == 200;
}

/** Render the connection-status line + refresh any rendered zone cards. */
static void render_valve_status(void)
{
    lvgl_v9_port_lock(-1);
    if (s_valve_online) {
        lv_label_set_text(s_irr_status, LV_SYMBOL_OK "  " VALVE_HOST "  -  connected");
        lv_obj_set_style_text_color(s_irr_status, lv_color_hex(C_OK), 0);
    } else {
        lv_label_set_text(s_irr_status, LV_SYMBOL_WARNING "  " VALVE_HOST
                          "  -  offline (check garden unit power/WiFi)");
        lv_obj_set_style_text_color(s_irr_status, lv_color_hex(C_BAD), 0);
    }
    for (int i = 0; i < MAX_ZONES; i++) update_zone(i);
    lvgl_v9_port_unlock();
}

/** GET /status from the valve node: live open/countdown + its own config
 *  (kept authoritative — the node is the source of truth for what it's
 *  actually running, in case it was configured, rebooted, or edited some
 *  other way since the panel last touched it). */
static void fetch_valve_status(void)
{
    HTTPClient http;
    http.begin(String("http://") + VALVE_HOST + "/status");
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("valve status: unreachable (HTTP %d)\n", code);
        http.end();
        s_valve_online = false;
        render_valve_status();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("valve status: parse error");
        s_valve_online = false; render_valve_status(); return;
    }

    s_device_relays = doc["num_relays"] | s_device_relays;

    int i = 0;
    bool need_rebuild = false;
    for (JsonObject o : doc["zones"].as<JsonArray>()) {
        if (i >= MAX_ZONES) break;
        zone_t *z = &s_zones[i];
        bool was_configured = (z->mode != ZMODE_OFF);
        strlcpy(z->name, o["name"] | z->name, sizeof z->name);
        z->mode = o["mode"] | z->mode;
        z->device = o["device"] | z->device;
        z->relay = o["relay"] | z->relay;
        z->open = o["open"] | false;
        z->rem  = o["secs_left"] | 0;
        z->s_hour = o["s_hour"] | z->s_hour;
        z->s_min  = o["s_min"]  | z->s_min;
        z->e_hour = o["e_hour"] | z->e_hour;
        z->e_min  = o["e_min"]  | z->e_min;
        z->days   = o["days"]   | z->days;
        z->m_dur  = o["m_dur"]  | z->m_dur;
        strlcpy(z->t_node, o["t_node"] | z->t_node, sizeof z->t_node);
        z->t_soil = o["t_soil"] | z->t_soil;
        z->t_dur  = o["t_dur"]  | z->t_dur;
        strlcpy(z->last_water, o["last_water"] | z->last_water, sizeof z->last_water);
        z->last_water_min = o["last_water_min"] | z->last_water_min;
        strlcpy(z->next_run, o["next_run"] | z->next_run, sizeof z->next_run);
        if (was_configured != (z->mode != ZMODE_OFF)) need_rebuild = true;
        i++;
    }
    s_valve_online = true;
    int configured = 0;
    for (int k = 0; k < MAX_ZONES; k++) if (s_zones[k].mode != ZMODE_OFF) configured++;
    Serial.printf("valve status: online, %d/%d zones configured\n", configured, MAX_ZONES);
    lvgl_v9_port_lock(-1);
    if (need_rebuild) rebuild_zones();
    lvgl_v9_port_unlock();
    render_valve_status();
}

/* Find a cached hub by dev_eui, or NULL. */
static hub_t *hub_by_eui(const char *eui)
{
    for (int i = 0; i < s_hub_count; i++) {
        if (strcmp(s_hubs[i].dev_eui, eui) == 0) return &s_hubs[i];
    }
    return NULL;
}

static void fetch_hubs(void)
{
    /* 1) live nodes */
    JsonDocument nodes;
    if (!http_get_json("/api/nodes", nodes)) {
        Serial.printf("hub fetch: failed (HTTP %d)\n", s_last_http);
        s_fetch_failed = true;
        s_hub_count = 0;
        s_alert_count = 0;
        s_hubs_dirty = true;
        return;
    }

    int n = 0;
    for (JsonObject node : nodes.as<JsonArray>()) {
        if (n >= MAX_HUBS) break;
        hub_t *h = &s_hubs[n];
        strlcpy(h->name, node["name"] | "(unnamed)", sizeof h->name);
        strlcpy(h->dev_eui, node["dev_eui"] | "", sizeof h->dev_eui);
        strlcpy(h->last_seen, node["last_seen"] | "?", sizeof h->last_seen);
        h->rssi = node["rssi"] | 0.0;
        JsonObject s = node["sensors"];
        h->temp = s["temperature"] | 0.0;
        h->hum  = s["humidity"] | 0.0;
        h->pres = s["pressure"] | 0.0;
        h->vpd  = s["vpd"] | 0.0;
        h->soil = s["soil_moisture"] | 0.0;
        h->batt = s["battery"] | 0.0;
        h->severity = 0;
        h->warn[0] = '\0';
        if (n == 0 && !s_have_loc) {          /* locate weather from a field node */
            double la = node["lat"] | 0.0, lo = node["lng"] | 0.0;
            if (la != 0.0 || lo != 0.0) {
                s_lat = la; s_lng = lo; s_have_loc = true;
                s_last_weather = millis() - WEATHER_REFRESH_MS;  /* fetch now */
            }
        }
        n++;
    }
    s_hub_count = n;
    s_fetch_failed = false;

    /* 2) frost risk — caution (amber) overlay */
    JsonDocument frost;
    if (http_get_json("/api/stats/frost-risk", frost)) {
        for (JsonObject fr : frost.as<JsonArray>()) {
            const char *level = fr["risk_level"] | "none";
            if (!strcmp(level, "none") || !strcmp(level, "None") || !level[0]) continue;
            hub_t *h = hub_by_eui(fr["dev_eui"] | "");
            if (!h) continue;
            h->severity = h->severity < 1 ? 1 : h->severity;
            if (!fr["hours_to_frost"].isNull()) {
                snprintf(h->warn, sizeof h->warn, LV_SYMBOL_WARNING " Frost ~%.0fh",
                         (double)(fr["hours_to_frost"] | 0.0));
            } else {
                snprintf(h->warn, sizeof h->warn, LV_SYMBOL_WARNING " Frost risk");
            }
        }
    }

    /* 3) alerts — /api/alerts is the alert_history log (newest first, LIMIT
     * 100), NOT current state. So consider only each node's LATEST alert: if
     * that one is unacknowledged, the node needs attention. Dedupe by dev_eui
     * (first occurrence = newest), which bounds the count to the node count. */
    JsonDocument alerts;
    int flagged = 0;
    if (http_get_json("/api/alerts", alerts)) {
        char seen[MAX_HUBS][24];
        int seen_n = 0;
        for (JsonObject a : alerts.as<JsonArray>()) {
            const char *eui = a["dev_eui"] | "";
            if (!eui[0]) continue;
            bool dup = false;
            for (int k = 0; k < seen_n; k++)
                if (!strcmp(seen[k], eui)) { dup = true; break; }
            if (dup) continue;                    /* older alert for this node */
            if (seen_n < MAX_HUBS) strlcpy(seen[seen_n++], eui, 24);

            if (a["acknowledged"] | false) continue;   /* latest already handled */
            hub_t *h = hub_by_eui(eui);
            if (!h) continue;
            h->severity = 2;
            snprintf(h->warn, sizeof h->warn, LV_SYMBOL_WARNING " %s",
                     (const char *)(a["message"] | "alert"));
            flagged++;
        }
    }
    s_alert_count = flagged;

    Serial.printf("hub fetch: %d nodes, %d need attention\n", n, flagged);
    s_hubs_dirty = true;
}

/* Blocking WiFi scan; copy results into s_scan[] for the UI. */
static void do_scan(void)
{
    int n = WiFi.scanNetworks(false, false);
    if (n < 0) n = 0;
    if (n > (int)(sizeof(s_scan) / sizeof(s_scan[0]))) n = sizeof(s_scan) / sizeof(s_scan[0]);
    for (int i = 0; i < n; i++) {
        strlcpy(s_scan[i].ssid, WiFi.SSID(i).c_str(), sizeof s_scan[i].ssid);
        s_scan[i].rssi = WiFi.RSSI(i);
        s_scan[i].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    s_scan_count = n;
    WiFi.scanDelete();
    Serial.printf("wifi scan: %d networks\n", n);
    render_available();
    lv_obj_add_flag(s_scan_spin, LV_OBJ_FLAG_HIDDEN);
    set_status2(s_wifi_up ? WiFi.SSID().c_str() : "not connected",
                s_wifi_up ? C_OK : C_MUTED);
}

/* Blocking connect to the pending network; save on success. */
static void do_connect(void)
{
    Serial.printf("WiFi: connecting to %s\n", s_pending_ssid);
    WiFi.disconnect();
    WiFi.begin(s_pending_ssid, s_pending_pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 9000) delay(100);

    if (WiFi.status() == WL_CONNECTED) {
        s_store.add(s_pending_ssid, s_pending_pass);   /* remember it */
        Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
        set_status2(WiFi.SSID().c_str(), C_OK);
        s_last_fetch = millis() - HUB_REFRESH_MS;       /* refresh hubs soon */
    } else {
        Serial.println("WiFi: connect failed");
        set_status2("Connect failed — wrong password?", C_BAD);
    }
    render_saved();
}

void mc_dash_loop(void)
{
    if (!s_wifi_started) {
        s_wifi_started = true;
        WiFi.mode(WIFI_STA);
        configTime(MC_GMT_OFFSET, 0, "pool.ntp.org");
        if (s_store.count() > 0) {
            WiFi.begin(s_store.get(0).ssid, s_store.get(0).pass);
            Serial.printf("WiFi: auto-connecting to %s\n", s_store.get(0).ssid);
            s_last_autoconnect = millis();
        }
    }

    /* Handle a UI-requested scan or connect (both block). */
    if (s_action == WA_SCAN) {
        do_scan();
        s_action = WA_NONE;
    } else if (s_action == WA_CONNECT) {
        do_connect();
        s_action = WA_NONE;
    }

    bool up = (WiFi.status() == WL_CONNECTED);
    if (up != s_wifi_up) {
        s_wifi_up = up;
        if (up) {
            Serial.printf("WiFi: up, IP %s\n", WiFi.localIP().toString().c_str());
            s_last_fetch = millis() - HUB_REFRESH_MS;
            s_last_valve_poll = millis() - VALVE_REFRESH_MS;   /* poll it now */
            if (!s_mdns_started) {
                /* Needed to resolve gaiaforge-valve.local (the garden unit). */
                s_mdns_started = MDNS.begin("gaiaforge-panel");
            }
        } else {
            Serial.println("WiFi: down");
            s_valve_online = false;
            render_valve_status();
        }
    }

    /* Auto-reconnect to the last-good network if we drop and nothing's pending. */
    if (!s_wifi_up && s_action == WA_NONE && s_store.count() > 0 &&
        millis() - s_last_autoconnect > 12000) {
        WiFi.begin(s_store.get(0).ssid, s_store.get(0).pass);
        s_last_autoconnect = millis();
    }

    /* Node trendline fetch, on demand (tap or range change). */
    if (s_spark_request && s_wifi_up) {
        s_spark_request = false;
        fetch_sparkline();
    }

    if (s_wifi_up && s_action == WA_NONE && (millis() - s_last_fetch >= HUB_REFRESH_MS)) {
        s_last_fetch = millis();
        fetch_hubs();
    }

    if (s_wifi_up && s_have_loc && s_action == WA_NONE &&
        (millis() - s_last_weather >= WEATHER_REFRESH_MS)) {
        s_last_weather = millis();
        fetch_weather();
    }

    /* Garden valve controller — a separate device, own polling/actions. */
    if (s_wifi_up) {
        if (s_valve_action != VA_NONE) {
            valve_action_t act = s_valve_action;
            int zone = s_valve_action_zone, mins = s_valve_action_minutes;
            s_valve_action = VA_NONE;
            switch (act) {
                case VA_WATER:  valve_post("/water", zone, mins); break;
                case VA_STOP:   valve_post("/stop", zone, -1); break;
                case VA_SAVE:   valve_push_config(); break;
                case VA_DELETE: valve_post("/stop", zone, -1);
                                 valve_push_config(); break;
                default: break;
            }
            fetch_valve_status();               /* reconcile immediately */
            s_last_valve_poll = millis();
        } else if (millis() - s_last_valve_poll >= VALVE_REFRESH_MS) {
            s_last_valve_poll = millis();
            fetch_valve_status();
        }
    }

    /* Nous A5T pump/appliance strip — separate mains-rated hardware. */
    pump_client_loop();
}

/* ------------------------------------------------------------------ */

/* ================================================================== */
/* Settings overlay — WiFi scan / connect / saved networks            */
/* ================================================================== */

static void set_status2(const char *msg, uint32_t color)
{
    lv_label_set_text(s_set_status, msg);
    lv_obj_set_style_text_color(s_set_status, lv_color_hex(color), 0);
}

/* --- password entry dialog --- */

static void pw_connect_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    if (s_dialog_mode == 1) {                 /* location entry (two fields) */
        double la = atof(lv_textarea_get_text(s_pw_ta));
        double lo = atof(lv_textarea_get_text(s_pw_ta2));
        if (la != 0 || lo != 0) {
            save_location(la, lo);
            char b[48];
            snprintf(b, sizeof b, "Location:  %.4f, %.4f", la, lo);
            lv_label_set_text(s_loc_lbl, b);
        } else {
            lv_label_set_text(s_loc_lbl, "Invalid — enter latitude and longitude");
        }
        return;
    }

    if (s_dialog_mode == 2) {                 /* pump strip address */
        const char *host = lv_textarea_get_text(s_pw_ta);
        if (host[0]) {
            pump_client_set_host(host);
            char b[80];
            snprintf(b, sizeof b, "Pump strip:  %s", host);
            lv_label_set_text(s_pump_lbl, b);
        }
        return;
    }

    /* wifi password */
    strlcpy(s_pending_ssid, s_sel_ssid, sizeof s_pending_ssid);
    strlcpy(s_pending_pass, lv_textarea_get_text(s_pw_ta), sizeof s_pending_pass);
    set_status2("Connecting...", C_WARN);
    s_action = WA_CONNECT;
}

static void pw_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* Keep the keyboard pointed at whichever field was tapped. */
static void ta_focus_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(s_kb, (lv_obj_t *)lv_event_get_target(e));
}

static void show_dialog(void)
{
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pw_panel);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void open_pw_dialog(const char *ssid)
{
    s_dialog_mode = 0;
    strlcpy(s_sel_ssid, ssid, sizeof s_sel_ssid);
    lv_label_set_text_fmt(s_pw_title, "Password for %s", ssid);

    /* single full-width text field; hide the second field */
    lv_obj_add_flag(s_pw_ta2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_pw_ta, 560, 56);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 54);
    lv_textarea_set_placeholder_text(s_pw_ta, "network password");
    lv_textarea_set_text(s_pw_ta, "");

    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_pw_ta);
    show_dialog();
}

static void open_loc_dialog(void)
{
    s_dialog_mode = 1;
    lv_label_set_text(s_pw_title, "Latitude          Longitude   (e.g. 52.5200  13.4050)");

    /* two side-by-side numeric fields with a clear gap */
    lv_obj_set_size(s_pw_ta, 250, 56);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_LEFT, 16, 54);
    lv_textarea_set_placeholder_text(s_pw_ta, "latitude");
    lv_obj_set_size(s_pw_ta2, 250, 56);
    lv_obj_align(s_pw_ta2, LV_ALIGN_TOP_RIGHT, -16, 54);
    lv_obj_remove_flag(s_pw_ta2, LV_OBJ_FLAG_HIDDEN);

    char b[24] = "";
    if (s_have_loc) snprintf(b, sizeof b, "%.4f", s_lat);
    lv_textarea_set_text(s_pw_ta, b);
    b[0] = '\0';
    if (s_have_loc) snprintf(b, sizeof b, "%.4f", s_lng);
    lv_textarea_set_text(s_pw_ta2, b);

    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_kb, s_pw_ta);
    show_dialog();
}

static void loc_edit_cb(lv_event_t *e) { LV_UNUSED(e); open_loc_dialog(); }

static void open_pump_dialog(void)
{
    s_dialog_mode = 2;
    lv_label_set_text(s_pw_title, "Pump strip address (IP or hostname)");

    lv_obj_add_flag(s_pw_ta2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_pw_ta, 560, 56);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 54);
    lv_textarea_set_placeholder_text(s_pw_ta, "192.168.1.xxx");
    lv_textarea_set_text(s_pw_ta, pump_client_get_host());

    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_pw_ta);
    show_dialog();
}

static void pump_edit_cb(lv_event_t *e) { LV_UNUSED(e); open_pump_dialog(); }

/* --- connect helpers --- */

static void connect_now(const char *ssid, const char *pass)
{
    strlcpy(s_pending_ssid, ssid, sizeof s_pending_ssid);
    strlcpy(s_pending_pass, pass, sizeof s_pending_pass);
    set_status2("Connecting...", C_WARN);
    s_action = WA_CONNECT;
}

static void avail_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_scan_count) return;
    ap_t *ap = &s_scan[i];

    char saved_pw[65];
    if (s_store.find(ap->ssid, saved_pw)) {
        connect_now(ap->ssid, saved_pw);       /* known — reuse password */
    } else if (ap->secured) {
        open_pw_dialog(ap->ssid);              /* ask for password */
    } else {
        connect_now(ap->ssid, "");             /* open network */
    }
}

static void saved_connect_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_store.count()) return;
    connect_now(s_store.get(i).ssid, s_store.get(i).pass);
}

static void saved_forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_store.count()) return;
    s_store.remove(s_store.get(i).ssid);
    render_saved();
}

static void scan_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_action != WA_NONE) return;
    lv_obj_remove_flag(s_scan_spin, LV_OBJ_FLAG_HIDDEN);
    set_status2("Scanning...", C_WARN);
    s_action = WA_SCAN;
}

static void gear_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    render_saved();
    set_status2(s_wifi_up ? WiFi.SSID().c_str() : "not connected",
                s_wifi_up ? C_OK : C_MUTED);
    lv_obj_remove_flag(s_set_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_overlay);
}

static void settings_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_set_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* --- list rendering --- */

/* Bare clickable row container; callers add their own content. */
static lv_obj_t *net_row(lv_obj_t *list, uint32_t edge, lv_event_cb_t cb, int idx)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_INNER), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(edge), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
    return row;
}

/* SSID label bounded to a width so long names ellipsize instead of clipping. */
static lv_obj_t *ssid_label(lv_obj_t *row, const char *ssid, int width)
{
    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, ssid);
    lv_obj_set_width(nm, width);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);
    return nm;
}

static void render_available(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_avail_list);
    if (s_scan_count == 0) {
        lv_obj_t *l = lv_label_create(s_avail_list);
        lv_label_set_text(l, "Tap SCAN to find networks");
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    }
    for (int i = 0; i < s_scan_count; i++) {
        char tmp[65];
        bool known = s_store.find(s_scan[i].ssid, tmp);
        lv_obj_t *row = net_row(s_avail_list, known ? C_OK : C_MUTED, avail_row_cb, i);

        ssid_label(row, s_scan[i].ssid, 250);           /* room for signal */

        char right[28];
        snprintf(right, sizeof right, "%s%d dBm",
                 s_scan[i].secured ? LV_SYMBOL_EYE_CLOSE "  " : "", s_scan[i].rssi);
        lv_obj_t *r = lv_label_create(row);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(C_MUTED), 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    /* Force layout now: rows are added from the loop() task, so without this
     * their %-widths and SSID ellipsis stay stale until a tab switch. */
    lv_obj_update_layout(s_avail_list);
    lvgl_v9_port_unlock();
}

static void render_saved(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_saved_list);
    if (s_store.count() == 0) {
        lv_obj_t *l = lv_label_create(s_saved_list);
        lv_label_set_text(l, "No saved networks yet");
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    }
    for (int i = 0; i < s_store.count(); i++) {
        bool current = s_wifi_up && WiFi.SSID() == s_store.get(i).ssid;
        lv_obj_t *row = net_row(s_saved_list, current ? C_OK : C_INNER,
                                saved_connect_cb, i);

        ssid_label(row, s_store.get(i).ssid, 230);      /* room for status + trash */

        /* forget button at the far right */
        lv_obj_t *fb = lv_button_create(row);
        lv_obj_set_size(fb, 44, 40);
        lv_obj_align(fb, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_color(fb, lv_color_hex(C_BAD), 0);
        lv_obj_set_style_radius(fb, 8, 0);
        lv_obj_add_event_cb(fb, saved_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *fx = lv_label_create(fb);
        lv_label_set_text(fx, LV_SYMBOL_TRASH);
        lv_obj_center(fx);

        /* status text, left of the forget button (no overlap) */
        lv_obj_t *st = lv_label_create(row);
        lv_label_set_text(st, current ? "connected" : "tap to connect");
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st, lv_color_hex(current ? C_OK : C_MUTED), 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, -64, 0);
    }
    lv_obj_update_layout(s_saved_list);
    lvgl_v9_port_unlock();
}

static void build_settings_overlay(lv_obj_t *scr)
{
    s_set_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_set_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_set_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_set_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_set_overlay, 0, 0);
    lv_obj_set_style_radius(s_set_overlay, 0, 0);
    lv_obj_add_flag(s_set_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_set_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_set_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_set_overlay);
    lv_obj_set_size(card, 940, 560);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings  -  WiFi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, 52, 52);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_INNER), 0);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cx = lv_label_create(close);
    lv_label_set_text(cx, LV_SYMBOL_CLOSE);
    lv_obj_center(cx);

    lv_obj_t *scan = lv_button_create(card);
    lv_obj_set_size(scan, 140, 50);
    lv_obj_align(scan, LV_ALIGN_TOP_LEFT, 4, 48);
    lv_obj_set_style_bg_color(scan, lv_color_hex(C_HUM), 0);
    lv_obj_add_event_cb(scan, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(scan);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH "  SCAN");
    lv_obj_center(sl);

    s_scan_spin = lv_spinner_create(card);
    lv_obj_set_size(s_scan_spin, 40, 40);
    lv_obj_align(s_scan_spin, LV_ALIGN_TOP_LEFT, 154, 53);
    lv_obj_add_flag(s_scan_spin, LV_OBJ_FLAG_HIDDEN);

    s_set_status = lv_label_create(card);
    lv_label_set_text(s_set_status, "not connected");
    lv_obj_set_style_text_font(s_set_status, &lv_font_montserrat_20, 0);
    lv_obj_align(s_set_status, LV_ALIGN_TOP_LEFT, 210, 60);

    /* two columns */
    lv_obj_t *ah = lv_label_create(card);
    lv_label_set_text(ah, "AVAILABLE");
    lv_obj_set_style_text_font(ah, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ah, lv_color_hex(C_MUTED), 0);
    lv_obj_align(ah, LV_ALIGN_TOP_LEFT, 4, 112);

    lv_obj_t *sh = lv_label_create(card);
    lv_label_set_text(sh, "SAVED");
    lv_obj_set_style_text_font(sh, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sh, lv_color_hex(C_MUTED), 0);
    lv_obj_align(sh, LV_ALIGN_TOP_LEFT, 470, 112);

    s_avail_list = lv_obj_create(card);
    lv_obj_set_size(s_avail_list, 448, 260);
    lv_obj_align(s_avail_list, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_obj_set_style_bg_color(s_avail_list, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(s_avail_list, 0, 0);
    lv_obj_set_style_radius(s_avail_list, 8, 0);
    lv_obj_set_style_pad_all(s_avail_list, 8, 0);
    lv_obj_set_style_pad_row(s_avail_list, 6, 0);
    lv_obj_set_flex_flow(s_avail_list, LV_FLEX_FLOW_COLUMN);

    s_saved_list = lv_obj_create(card);
    lv_obj_set_size(s_saved_list, 448, 260);
    lv_obj_align(s_saved_list, LV_ALIGN_TOP_LEFT, 466, 136);
    lv_obj_set_style_bg_color(s_saved_list, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_style_radius(s_saved_list, 8, 0);
    lv_obj_set_style_pad_all(s_saved_list, 8, 0);
    lv_obj_set_style_pad_row(s_saved_list, 6, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);

    /* location row (weather uses this; field nodes have no GPS) */
    s_loc_lbl = lv_label_create(card);
    if (s_have_loc) {
        char b[48];
        snprintf(b, sizeof b, "Location:  %.4f, %.4f", s_lat, s_lng);
        lv_label_set_text(s_loc_lbl, b);
    } else {
        lv_label_set_text(s_loc_lbl, "Location: not set  (needed for the Outside tab)");
    }
    lv_obj_set_style_text_font(s_loc_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_loc_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_loc_lbl, LV_ALIGN_TOP_LEFT, 4, 412);

    lv_obj_t *le = lv_button_create(card);
    lv_obj_set_size(le, 200, 44);
    lv_obj_align(le, LV_ALIGN_TOP_LEFT, 700, 404);
    lv_obj_set_style_bg_color(le, lv_color_hex(C_PRES), 0);
    lv_obj_add_event_cb(le, loc_edit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lel = lv_label_create(le);
    lv_label_set_text(lel, LV_SYMBOL_GPS "  Set location");
    lv_obj_center(lel);

    /* pump strip address row (Tasmota strip; IP isn't known until it's
     * physically set up on the network — see pump_client.h) */
    s_pump_lbl = lv_label_create(card);
    {
        char b[80];
        snprintf(b, sizeof b, "Pump strip:  %s", pump_client_get_host());
        lv_label_set_text(s_pump_lbl, b);
    }
    lv_obj_set_style_text_font(s_pump_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pump_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_pump_lbl, LV_ALIGN_TOP_LEFT, 4, 460);

    lv_obj_t *pe = lv_button_create(card);
    lv_obj_set_size(pe, 200, 44);
    lv_obj_align(pe, LV_ALIGN_TOP_LEFT, 700, 452);
    lv_obj_set_style_bg_color(pe, lv_color_hex(C_PRES), 0);
    lv_obj_add_event_cb(pe, pump_edit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pel = lv_label_create(pe);
    lv_label_set_text(pel, LV_SYMBOL_POWER "  Set pump IP");
    lv_obj_center(pel);

    /* password dialog (hidden) */
    s_pw_panel = lv_obj_create(s_set_overlay);
    lv_obj_set_size(s_pw_panel, 620, 250);
    lv_obj_align(s_pw_panel, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(s_pw_panel, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(s_pw_panel, 2, 0);
    lv_obj_set_style_border_color(s_pw_panel, lv_color_hex(C_HUM), 0);
    lv_obj_set_style_radius(s_pw_panel, 12, 0);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_title = lv_label_create(s_pw_panel);
    lv_label_set_text(s_pw_title, "Password");
    lv_obj_set_style_text_font(s_pw_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pw_title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_pw_title, LV_ALIGN_TOP_LEFT, 16, 14);

    s_pw_ta = lv_textarea_create(s_pw_panel);
    lv_obj_set_size(s_pw_ta, 588, 56);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "network password");
    lv_obj_add_event_cb(s_pw_ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    /* second field, used for longitude in location mode (hidden otherwise) */
    s_pw_ta2 = lv_textarea_create(s_pw_panel);
    lv_obj_set_size(s_pw_ta2, 285, 56);
    lv_obj_align(s_pw_ta2, LV_ALIGN_TOP_RIGHT, -16, 48);
    lv_textarea_set_one_line(s_pw_ta2, true);
    lv_textarea_set_placeholder_text(s_pw_ta2, "longitude");
    lv_obj_add_flag(s_pw_ta2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_pw_ta2, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *conn = lv_button_create(s_pw_panel);
    lv_obj_set_size(conn, 160, 52);
    lv_obj_align(conn, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
    lv_obj_set_style_bg_color(conn, lv_color_hex(C_OK), 0);
    lv_obj_add_event_cb(conn, pw_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(conn);
    lv_label_set_text(cl, "Connect");
    lv_obj_center(cl);

    lv_obj_t *canc = lv_button_create(s_pw_panel);
    lv_obj_set_size(canc, 140, 52);
    lv_obj_align(canc, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    lv_obj_set_style_bg_color(canc, lv_color_hex(C_CARD), 0);
    lv_obj_add_event_cb(canc, pw_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(canc);
    lv_label_set_text(cnl, "Cancel");
    lv_obj_center(cnl);

    /* keyboard (hidden) — overlays the bottom when typing */
    s_kb = lv_keyboard_create(s_set_overlay);
    lv_obj_set_size(s_kb, LV_PCT(100), 260);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, pw_connect_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, pw_cancel_cb, LV_EVENT_CANCEL, NULL);
}

/* ================================================================== */
/* Node trendline overlay                                             */
/* ================================================================== */

static void node_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_node_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void metric_cb(lv_event_t *e)
{
    s_spark_metric = (int)(intptr_t)lv_event_get_user_data(e);
    render_node_chart();                 /* re-plot from cache, no refetch */
}

static void range_cb(lv_event_t *e)
{
    s_spark_hours = (int)(intptr_t)lv_event_get_user_data(e);
    lv_label_set_text(s_node_status, "Loading...");
    s_spark_request = true;              /* refetch at the new range */
}

static void node_card_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_hub_count) return;
    strlcpy(s_detail_eui, s_hubs[i].dev_eui, sizeof s_detail_eui);
    strlcpy(s_detail_name, s_hubs[i].name, sizeof s_detail_name);
    s_spark_metric = 0;
    s_spark_hours = 24;
    s_spark_n = 0;
    lv_label_set_text(s_node_title, s_detail_name);
    lv_label_set_text(s_node_status, "Loading history...");
    lv_obj_remove_flag(s_node_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_node_overlay);
    s_spark_request = true;
}

static lv_obj_t *pill_btn(lv_obj_t *parent, const char *txt, int x, int y, int w,
                          lv_event_cb_t cb, int data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 46);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void build_node_overlay(lv_obj_t *scr)
{
    s_node_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_node_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_node_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_node_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_node_overlay, 0, 0);
    lv_obj_set_style_radius(s_node_overlay, 0, 0);
    lv_obj_add_flag(s_node_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_node_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_node_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_node_overlay);
    lv_obj_set_size(card, 920, 540);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_node_title = lv_label_create(card);
    lv_label_set_text(s_node_title, "Node");
    lv_obj_set_style_text_font(s_node_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_node_title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_node_title, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, 52, 52);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_INNER), 0);
    lv_obj_add_event_cb(close, node_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cx = lv_label_create(close);
    lv_label_set_text(cx, LV_SYMBOL_CLOSE);
    lv_obj_center(cx);

    /* metric selector */
    const char *mlabel[4] = {"Temp", "Humidity", "Soil", "Battery"};
    for (int i = 0; i < 4; i++)
        s_metric_btns[i] = pill_btn(card, mlabel[i], i * 150, 58, 140, metric_cb, i);

    /* range selector */
    s_range_btns[0] = pill_btn(card, "24 h", 640, 58, 90, range_cb, 24);
    s_range_btns[1] = pill_btn(card, "7 d", 738, 58, 90, range_cb, 168);

    /* chart */
    s_node_chart = lv_chart_create(card);
    lv_obj_set_size(s_node_chart, 860, 300);
    lv_obj_align(s_node_chart, LV_ALIGN_TOP_LEFT, 0, 120);
    lv_chart_set_type(s_node_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_node_chart, 5, 8);
    lv_obj_set_style_bg_color(s_node_chart, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(s_node_chart, 0, 0);
    lv_obj_set_style_radius(s_node_chart, 8, 0);
    lv_obj_set_style_line_width(s_node_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(s_node_chart, 0, 0, LV_PART_INDICATOR);   /* hide dots */
    s_node_series = lv_chart_add_series(s_node_chart, lv_color_hex(C_TEMP),
                                        LV_CHART_AXIS_PRIMARY_Y);

    s_node_status = lv_label_create(card);
    lv_label_set_text(s_node_status, "");
    lv_obj_set_style_text_font(s_node_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_node_status, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_node_status, LV_ALIGN_TOP_LEFT, 0, 436);
}

/* ================================================================== */
/* Zone config overlay                                                */
/* ================================================================== */

static char OPT_HOUR[128], OPT_MIN[64], OPT_DUR[96], OPT_SOIL[96];

static void num_opts(char *buf, size_t n, int lo, int hi, int step, bool pad)
{
    buf[0] = '\0';
    for (int v = lo; v <= hi; v += step) {
        char t[8];
        snprintf(t, sizeof t, pad ? "%02d" : "%d", v);
        if (v > lo) strlcat(buf, "\n", n);
        strlcat(buf, t, n);
    }
}

static void roller_set_val(lv_obj_t *r, int lo, int step, int val)
{
    int idx = (val - lo) / step;
    if (idx < 0) idx = 0;
    lv_roller_set_selected(r, idx, LV_ANIM_OFF);
}

static int roller_int(lv_obj_t *r)
{
    char b[8];
    lv_roller_get_selected_str(r, b, sizeof b);
    return atoi(b);
}

static void zc_show_mode(int m)
{
    s_zc_mode = m;
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(s_zc_mbtn[i], lv_color_hex(i == m ? C_ACCENT : C_INNER), 0);
    lv_obj_add_flag(s_zc_sched, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zc_thresh, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zc_manual, LV_OBJ_FLAG_HIDDEN);
    if (m == 0)      lv_obj_remove_flag(s_zc_sched, LV_OBJ_FLAG_HIDDEN);
    else if (m == 1) lv_obj_remove_flag(s_zc_thresh, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_remove_flag(s_zc_manual, LV_OBJ_FLAG_HIDDEN);
}

static void zc_mode_cb(lv_event_t *e) { zc_show_mode((int)(intptr_t)lv_event_get_user_data(e)); }

static void zc_everyday_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    for (int d = 0; d < 7; d++) lv_obj_add_state(s_zc_day_btn[d], LV_STATE_CHECKED);
}

static void zc_name_focus_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_keyboard_set_textarea(s_zc_kb, s_zc_name);
    lv_obj_remove_flag(s_zc_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_zc_kb);
}
static void zc_kb_done_cb(lv_event_t *e) { LV_UNUSED(e); lv_obj_add_flag(s_zc_kb, LV_OBJ_FLAG_HIDDEN); }
static void zc_cancel_cb(lv_event_t *e) { LV_UNUSED(e); lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN); }

static void zc_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int idx = s_edit_zone;
    lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN);
    if (idx < 0 || idx >= MAX_ZONES) return;
    /* Physical relay slot — never reindex. Mark unconfigured locally... */
    s_zones[idx].mode = ZMODE_OFF;
    rebuild_zones();
    /* ...then tell the node: close it now (don't wait for a mid-water
     * deadline to expire) and push the updated config, in that order. */
    s_valve_action_zone = idx;
    s_valve_action = VA_DELETE;
}

static void zc_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int idx = s_edit_zone;
    if (idx < 0 || idx >= MAX_ZONES) { lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN); return; }
    zone_t *z = &s_zones[idx];
    const char *nm = lv_textarea_get_text(s_zc_name);
    strlcpy(z->name, nm[0] ? nm : "Zone", sizeof z->name);
    z->mode = s_zc_mode;
    {
        int sel = lv_dropdown_get_selected(s_zc_relay);
        if (sel < s_zc_valve_count) { z->device = 0; z->relay = sel; }
        else                        { z->device = 1; z->relay = sel - s_zc_valve_count; }
    }
    z->s_hour = roller_int(s_zc_hour);
    z->s_min  = roller_int(s_zc_min);
    z->e_hour = roller_int(s_zc_ehour);
    z->e_min  = roller_int(s_zc_emin);
    z->m_dur  = roller_int(s_zc_mdur);
    z->t_soil = roller_int(s_zc_soil);
    z->t_dur  = roller_int(s_zc_tdur);
    lv_dropdown_get_selected_str(s_zc_node, z->t_node, sizeof z->t_node);
    uint8_t days = 0;
    for (int d = 0; d < 7; d++)
        if (lv_obj_has_state(s_zc_day_btn[d], LV_STATE_CHECKED)) days |= (1 << d);
    z->days = days;
    lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN);
    rebuild_zones();
    s_valve_action = VA_SAVE;      /* push the whole 3-slot config to the node */
}

/** First valve relay index (0..count-1) not already assigned to another
 *  configured valve zone — a sensible default for a freshly-created zone.
 *  Only considers device==0 zones; a pump-outlet zone must be picked
 *  explicitly, there's normally just the one cistern pump to assign. */
static int suggest_relay(int count, int exclude_idx)
{
    for (int r = 0; r < count; r++) {
        bool used = false;
        for (int i = 0; i < MAX_ZONES; i++) {
            if (i == exclude_idx) continue;
            if (s_zones[i].mode != ZMODE_OFF && s_zones[i].device == 0 &&
                s_zones[i].relay == r) { used = true; break; }
        }
        if (!used) return r;
    }
    return 0;
}

static void open_zone_config(int idx)
{
    if (idx < 0 || idx >= MAX_ZONES) return;
    s_edit_zone = idx;
    s_edit_is_new = (s_zones[idx].mode == ZMODE_OFF);

    /* node dropdown from the live field hubs */
    char opts[256] = "";
    for (int i = 0; i < s_hub_count; i++) {
        if (i) strlcat(opts, "\n", sizeof opts);
        strlcat(opts, s_hubs[i].name, sizeof opts);
    }
    if (!opts[0]) strlcpy(opts, "(no nodes)", sizeof opts);
    lv_dropdown_set_options(s_zc_node, opts);

    /* relay dropdown — valve relays THIS connected device actually reports,
     * then the pump strip's outlets (fixed count, separate hardware). A
     * dropdown index < relay_count is a valve relay; >= it is a pump outlet
     * (index - relay_count). s_zc_valve_count remembers the split for
     * zc_save_cb(), which runs after this scope ends. */
    int relay_count = s_device_relays > 0 ? s_device_relays : 1;
    if (relay_count > MAX_RELAYS) relay_count = MAX_RELAYS;
    s_zc_valve_count = relay_count;
    char ropts[128] = "";
    for (int r = 0; r < relay_count; r++) {
        char t[12];
        snprintf(t, sizeof t, "Relay %d", r + 1);
        if (r) strlcat(ropts, "\n", sizeof ropts);
        strlcat(ropts, t, sizeof ropts);
    }
    for (int p = 0; p < PUMP_OUTLETS; p++) {
        char t[16];
        snprintf(t, sizeof t, "Pump Outlet %d", p + 1);
        strlcat(ropts, "\n", sizeof ropts);
        strlcat(ropts, t, sizeof ropts);
    }
    lv_dropdown_set_options(s_zc_relay, ropts);

    zone_t *z = &s_zones[idx];
    if (!s_edit_is_new) {
        lv_textarea_set_text(s_zc_name, z->name);
        zc_show_mode(z->mode);
        int sel;
        if (z->device == 1 && z->relay >= 0 && z->relay < PUMP_OUTLETS)
            sel = relay_count + z->relay;
        else if (z->device == 0 && z->relay >= 0 && z->relay < relay_count)
            sel = z->relay;
        else
            sel = suggest_relay(relay_count, idx);
        lv_dropdown_set_selected(s_zc_relay, sel);
        roller_set_val(s_zc_hour, 0, 1, z->s_hour);
        roller_set_val(s_zc_min, 0, 5, z->s_min);
        roller_set_val(s_zc_ehour, 0, 1, z->e_hour);
        roller_set_val(s_zc_emin, 0, 5, z->e_min);
        roller_set_val(s_zc_mdur, 5, 5, z->m_dur > 0 ? z->m_dur : 10);
        roller_set_val(s_zc_soil, 10, 5, z->t_soil > 0 ? z->t_soil : 30);
        roller_set_val(s_zc_tdur, 5, 5, z->t_dur > 0 ? z->t_dur : 10);
        for (int d = 0; d < 7; d++) {
            if (z->days & (1 << d)) lv_obj_add_state(s_zc_day_btn[d], LV_STATE_CHECKED);
            else lv_obj_remove_state(s_zc_day_btn[d], LV_STATE_CHECKED);
        }
    } else {
        lv_textarea_set_text(s_zc_name, "");
        zc_show_mode(0);
        lv_dropdown_set_selected(s_zc_relay, suggest_relay(relay_count, idx));
        roller_set_val(s_zc_hour, 0, 1, 0);
        roller_set_val(s_zc_min, 0, 5, 0);
        roller_set_val(s_zc_ehour, 0, 1, 0);
        roller_set_val(s_zc_emin, 0, 5, 0);
        roller_set_val(s_zc_mdur, 5, 5, 10);
        roller_set_val(s_zc_soil, 10, 5, 30);
        roller_set_val(s_zc_tdur, 5, 5, 10);
        for (int d = 0; d < 7; d++) lv_obj_add_state(s_zc_day_btn[d], LV_STATE_CHECKED);
    }
    if (s_edit_is_new) lv_obj_add_flag(s_zc_delete, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_remove_flag(s_zc_delete, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_zc_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_zc_overlay);
}

static lv_obj_t *zc_label(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

static lv_obj_t *zc_roller(lv_obj_t *parent, const char *opts, int x, int y, int w)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), LV_PART_SELECTED);
    return r;
}

static void build_zone_config_overlay(lv_obj_t *scr)
{
    num_opts(OPT_HOUR, sizeof OPT_HOUR, 0, 23, 1, true);
    num_opts(OPT_MIN, sizeof OPT_MIN, 0, 55, 5, true);
    num_opts(OPT_DUR, sizeof OPT_DUR, 5, 60, 5, false);
    num_opts(OPT_SOIL, sizeof OPT_SOIL, 10, 90, 5, false);

    s_zc_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_zc_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_zc_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_zc_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_zc_overlay, 0, 0);
    lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zc_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_zc_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_zc_overlay);
    lv_obj_set_size(card, 880, 520);      /* screen is 600 tall; must fit with margin */
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Configure zone");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* name */
    zc_label(card, "NAME", 0, 42);
    s_zc_name = lv_textarea_create(card);
    lv_obj_set_size(s_zc_name, 400, 50);
    lv_obj_align(s_zc_name, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_textarea_set_one_line(s_zc_name, true);
    lv_textarea_set_placeholder_text(s_zc_name, "zone name");
    lv_obj_add_event_cb(s_zc_name, zc_name_focus_cb, LV_EVENT_CLICKED, NULL);

    /* mode buttons */
    const char *ml[3] = {"Schedule", "Threshold", "Manual"};
    for (int i = 0; i < 3; i++) {
        s_zc_mbtn[i] = lv_button_create(card);
        lv_obj_set_size(s_zc_mbtn[i], 130, 48);
        lv_obj_align(s_zc_mbtn[i], LV_ALIGN_TOP_LEFT, 440 + i * 138, 63);
        lv_obj_set_style_bg_color(s_zc_mbtn[i], lv_color_hex(C_INNER), 0);
        lv_obj_add_event_cb(s_zc_mbtn[i], zc_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_zc_mbtn[i]);
        lv_label_set_text(l, ml[i]);
        lv_obj_center(l);
    }

    /* relay — which physical output this zone drives, always visible
     * regardless of mode. Options are populated per-device in
     * open_zone_config(), since different physical boards have different
     * relay counts. */
    zc_label(card, "RELAY", 0, 100);
    s_zc_relay = lv_dropdown_create(card);
    lv_obj_set_width(s_zc_relay, 200);
    lv_obj_align(s_zc_relay, LV_ALIGN_TOP_LEFT, 0, 122);

    /* schedule panel */
    s_zc_sched = lv_obj_create(card);
    lv_obj_set_size(s_zc_sched, 840, 250);
    lv_obj_align(s_zc_sched, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_bg_opa(s_zc_sched, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zc_sched, 0, 0);
    lv_obj_remove_flag(s_zc_sched, LV_OBJ_FLAG_SCROLLABLE);
    zc_label(s_zc_sched, "START HOUR", 0, 0);
    s_zc_hour = zc_roller(s_zc_sched, OPT_HOUR, 0, 22, 120);
    zc_label(s_zc_sched, "START MIN", 150, 0);
    s_zc_min = zc_roller(s_zc_sched, OPT_MIN, 150, 22, 120);
    zc_label(s_zc_sched, "END HOUR", 330, 0);
    s_zc_ehour = zc_roller(s_zc_sched, OPT_HOUR, 330, 22, 120);
    zc_label(s_zc_sched, "END MIN", 480, 0);
    s_zc_emin = zc_roller(s_zc_sched, OPT_MIN, 480, 22, 120);

    zc_label(s_zc_sched, "DAYS", 0, 158);
    for (int d = 0; d < 7; d++) {
        s_zc_day_btn[d] = lv_button_create(s_zc_sched);
        lv_obj_set_size(s_zc_day_btn[d], 66, 46);
        lv_obj_align(s_zc_day_btn[d], LV_ALIGN_TOP_LEFT, d * 72, 180);
        lv_obj_add_flag(s_zc_day_btn[d], LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_color(s_zc_day_btn[d], lv_color_hex(C_INNER), 0);
        lv_obj_set_style_bg_color(s_zc_day_btn[d], lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_t *dl = lv_label_create(s_zc_day_btn[d]);
        lv_label_set_text(dl, DAY_ABBR[d]);
        lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
        lv_obj_center(dl);
    }
    lv_obj_t *everyday = lv_button_create(s_zc_sched);
    lv_obj_set_size(everyday, 130, 46);
    lv_obj_align(everyday, LV_ALIGN_TOP_LEFT, 7 * 72 + 14, 180);
    lv_obj_set_style_bg_color(everyday, lv_color_hex(C_OK), 0);
    lv_obj_add_event_cb(everyday, zc_everyday_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(everyday);
    lv_label_set_text(el, "Everyday");
    lv_obj_center(el);

    /* threshold panel */
    s_zc_thresh = lv_obj_create(card);
    lv_obj_set_size(s_zc_thresh, 840, 250);
    lv_obj_align(s_zc_thresh, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_bg_opa(s_zc_thresh, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zc_thresh, 0, 0);
    lv_obj_remove_flag(s_zc_thresh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_zc_thresh, LV_OBJ_FLAG_HIDDEN);
    zc_label(s_zc_thresh, "FIELD NODE", 0, 0);
    s_zc_node = lv_dropdown_create(s_zc_thresh);
    lv_obj_set_width(s_zc_node, 280);
    lv_obj_align(s_zc_node, LV_ALIGN_TOP_LEFT, 0, 22);
    zc_label(s_zc_thresh, "SOIL BELOW (%)", 300, 0);
    s_zc_soil = zc_roller(s_zc_thresh, OPT_SOIL, 300, 22, 120);
    zc_label(s_zc_thresh, "DURATION (MIN)", 450, 0);
    s_zc_tdur = zc_roller(s_zc_thresh, OPT_DUR, 450, 22, 120);

    /* manual panel — just a duration, used when you tap Irrigate by hand */
    s_zc_manual = lv_obj_create(card);
    lv_obj_set_size(s_zc_manual, 840, 250);
    lv_obj_align(s_zc_manual, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_set_style_bg_opa(s_zc_manual, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zc_manual, 0, 0);
    lv_obj_remove_flag(s_zc_manual, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_zc_manual, LV_OBJ_FLAG_HIDDEN);
    zc_label(s_zc_manual, "DURATION (MIN)", 0, 0);
    s_zc_mdur = zc_roller(s_zc_manual, OPT_DUR, 0, 22, 120);

    /* save / cancel */
    lv_obj_t *save = lv_button_create(card);
    lv_obj_set_size(save, 160, 54);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(C_OK), 0);
    lv_obj_add_event_cb(save, zc_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(save);
    lv_label_set_text(svl, "Save");
    lv_obj_center(svl);

    lv_obj_t *canc = lv_button_create(card);
    lv_obj_set_size(canc, 140, 54);
    lv_obj_align(canc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(canc, lv_color_hex(C_INNER), 0);
    lv_obj_add_event_cb(canc, zc_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(canc);
    lv_label_set_text(cnl, "Cancel");
    lv_obj_center(cnl);

    /* delete — only shown when editing an existing zone */
    s_zc_delete = lv_button_create(card);
    lv_obj_set_size(s_zc_delete, 160, 54);
    lv_obj_align(s_zc_delete, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_zc_delete, lv_color_hex(C_BAD), 0);
    lv_obj_add_event_cb(s_zc_delete, zc_delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(s_zc_delete);
    lv_label_set_text(dl, LV_SYMBOL_TRASH " Delete");
    lv_obj_center(dl);

    /* keyboard for the name field */
    s_zc_kb = lv_keyboard_create(s_zc_overlay);
    lv_obj_set_size(s_zc_kb, LV_PCT(100), 240);
    lv_obj_align(s_zc_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_zc_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(s_zc_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_zc_kb, zc_kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_zc_kb, zc_kb_done_cb, LV_EVENT_CANCEL, NULL);
}

void mc_dash_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    bg_grad(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Header — subtle gradient + a thin accent line at the bottom */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 56);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x18263E), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_color_hex(0x0E1A2C), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hdr, 2, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_opa(hdr, LV_OPA_40, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "MicroClimate");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 18, 0);

    s_clock = lv_label_create(hdr);
    lv_label_set_text(s_clock, "--:--:--");
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, 0);

    s_wifi_lbl = lv_label_create(hdr);
    lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI "  ...");
    lv_obj_set_style_text_font(s_wifi_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(C_WARN), 0);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_RIGHT_MID, -74, 0);

    /* Gear -> settings overlay */
    lv_obj_t *gear = lv_button_create(hdr);
    lv_obj_set_size(gear, 48, 44);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(gear, 8, 0);
    lv_obj_add_event_cb(gear, gear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gi = lv_label_create(gear);
    lv_label_set_text(gi, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gi, &lv_font_montserrat_20, 0);
    lv_obj_center(gi);

    /* Tabview */
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_size(tv, 56);
    lv_obj_set_size(tv, LV_PCT(100), LV_VER_RES - 56);
    lv_obj_align(tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    bg_grad(tv);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101B2C), 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(C_TEXT), LV_STATE_CHECKED);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_20, 0);

    build_indoor(lv_tabview_add_tab(tv, LV_SYMBOL_HOME "  Indoor"));
    build_outside(lv_tabview_add_tab(tv, LV_SYMBOL_UP "  Weather API"));
    build_hubs(lv_tabview_add_tab(tv, LV_SYMBOL_GPS "  Field Hubs"));
    build_irrigation(lv_tabview_add_tab(tv, LV_SYMBOL_TINT "  Irrigation"));
    pump_client_load_host();
    pump_tab_build(lv_tabview_add_tab(tv, LV_SYMBOL_POWER "  Pump"));

    /* Load saved networks; seed the first run from secrets.h so the board
     * still auto-connects before any network has been added via the UI. */
    s_store.load();
    if (s_store.count() == 0) s_store.add(WIFI_SSID, WIFI_PASS);
    load_location();

    build_settings_overlay(scr);
    build_node_overlay(scr);
    build_zone_config_overlay(scr);

    s_bme_ok = s_bme.begin(0);
    if (s_bme_ok) lv_label_set_text(s_in_stat, "BME280 LIVE  -  0x76");

    lv_timer_create(tick, 1000, NULL);
}
