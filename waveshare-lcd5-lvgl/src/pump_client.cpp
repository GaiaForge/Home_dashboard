/**
 * Nous A5T (Tasmota) power-strip client. See pump_client.h.
 *
 * Uses Tasmota's real HTTP command API (GET /cm?cmnd=...), not a REST
 * convention of our own — this is a stock device, so we speak its language:
 *   GET /cm?cmnd=STATE        -> {"POWER1":"ON","POWER2":"OFF",...}
 *   GET /cm?cmnd=Power1%20ON  -> switches relay 1, returns {"POWER1":"ON"}
 *   GET /cm?cmnd=Status%208   -> {"StatusSNS":{"ENERGY":{"Power":.., "Voltage":.., "Current":..}}}
 */
#include "pump_client.h"
#include "lvgl_v9_port.h"
#include "secrets.h"

#include <lvgl.h>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define C_BG      0x081018
#define C_CARD    0x1B2942
#define C_CARD2   0x121C30
#define C_INNER   0x1C2E42
#define C_TEXT    0xEAF2FB
#define C_MUTED   0x7C93AC
#define C_OK      0x22C55E
#define C_BAD     0xEF4444
#define C_WARN    0xF59E0B
#define C_ACCENT  0x3B82F6

/* MEASURED 2026-07-26: was 8000ms, but with PUMP_HOST still an unset
 * placeholder, every poll fails and burns ~6s in blocked connect-timeout
 * WiFi activity — near-constant WiFi churn. Relaxed to 30s; a simple on/off
 * power strip doesn't need faster polling once the real device is set. */
#define POLL_MS   30000

static const char *OUTLET_NAME[3] = { "Outlet 1", "Outlet 2", "Outlet 3" };

/* Runtime address — set from NVS (see pump_client_load_host()), falling
 * back to secrets.h's PUMP_HOST until the user sets a real one from the
 * panel's Settings overlay. Was a compile-time macro; now editable on
 * device since the strip's IP isn't known until it's physically set up. */
static char s_host[64] = PUMP_HOST;

static bool  s_relay[3];
static bool  s_online;
static float s_power_w, s_volt_v, s_curr_a;

static lv_obj_t *s_status_lbl;
static lv_obj_t *s_relay_sw[3];
static lv_obj_t *s_power_val, *s_volt_val, *s_curr_val;

typedef enum { PA_NONE, PA_TOGGLE } pump_action_t;
static volatile pump_action_t s_action = PA_NONE;
static int  s_action_relay;
static bool s_action_on;

static void render_pump_status(void);

/* GET a Tasmota command; cmnd may contain a literal space (percent-encoded
 * here) e.g. "Power1 ON" or "Status 8". Returns true on HTTP 200 + valid JSON.
 * Per Tasmota's documented HTTP API, credentials (if PUMP_PASS is set) go in
 * the URL as plain query params — that's Tasmota's own scheme, not ours. */
static bool tasmota_cmd(const char *cmnd, JsonDocument &doc)
{
    String enc = cmnd;
    enc.replace(" ", "%20");
    String url = String("http://") + s_host + "/cm?";
    if (PUMP_PASS[0]) {
        url += "user=" + String(PUMP_USER[0] ? PUMP_USER : "admin") +
               "&password=" + String(PUMP_PASS) + "&";
    }
    url += "cmnd=" + enc;

    HTTPClient http;
    http.begin(url);
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    return deserializeJson(doc, body) == DeserializationError::Ok;
}

static void fetch_pump_status(void)
{
    JsonDocument state;
    bool ok1 = tasmota_cmd("STATE", state);
    if (ok1) {
        s_relay[0] = strcmp(state["POWER1"] | "OFF", "ON") == 0;
        s_relay[1] = strcmp(state["POWER2"] | "OFF", "ON") == 0;
        s_relay[2] = strcmp(state["POWER3"] | "OFF", "ON") == 0;
    }

    JsonDocument sns;
    bool ok2 = tasmota_cmd("Status 8", sns);
    if (ok2) {
        JsonObject e = sns["StatusSNS"]["ENERGY"];
        s_power_w = e["Power"] | 0.0f;
        s_volt_v  = e["Voltage"] | 0.0f;
        s_curr_a  = e["Current"] | 0.0f;
    }

    s_online = ok1 && ok2;
    if (!s_online) Serial.println("pump: unreachable");
    render_pump_status();
}

static void pump_set(int relay, bool on)
{
    char cmnd[16];
    snprintf(cmnd, sizeof cmnd, "Power%d %s", relay + 1, on ? "ON" : "OFF");
    JsonDocument doc;
    bool ok = tasmota_cmd(cmnd, doc);
    Serial.printf("pump: %s -> %s\n", cmnd, ok ? "ok" : "failed");
}

static void relay_switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_action_relay = i;
    s_action_on = on;
    s_action = PA_TOGGLE;
}

