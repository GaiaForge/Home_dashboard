/**
 * GaiaForge garden valve controller — standalone ESP32 + relay board.
 *
 * Autonomous irrigation node. It waters on its own (schedule + soil threshold),
 * driven entirely by config held locally in flash. The MicroClimate wall panel
 * talks to it directly over WiFi (HTTP) for config, status and manual override,
 * but the node NEVER needs the panel or the server to water — if the network
 * is down it simply fails safe (all valves closed).
 *
 *   Panel  --HTTP-->  this node          (config / status / water / stop)
 *   this node --GET--> MicroClimate srv   (read-only, field-node soil %)
 *
 * A "zone" is a named watering PROFILE (schedule/threshold/manual config),
 * separate from a "relay" (a physical GPIO/output). Each zone is explicitly
 * assigned to one relay (Zone.relay, -1 = unassigned = drives nothing) by
 * whoever configures it — there is no assumed 1:1 slot-to-relay mapping.
 * MAX_ZONE_SLOTS (3) is fixed to match the panel; NUM_RELAYS is however many
 * physical outputs THIS board actually has, from RELAY_PINS[].
 *
 * A zone's output can also be a Tasmota outlet on the Nous A5T strip
 * (Zone.device=1, PUMP_HOST) instead of a local relay — e.g. a cistern pump
 * on a permanently-open manual valve, scheduled the same way as a real zone.
 *
 * ================== SAFETY — READ BEFORE CONNECTING WATER ==================
 *  - Relays are driven so the DE-ENERGIZED state = valve CLOSED. Wire your
 *    solenoids to the relay's Normally-Open (NO) contact, and set
 *    RELAY_ACTIVE_LOW to match your board so that on boot/fault/reset every
 *    valve is shut. VERIFY with a multimeter (no water connected) that a fresh
 *    boot leaves all relays de-energized before you plumb it in.
 *  - Every zone has a hard max-run-time (GLOBAL_MAX_RUN_MIN). A valve cannot
 *    stay open longer than that no matter what the config, panel, or network
 *    do. This is the backstop against a flooded garden. This cap applies to
 *    device=1 (pump) zones too.
 *  - Loss of WiFi/time/soil data means triggers simply don't fire (no water).
 *  - device=1 zones are the ONE exception to "fails safe on reboot": a
 *    Tasmota outlet holds its last commanded state across OUR reboot, unlike
 *    a GPIO relay. setup() best-effort forces every pump zone's outlet off
 *    once WiFi is up, but if the strip itself is unreachable at that moment
 *    the pump could stay running until it is. Don't treat a pump zone as
 *    having the same reboot guarantee as a real valve.
 * ==========================================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "secrets.h"

/* ------------------------------------------------------------------ *
 * HARDWARE CONFIG — set these to match your board, then verify safety.
 * ------------------------------------------------------------------ */
/* ---- BENCH TEST CONFIG: Travis's 2-relay test unit, pins as wired ----
 * MEASURED 2026-07-26: relay 1 appeared stuck ON regardless of commanded
 * state or which GPIO drove it (tried 5, then 16) — root cause was the
 * relay module's own trigger-polarity jumper set to active-LOW while this
 * firmware drives active-HIGH (RELAY_ACTIVE_LOW below). Not a bad GPIO —
 * GPIO 5 was never actually at fault, the jumper fought whatever pin fed
 * it. Fixed by flipping the jumper to active-HIGH. Left wired on 16/17
 * (no need to move back to 5) — both relays confirmed toggling cleanly. */
static const int RELAY_PINS[] = { 16, 17 };   /* one GPIO per physical relay */
#define NUM_RELAYS  (int)(sizeof(RELAY_PINS) / sizeof(RELAY_PINS[0]))
#define MAX_ZONE_SLOTS       3        /* fixed — matches the panel's MAX_ZONES */

/* MEASURED 2026-07-26: on boot with nothing configured, relays were
 * observed ON — the opposite of this board's polarity. Flipped from 1 to 0.
 * If relays click on at boot again, flip back. */
#define RELAY_ACTIVE_LOW     0        /* 1: LOW energizes relay, 0: HIGH energizes */
#define GLOBAL_MAX_RUN_MIN   30       /* hard per-zone cap, minutes (safety) */
#define THRESH_COOLDOWN_MIN  60       /* min gap between threshold waterings */
#define SOIL_REFRESH_MS      60000    /* how often to poll the server for soil */
#define WDT_TIMEOUT_S        20

/* ------------------------------------------------------------------ */
enum { MODE_SCHEDULE = 0, MODE_THRESHOLD = 1, MODE_MANUAL = 2, MODE_OFF = 3 };

struct Zone {
    char     name[24];
    int      mode;
    int      device;                        /* 0 = local relay, 1 = Tasmota pump outlet */
    int      relay;                         /* index within `device`'s own list; -1 = unassigned */
    int      s_hour, s_min;                 /* schedule: start time */
    int      e_hour, e_min;                 /* schedule: end time */
    uint8_t  days;                          /* schedule: bitmask, bit N = tm_wday N (0=Sun) */
    int      m_dur;                         /* manual: duration (min) */
    char     t_node[24];                    /* threshold: field-node name */
    int      t_soil, t_dur;                 /* threshold: soil% and duration */
    /* runtime (not persisted — lost on reboot, same as close_at/open below) */
    uint32_t close_at;                      /* millis deadline; 0 = closed */
    bool     open;
    int      last_run_yday;                 /* schedule: last day it fired */
    uint32_t last_thresh_ms;                /* threshold cooldown tracking */
    char     last_water[20];                /* "YYYY-MM-DD HH:MM" of last watering start, any mode */
    int      last_water_min;                /* actual (capped) duration of that run */
    /* device==1 only: last state actually confirmed applied to the strip,
     * and when we last tried — a local relay is a GPIO write (always
     * "succeeds"), but a Tasmota outlet is a network call that can fail, so
     * it needs its own retry/backoff instead of being hammered every tick. */
    bool     dev_applied;
    uint32_t dev_last_attempt;
};
static Zone zones[MAX_ZONE_SLOTS];

static WebServer server(80);
static Preferences prefs;
static uint32_t s_last_soil;
static bool     s_have_time;

/* ------------------------------------------------------------------ */
/* Relay control — fail-safe closed                                    */
/* ------------------------------------------------------------------ */

/** relay_idx is a PHYSICAL relay index (0..NUM_RELAYS-1). -1 (unassigned)
 *  or out-of-range safely does nothing — an unassigned zone can never
 *  energize a relay, even if its schedule/threshold logic fires. */
static void relay_set(int relay_idx, bool open)
{
    if (relay_idx < 0 || relay_idx >= NUM_RELAYS) return;
    int energized = open;
    int level = RELAY_ACTIVE_LOW ? (energized ? LOW : HIGH)
                                 : (energized ? HIGH : LOW);
    digitalWrite(RELAY_PINS[relay_idx], level);
}

static void all_closed(void)
{
    for (int i = 0; i < NUM_RELAYS; i++) relay_set(i, false);
    for (int i = 0; i < MAX_ZONE_SLOTS; i++) {
        zones[i].close_at = 0;
        zones[i].open = false;
    }
}

/** Switch outlet `outlet` (0-based) on the Nous A5T at PUMP_HOST. Unlike
 *  relay_set(), this is a network call that can fail or take a moment —
 *  callers must not invoke it every control_tick() the way relay_set() is;
 *  see the dev_applied/dev_last_attempt retry logic in control_tick(). */
static bool tasmota_set(int outlet, bool on)
{
    HTTPClient http;
    char url[96];
    snprintf(url, sizeof url, "http://%s/cm?cmnd=Power%d%%20%s",
             PUMP_HOST, outlet + 1, on ? "ON" : "OFF");
    http.begin(url);
    http.setConnectTimeout(2000);
    http.setTimeout(2000);
    int code = http.GET();
    http.end();
    return code == 200;
}