static void render_pump_status(void)
{
    lvgl_v9_port_lock(-1);
    char status[96];
    if (s_online) {
        snprintf(status, sizeof status, LV_SYMBOL_OK "  %s  -  connected", s_host);
        lv_label_set_text(s_status_lbl, status);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(C_OK), 0);
    } else {
        snprintf(status, sizeof status,
                 LV_SYMBOL_WARNING "  %s  -  offline (check strip power/WiFi)", s_host);
        lv_label_set_text(s_status_lbl, status);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(C_BAD), 0);
    }
    for (int i = 0; i < 3; i++) {
        if (s_relay[i]) lv_obj_add_state(s_relay_sw[i], LV_STATE_CHECKED);
        else            lv_obj_remove_state(s_relay_sw[i], LV_STATE_CHECKED);
    }
    char b[16];
    snprintf(b, sizeof b, "%.0f", s_power_w); lv_label_set_text(s_power_val, b);
    snprintf(b, sizeof b, "%.0f", s_volt_v);  lv_label_set_text(s_volt_val, b);
    snprintf(b, sizeof b, "%.2f", s_curr_a);  lv_label_set_text(s_curr_val, b);
    lvgl_v9_port_unlock();
}

/* ------------------------------------------------------------------ */
/* UI                                                                   */
/* ------------------------------------------------------------------ */

static lv_obj_t *glass_card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x93A9CC), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_shadow_width(c, 16, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_y(c, 5, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *stat_tile(lv_obj_t *parent, int x, const char *cap, const char *unit)
{
    lv_obj_t *c = glass_card(parent, 220, 130);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, x, 0);

    lv_obj_t *cl = lv_label_create(c);
    lv_label_set_text(cl, cap);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(C_MUTED), 0);
    lv_obj_align(cl, LV_ALIGN_TOP_LEFT, 14, 10);

    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 14, -10);

    lv_obj_t *u = lv_label_create(c);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), 0);
    lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, -14, -14);

    return v;      /* the caller stores this to update the value later */
}

void pump_tab_build(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_color(tab, lv_color_hex(0x05090F), 0);
    lv_obj_set_style_bg_grad_dir(tab, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(tab);
    char connecting[80];
    snprintf(connecting, sizeof connecting, LV_SYMBOL_WARNING "  Connecting to %s...", s_host);
    lv_label_set_text(s_status_lbl, connecting);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(C_WARN), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_LEFT, 4, 2);

    /* One card per relay: name + a big switch. Mains-rated device — this
     * strip is for pumps/appliances, never a low-voltage irrigation valve. */
    lv_obj_t *rowcard = glass_card(tab, LV_PCT(100), 100);
    lv_obj_align(rowcard, LV_ALIGN_TOP_LEFT, 0, 36);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *nm = lv_label_create(rowcard);
        lv_label_set_text(nm, OUTLET_NAME[i]);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 30 + i * 330, -16);

        s_relay_sw[i] = lv_switch_create(rowcard);
        lv_obj_set_size(s_relay_sw[i], 76, 40);
        lv_obj_align(s_relay_sw[i], LV_ALIGN_LEFT_MID, 30 + i * 330, 22);
        lv_obj_set_style_bg_color(s_relay_sw[i], lv_color_hex(C_OK),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_relay_sw[i], relay_switch_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
    }

    /* Power monitoring — TOTAL across the whole strip (single sensor), not
     * per-outlet. Only a clean proxy for one pump if it's the only load. */
    lv_obj_t *cap = lv_label_create(tab);
    lv_label_set_text(cap, "POWER  (total strip draw, not per-outlet)");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(C_MUTED), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 4, 160);

    s_power_val = stat_tile(tab, 0,   "POWER", "W");
    s_volt_val  = stat_tile(tab, 236, "VOLTAGE", "V");
    s_curr_val  = stat_tile(tab, 472, "CURRENT", "A");
    lv_obj_align(lv_obj_get_parent(s_power_val), LV_ALIGN_TOP_LEFT, 0, 190);
    lv_obj_align(lv_obj_get_parent(s_volt_val),  LV_ALIGN_TOP_LEFT, 236, 190);
    lv_obj_align(lv_obj_get_parent(s_curr_val),  LV_ALIGN_TOP_LEFT, 472, 190);
}

void pump_client_loop(void)
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (s_action != PA_NONE) {
        pump_action_t act = s_action;
        int relay = s_action_relay; bool on = s_action_on;
        s_action = PA_NONE;
        if (act == PA_TOGGLE) pump_set(relay, on);
        fetch_pump_status();
        return;
    }

    static uint32_t last_poll;
    if (millis() - last_poll >= POLL_MS) {
        last_poll = millis();
        fetch_pump_status();
    }
}

void pump_client_load_host(void)
{
    Preferences p;
    p.begin("mcpump", true);
    String h = p.getString("host", PUMP_HOST);
    p.end();
    strlcpy(s_host, h.c_str(), sizeof s_host);
}

void pump_client_set_host(const char *host)
{
    strlcpy(s_host, host, sizeof s_host);
    Preferences p;
    p.begin("mcpump", false);
    p.putString("host", host);
    p.end();
    s_online = false;         /* unknown until the next poll proves it */
}

const char *pump_client_get_host(void)
{
    return s_host;
}