/* ------------------------------------------------------------------ */
/* Config persistence (NVS, as JSON)                                   */
/* ------------------------------------------------------------------ */

static void config_defaults(void)
{
    for (int i = 0; i < MAX_ZONE_SLOTS; i++) {
        Zone &z = zones[i];
        snprintf(z.name, sizeof z.name, "Zone %d", i + 1);
        z.mode = MODE_OFF;
        z.device = 0;                       /* local relay by default */
        z.relay = -1;                       /* unassigned until explicitly chosen */
        z.s_hour = 0; z.s_min = 0; z.e_hour = 0; z.e_min = 0;
        z.days = 0x7F;                       /* everyday, once Schedule is enabled */
        z.m_dur = 10;
        z.t_node[0] = '\0'; z.t_soil = 30; z.t_dur = 10;
        z.close_at = 0; z.open = false; z.last_run_yday = -1; z.last_thresh_ms = 0;
        z.last_water[0] = '\0'; z.last_water_min = 0;
        z.dev_applied = false; z.dev_last_attempt = 0;
    }
}

static void config_to_json(JsonArray arr)
{
    for (int i = 0; i < MAX_ZONE_SLOTS; i++) {
        Zone &z = zones[i];
        JsonObject o = arr.add<JsonObject>();
        o["name"] = z.name;
        o["mode"] = z.mode;
        o["device"] = z.device;
        o["relay"] = z.relay;
        o["s_hour"] = z.s_hour; o["s_min"] = z.s_min;
        o["e_hour"] = z.e_hour; o["e_min"] = z.e_min;
        o["days"] = z.days;
        o["m_dur"] = z.m_dur;
        o["t_node"] = z.t_node; o["t_soil"] = z.t_soil; o["t_dur"] = z.t_dur;
    }
}

static void config_from_json(JsonArray arr)
{
    int i = 0;
    for (JsonObject o : arr) {
        if (i >= MAX_ZONE_SLOTS) break;
        Zone &z = zones[i];
        strlcpy(z.name, o["name"] | z.name, sizeof z.name);
        z.mode = o["mode"] | z.mode;
        z.device = o["device"] | z.device;
        z.relay = o["relay"] | z.relay;
        z.s_hour = o["s_hour"] | z.s_hour;
        z.s_min  = o["s_min"]  | z.s_min;
        z.e_hour = o["e_hour"] | z.e_hour;
        z.e_min  = o["e_min"]  | z.e_min;
        z.days   = o["days"]   | z.days;
        z.m_dur  = o["m_dur"]  | z.m_dur;
        strlcpy(z.t_node, o["t_node"] | z.t_node, sizeof z.t_node);
        z.t_soil = o["t_soil"] | z.t_soil;
        z.t_dur  = o["t_dur"]  | z.t_dur;
        i++;
    }
}

static void config_save(void)
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    config_to_json(arr);
    String s;
    serializeJson(doc, s);
    prefs.begin("valves", false);
    prefs.putString("cfg", s);
    prefs.end();
}

static void config_load(void)
{
    config_defaults();
    prefs.begin("valves", true);
    String s = prefs.getString("cfg", "");
    prefs.end();
    if (s.length()) {
        JsonDocument doc;
        if (!deserializeJson(doc, s)) config_from_json(doc.as<JsonArray>());
    }
}

/* ------------------------------------------------------------------ */
/* Soil from the MicroClimate server (read-only)                       */
/* ------------------------------------------------------------------ */

/* Cache of the latest field-node soil %, keyed by node name. */
#define SOIL_CACHE 16
static char  s_soil_name[SOIL_CACHE][24];
static float s_soil_val[SOIL_CACHE];
static int   s_soil_n;

static float soil_for(const char *node)
{
    for (int i = 0; i < s_soil_n; i++)
        if (strcmp(s_soil_name[i], node) == 0) return s_soil_val[i];
    return -1;
}

static void refresh_soil(void)
{
    HTTPClient http;
    http.begin(String(MC_SERVER) + "/api/nodes");
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    if (http.GET() != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    s_soil_n = 0;
    for (JsonObject node : doc.as<JsonArray>()) {
        if (s_soil_n >= SOIL_CACHE) break;
        strlcpy(s_soil_name[s_soil_n], node["name"] | "", 24);
        s_soil_val[s_soil_n] = node["sensors"]["soil_moisture"] | -1.0;
        s_soil_n++;
    }
}

/* ------------------------------------------------------------------ */
/* Control loop — evaluate zones, drive relays                         */
/* ------------------------------------------------------------------ */

static uint32_t cap_ms(int minutes)
{
    if (minutes > GLOBAL_MAX_RUN_MIN) minutes = GLOBAL_MAX_RUN_MIN;
    if (minutes < 1) minutes = 1;
    return (uint32_t)minutes * 60000UL;
}

/** Begin a watering run: applies the safety cap, sets the deadline, and
 *  records it for "last watered" — used identically whether the trigger was
 *  a schedule window, a soil threshold, or a manual /water command. */
static void start_watering(Zone &z, int minutes)
{
    uint32_t ms = cap_ms(minutes);
    z.close_at = millis() + ms;
    z.last_water_min = (int)(ms / 60000UL);   /* the ACTUAL capped duration */

    struct tm tm;
    if (getLocalTime(&tm, 0)) {
        strftime(z.last_water, sizeof z.last_water, "%Y-%m-%d %H:%M", &tm);
    } else {
        strlcpy(z.last_water, "unknown time", sizeof z.last_water);
    }
}

static void control_tick(void)
{
    struct tm tm;
    s_have_time = getLocalTime(&tm, 0);
    uint32_t now = millis();

    for (int i = 0; i < MAX_ZONE_SLOTS; i++) {
        Zone &z = zones[i];

        /* schedule: fire once per day at the start minute, on enabled days.
         * Run length is however long the [start,end) window is — still
         * hard-capped by start_watering()/cap_ms() no matter how big the
         * configured window is. end <= start (misconfigured) never fires. */
        if (z.mode == MODE_SCHEDULE && s_have_time) {
            if (tm.tm_hour == z.s_hour && tm.tm_min == z.s_min &&
                z.last_run_yday != tm.tm_yday &&
                (z.days & (1 << tm.tm_wday))) {
                z.last_run_yday = tm.tm_yday;
                int dur_min = (z.e_hour * 60 + z.e_min) - (z.s_hour * 60 + z.s_min);
                if (dur_min > 0) start_watering(z, dur_min);
            }
        }

        /* threshold: soil below target, respecting a cooldown */
        if (z.mode == MODE_THRESHOLD && z.close_at == 0) {
            float soil = soil_for(z.t_node);
            bool cooled = (z.last_thresh_ms == 0) ||
                          (now - z.last_thresh_ms > (uint32_t)THRESH_COOLDOWN_MIN * 60000UL);
            if (soil >= 0 && soil < z.t_soil && cooled) {
                z.last_thresh_ms = now;
                start_watering(z, z.t_dur);
            }
        }

        /* apply: open while a deadline is in the future */
        bool open = false;
        if (z.close_at != 0) {
            if ((int32_t)(z.close_at - now) > 0) open = true;
            else z.close_at = 0;             /* deadline passed -> close */
        }
        z.open = open;

        if (z.device == 0) {
            relay_set(z.relay, open);        /* local GPIO — cheap, fine every tick */
        } else if (z.relay >= 0) {
            /* Tasmota outlet — only call out on an actual state change, and
             * retry on a backoff (not every tick) until it's confirmed, so a
             * long watering run against an unreachable strip doesn't turn
             * into a synchronous HTTP call every second. */
            if (open != z.dev_applied && now - z.dev_last_attempt > 3000) {
                z.dev_last_attempt = now;
                if (tasmota_set(z.relay, open)) z.dev_applied = open;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* HTTP API                                                            */
/* ------------------------------------------------------------------ */

static void send_json(JsonDocument &doc)
{
    String s;
    serializeJson(doc, s);
    server.send(200, "application/json", s);
}

/** Next time this Schedule zone will fire, as a short human string, or ""
 *  if it's not a Schedule zone, has no days enabled, or time isn't synced. */
static void compute_next_run(Zone &z, char *out, size_t outsz)
{
    out[0] = '\0';
    if (z.mode != MODE_SCHEDULE || !s_have_time) return;

    struct tm tm;
    if (!getLocalTime(&tm, 0)) return;
    int today_min = tm.tm_hour * 60 + tm.tm_min;
    int target_min = z.s_hour * 60 + z.s_min;
    static const char *dayname[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    /* off=0..7: today, the next 6 days, then today again (next week) — the
     * last case covers "only today is enabled and today's slot already passed". */
    for (int off = 0; off <= 7; off++) {
        int wday = (tm.tm_wday + off) % 7;
        if (!(z.days & (1 << wday))) continue;
        if (off == 0 && target_min <= today_min) continue;
        if (off == 0)      snprintf(out, outsz, "Today %02d:%02d", z.s_hour, z.s_min);
        else if (off == 1) snprintf(out, outsz, "Tomorrow %02d:%02d", z.s_hour, z.s_min);
        else               snprintf(out, outsz, "%s %02d:%02d", dayname[wday], z.s_hour, z.s_min);
        return;
    }
    strlcpy(out, "no days selected", outsz);
}

static void handle_status(void)
{
    JsonDocument doc;
    doc["time_ok"] = s_have_time;
    doc["num_relays"] = NUM_RELAYS;      /* panel: only offer relays that exist */
    JsonArray arr = doc["zones"].to<JsonArray>();
    uint32_t now = millis();
    for (int i = 0; i < MAX_ZONE_SLOTS; i++) {
        Zone &z = zones[i];
        JsonObject o = arr.add<JsonObject>();
        o["name"] = z.name;
        o["mode"] = z.mode;
        o["device"] = z.device;
        o["relay"] = z.relay;
        o["open"] = z.open;
        int secs = (z.close_at && (int32_t)(z.close_at - now) > 0)
                   ? (int)((z.close_at - now) / 1000) : 0;
        o["secs_left"] = secs;
        o["s_hour"] = z.s_hour; o["s_min"] = z.s_min;
        o["e_hour"] = z.e_hour; o["e_min"] = z.e_min;
        o["days"] = z.days;
        o["m_dur"] = z.m_dur;
        o["t_node"] = z.t_node; o["t_soil"] = z.t_soil; o["t_dur"] = z.t_dur;
        o["last_water"] = z.last_water;
        o["last_water_min"] = z.last_water_min;
        char next[24];
        compute_next_run(z, next, sizeof next);
        o["next_run"] = next;
    }
    send_json(doc);
}

static void handle_get_config(void)
{
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    config_to_json(arr);
    send_json(doc);
}

static void handle_post_config(void)
{
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "bad json"); return;
    }
    config_from_json(doc.as<JsonArray>());
    config_save();
    server.send(200, "text/plain", "ok");
}

/** Raw relay test — bypasses ALL zone/schedule/threshold logic, directly
 *  drives one physical pin. For wiring sanity checks only; not used by the
 *  panel. GET /relaytest?relay=0&on=1 */
static void handle_relay_test(void)
{
    int r = server.arg("relay").toInt();
    int on = server.arg("on").toInt();
    if (r < 0 || r >= NUM_RELAYS) { server.send(400, "text/plain", "bad relay"); return; }
    relay_set(r, on != 0);
    char buf[80];
    snprintf(buf, sizeof buf, "relay %d (GPIO %d) -> %s", r, RELAY_PINS[r], on ? "ON" : "OFF");
    Serial.println(buf);
    server.send(200, "text/plain", buf);
}

static void handle_water(void)
{
    int zi = server.arg("zone").toInt();
    int mins = server.hasArg("minutes") ? server.arg("minutes").toInt() : 10;
    if (zi < 0 || zi >= MAX_ZONE_SLOTS) { server.send(400, "text/plain", "bad zone"); return; }
    start_watering(zones[zi], mins);
    server.send(200, "text/plain", "ok");
}

static void handle_stop(void)
{
    int zi = server.arg("zone").toInt();
    if (zi < 0 || zi >= MAX_ZONE_SLOTS) { server.send(400, "text/plain", "bad zone"); return; }
    Zone &z = zones[zi];
    z.close_at = 0;
    z.open = false;
    if (z.device == 0) {
        relay_set(z.relay, false);           /* close it now, don't wait for the next tick */
    } else if (z.relay >= 0) {
        z.dev_last_attempt = millis();
        if (tasmota_set(z.relay, false)) z.dev_applied = false;
        /* on failure, control_tick()'s retry loop will keep trying */
    }
    server.send(200, "text/plain", "ok");
}

/* ------------------------------------------------------------------ */

void setup()
{
    Serial.begin(115200);

    /* Relays FIRST — everything closed before anything else runs. */
    for (int i = 0; i < NUM_RELAYS; i++) pinMode(RELAY_PINS[i], OUTPUT);
    all_closed();

    config_load();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

        /* A local relay fails safe on reboot for free (GPIO resets LOW/de-
         * energized). A remote Tasmota outlet does NOT — it keeps whatever
         * it was last told, even if we rebooted mid-run. Best-effort only:
         * if the strip isn't reachable yet this does nothing, but
         * control_tick()'s dev_applied/dev_last_attempt retry will keep
         * trying once a pump zone next evaluates. */
        for (int i = 0; i < MAX_ZONE_SLOTS; i++)
            if (zones[i].device == 1 && zones[i].relay >= 0)
                tasmota_set(zones[i].relay, false);
    }

    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org");

    if (MDNS.begin(VALVE_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", VALVE_HOSTNAME);
    }

    server.on("/status", HTTP_GET, handle_status);
    server.on("/config", HTTP_GET, handle_get_config);
    server.on("/config", HTTP_POST, handle_post_config);
    server.on("/water",  HTTP_POST, handle_water);
    server.on("/stop",   HTTP_POST, handle_stop);
    server.on("/relaytest", HTTP_GET, handle_relay_test);
    server.begin();

    esp_task_wdt_config_t wdt = { .timeout_ms = WDT_TIMEOUT_S * 1000,
                                  .idle_core_mask = 0, .trigger_panic = true };
    /* The Arduino core already inits the TWDT; reconfigure rather than re-init. */
    if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE)
        esp_task_wdt_reconfigure(&wdt);
    esp_task_wdt_add(NULL);

    Serial.printf("Valve controller ready: %d relay(s), %d zone slots\n",
                  NUM_RELAYS, MAX_ZONE_SLOTS);
}

void loop()
{
    server.handleClient();

    static uint32_t last_ctrl;
    if (millis() - last_ctrl >= 1000) {
        last_ctrl = millis();
        control_tick();
    }

    if (millis() - s_last_soil >= SOIL_REFRESH_MS) {
        s_last_soil = millis();
        if (WiFi.status() == WL_CONNECTED) refresh_soil();
    }

    /* Auto-reconnect WiFi (watering keeps running on local schedule regardless). */
    static uint32_t last_wifi;
    if (WiFi.status() != WL_CONNECTED && millis() - last_wifi > 10000) {
        last_wifi = millis();
        WiFi.reconnect();
    }

    esp_task_wdt_reset();
    delay(5);
}
