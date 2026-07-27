/**
 * 2.4 GHz Recon — WiFi + BLE scanner. See net_tool.h for scope + threading.
 *
 * Scan results are copied out of the volatile WiFi/BLE APIs into fixed struct
 * arrays as soon as a scan finishes. Everything the UI shows — list rows, the
 * tap-a-row detail overlay, the channel graph — reads from those caches, so
 * detail stays valid long after the underlying scan buffers are freed.
 */
#include "net_tool.h"
#include "lvgl_v9_port.h"

#include <lvgl.h>
#include <Arduino.h>
#include <cstdarg>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>

/* ------------------------------------------------------------------ */
#define C_BG      0x0A0F1C
#define C_CARD    0x141E33
#define C_INNER   0x1E2A45
#define C_TEXT    0xE8EFFA
#define C_MUTED   0x8296B4
#define C_ACCENT  0x3B82F6
#define C_OK      0x22C55E
#define C_WARN    0xF59E0B
#define C_BAD     0xEF4444
#define C_CYAN    0x22D3EE
#define C_PURPLE  0xA855F7

#define MAX_ROWS  40

/* ------------------------------------------------------------------ */
/* Cached scan results                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char ssid[33];
    char bssid[18];
    int  channel;
    int  rssi;
    wifi_auth_mode_t auth;
} wifi_ap_t;

typedef struct {
    char     name[32];
    char     addr[18];
    int      rssi;
    uint8_t  addr_type;
    int8_t   txpower;
    bool     have_tx;
    uint16_t appearance;
    int      svc_count;
    char     svc0[42];
    bool     have_mfg;
    int      mfg_len;
    char     mfg_hex[26];

    /* Decoded from the raw advertisement payload (no connection needed). */
    char     mfr[24];         /* manufacturer name from company ID, or "" */
    char     adv_svc[24];     /* friendly name of an advertised service, or "" */
    char     decoded[3][32];  /* human-readable sensor readings */
    int      decoded_count;
} ble_dev_t;

static wifi_ap_t s_aps[MAX_ROWS];
static int       s_ap_count;
static ble_dev_t s_bles[MAX_ROWS];
static int       s_ble_count;

/* GATT discovery cache — filled when we connect to a device. */
#define MAX_SVC 14
#define MAX_CHR 48
typedef struct {
    char    uuid[42];
    uint8_t chr_start;   /* index into s_chrs */
    uint8_t chr_count;
} gatt_svc_t;
typedef struct {
    char uuid[42];
    bool r, w, n;
    bool has_value;
    char value[36];      /* read preview: text if printable, else hex */
} gatt_chr_t;

static gatt_svc_t s_svcs[MAX_SVC];
static int        s_svc_count;
static gatt_chr_t s_chrs[MAX_CHR];
static int        s_chr_count;
static char       s_conn_msg[64];   /* connection result line */
static bool       s_conn_ok;

/* Cross-task state: set by the LVGL side, read by the loop() side. */
typedef enum { IDLE, REQ_WIFI, REQ_BLE, REQ_CONNECT, RUNNING } scan_state_t;
static volatile scan_state_t s_state = IDLE;
static volatile int s_connect_idx = -1;
static bool s_ble_inited = false;

/** Friendly name for a standard 16-bit BLE UUID, or NULL. */
static const char *uuid_friendly(const char *uuid)
{
    /* Standard UUIDs render as 0000XXXX-0000-1000-8000-00805f9b34fb. */
    if (strlen(uuid) < 8) return NULL;
    if (strcmp(uuid + 8, "-0000-1000-8000-00805f9b34fb") != 0) return NULL;
    char code[5] = { uuid[4], uuid[5], uuid[6], uuid[7], 0 };
    struct { const char *c, *n; } tbl[] = {
        {"1800", "Generic Access"},   {"1801", "Generic Attribute"},
        {"180a", "Device Info"},       {"180f", "Battery"},
        {"180d", "Heart Rate"},        {"181a", "Environmental"},
        {"1809", "Health Thermometer"},{"1816", "Cycling Speed"},
        {"2a00", "Device Name"},       {"2a19", "Battery Level"},
        {"2a29", "Manufacturer"},      {"2a24", "Model Number"},
        {"2a25", "Serial Number"},     {"2a26", "Firmware Rev"},
        {"2a37", "Heart Rate Meas"},   {"2a6e", "Temperature"},
        {"2a6f", "Humidity"},          {"2a01", "Appearance"},
    };
    for (unsigned i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (strcasecmp(code, tbl[i].c) == 0) return tbl[i].n;
    }
    return NULL;
}

static lv_obj_t *s_wifi_list, *s_ble_list, *s_chan_chart;
static lv_obj_t *s_wifi_status, *s_ble_status, *s_chan_status;
static lv_obj_t *s_wifi_btn, *s_ble_btn;
static lv_obj_t *s_wifi_spin, *s_ble_spin;
static lv_chart_series_t *s_chan_ser;

static lv_obj_t *s_overlay, *s_overlay_body;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t rssi_color(int rssi)
{
    if (rssi >= -60) return C_OK;
    if (rssi >= -72) return C_WARN;
    return C_BAD;
}

static int rssi_bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    if (rssi >= -88) return 1;
    return 0;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        default:                        return "unknown";
    }
}

static const char *addr_type_name(uint8_t t)
{
    switch (t) {
        case 0:  return "public";
        case 1:  return "random";
        case 2:  return "public-id";
        case 3:  return "random-id";
        default: return "?";
    }
}

static void signal_bars(lv_obj_t *parent, int bars, uint32_t color)
{
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        int h = 6 + i * 6;
        lv_obj_set_size(b, 7, h);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -14 + i * 10 - 30, (24 - h) / 2);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < bars ? color : C_INNER), 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void set_scanning_ui(lv_obj_t *status, lv_obj_t *btn, lv_obj_t *spin,
                            bool scanning, const char *msg)
{
    lv_label_set_text(status, msg);
    if (scanning) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_remove_flag(spin, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
        lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, const lv_font_t *font,
                          uint32_t color, lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, x, y);
    return l;
}

/* ------------------------------------------------------------------ */
/* Detail overlay                                                      */
/* ------------------------------------------------------------------ */

static void overlay_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/** One "LABEL: value" line inside the detail card. */
static void detail_line(const char *key, const char *val, uint32_t vcolor)
{
    lv_obj_t *row = lv_obj_create(s_overlay_body);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(C_MUTED), 0);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(vcolor), 0);
    lv_obj_set_width(v, 560);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 150, 0);
}

static void open_overlay(const char *title, uint32_t accent)
{
    lv_obj_clean(s_overlay_body);
    lv_obj_t *card = lv_obj_get_parent(s_overlay_body);
    lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);

    lv_obj_t *hdr = (lv_obj_t *)lv_obj_get_user_data(s_overlay);   /* title label */
    lv_label_set_text(hdr, title);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

static void wifi_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ap_count) return;
    wifi_ap_t *a = &s_aps[i];

    open_overlay(a->ssid[0] ? a->ssid : "<hidden>", C_ACCENT);

    char buf[64];
    detail_line("SIGNAL", (snprintf(buf, sizeof buf, "%d dBm", a->rssi), buf),
                rssi_color(a->rssi));
    detail_line("SECURITY", auth_name(a->auth),
                a->auth == WIFI_AUTH_OPEN ? C_WARN : C_TEXT);
    detail_line("CHANNEL", (snprintf(buf, sizeof buf, "%d  (2.4 GHz)", a->channel), buf), C_TEXT);
    detail_line("BSSID", a->bssid, C_TEXT);
    if (a->auth == WIFI_AUTH_OPEN) {
        detail_line("NOTE", "Open network — no link-layer encryption", C_WARN);
    }
}

/** Add a CONNECT button to the detail overlay for BLE device index i. */
static void connect_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_state != IDLE) return;
    s_connect_idx = i;

    /* Immediate feedback; the real work happens in net_tool_loop(). */
    lv_obj_clean(s_overlay_body);
    lv_obj_t *sp = lv_spinner_create(s_overlay_body);
    lv_obj_set_size(sp, 60, 60);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_PURPLE), LV_PART_INDICATOR);
    lv_obj_t *msg = lv_label_create(s_overlay_body);
    lv_label_set_text(msg, "Connecting — reading services...");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(C_TEXT), 0);

    s_state = REQ_CONNECT;
}

static void add_connect_button(int i)
{
    lv_obj_t *btn = lv_button_create(s_overlay_body);
    lv_obj_set_size(btn, 200, 54);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_PURPLE), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, LV_SYMBOL_BLUETOOTH "  CONNECT");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, connect_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}

static void ble_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ble_count) return;
    ble_dev_t *d = &s_bles[i];

    open_overlay(d->name[0] ? d->name : (d->mfr[0] ? d->mfr : "(unnamed device)"), C_PURPLE);

    char buf[64];
    /* Lead with what it's broadcasting — the passive, no-connection payload. */
    for (int k = 0; k < d->decoded_count; k++) {
        detail_line(k == 0 ? "BROADCAST" : "", d->decoded[k], C_OK);
    }
    if (d->mfr[0])     detail_line("MAKER", d->mfr, C_TEXT);
    if (d->adv_svc[0]) detail_line("ADVERTISES", d->adv_svc, C_CYAN);

    detail_line("SIGNAL", (snprintf(buf, sizeof buf, "%d dBm", d->rssi), buf),
                rssi_color(d->rssi));
    detail_line("ADDRESS", d->addr, C_TEXT);
    detail_line("ADDR TYPE", addr_type_name(d->addr_type), C_TEXT);
    if (d->have_tx) {
        detail_line("TX POWER", (snprintf(buf, sizeof buf, "%d dBm", d->txpower), buf), C_TEXT);
    }
    if (d->appearance) {
        detail_line("APPEARANCE", (snprintf(buf, sizeof buf, "0x%04X", d->appearance), buf), C_TEXT);
    }
    if (d->svc_count > 0) {
        detail_line("SERVICES", (snprintf(buf, sizeof buf, "%d advertised", d->svc_count), buf), C_CYAN);
        detail_line("PRIMARY", d->svc0, C_TEXT);
    } else {
        detail_line("SERVICES", "none advertised", C_MUTED);
    }
    if (d->have_mfg) {
        detail_line("MFG DATA", (snprintf(buf, sizeof buf, "%d bytes: %s%s", d->mfg_len,
                     d->mfg_hex, d->mfg_len > 12 ? "..." : ""), buf), C_TEXT);
    }
    add_connect_button(i);
}

/** Render the discovered GATT tree into the overlay body. */
static void render_gatt(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_overlay_body);

    lv_obj_t *status = lv_label_create(s_overlay_body);
    lv_label_set_text(status, s_conn_msg);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(s_conn_ok ? C_OK : C_BAD), 0);

    for (int si = 0; si < s_svc_count; si++) {
        gatt_svc_t *sv = &s_svcs[si];
        const char *fname = uuid_friendly(sv->uuid);

        lv_obj_t *sh = lv_label_create(s_overlay_body);
        char sbuf[80];
        snprintf(sbuf, sizeof sbuf, LV_SYMBOL_DIRECTORY "  %s", fname ? fname : sv->uuid);
        lv_label_set_text(sh, sbuf);
        lv_obj_set_style_text_font(sh, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(sh, lv_color_hex(C_CYAN), 0);

        for (int ci = sv->chr_start; ci < sv->chr_start + sv->chr_count; ci++) {
            gatt_chr_t *ch = &s_chrs[ci];
            const char *cn = uuid_friendly(ch->uuid);

            lv_obj_t *row = lv_label_create(s_overlay_body);
            char cbuf[140];
            char props[8] = "";
            if (ch->r) strcat(props, "R");
            if (ch->w) strcat(props, "W");
            if (ch->n) strcat(props, "N");
            if (ch->has_value) {
                snprintf(cbuf, sizeof cbuf, "    [%s] %s  =  %s",
                         props, cn ? cn : ch->uuid, ch->value);
            } else {
                snprintf(cbuf, sizeof cbuf, "    [%s] %s", props, cn ? cn : ch->uuid);
            }
            lv_label_set_text(row, cbuf);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(row,
                lv_color_hex(ch->has_value ? C_TEXT : C_MUTED), 0);
            lv_obj_set_width(row, 640);
            lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_WRAP);
        }
    }

    if (s_svc_count == 0 && s_conn_ok) {
        lv_obj_t *none = lv_label_create(s_overlay_body);
        lv_label_set_text(none, "Connected, but no services discovered.");
        lv_obj_set_style_text_color(none, lv_color_hex(C_MUTED), 0);
    }
    lvgl_v9_port_unlock();
}

/* ------------------------------------------------------------------ */
/* Button callbacks — LVGL context, lock held; only flag work.         */
/* ------------------------------------------------------------------ */

static void wifi_scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_state != IDLE) return;
    set_scanning_ui(s_wifi_status, s_wifi_btn, s_wifi_spin, true, "Scanning 2.4 GHz...");
    s_state = REQ_WIFI;
}

static void ble_scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_state != IDLE) return;
    set_scanning_ui(s_ble_status, s_ble_btn, s_ble_spin, true, "Listening for advertisements...");
    s_state = REQ_BLE;
}

/* ------------------------------------------------------------------ */
/* Row rendering                                                       */
/* ------------------------------------------------------------------ */

static lv_obj_t *result_row(lv_obj_t *list, uint32_t edge, lv_event_cb_t cb, int idx)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_INNER), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(edge), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return row;
}

static void populate_channels(void)
{
    int32_t count[13] = {0};
    for (int i = 0; i < s_ap_count; i++) {
        int c = s_aps[i].channel;
        if (c >= 1 && c <= 13) count[c - 1]++;
    }
    lv_chart_set_series_values(s_chan_chart, s_chan_ser, count, 13);
    lv_chart_refresh(s_chan_chart);

    char msg[64];
    snprintf(msg, sizeof msg, "%d APs across 2.4 GHz   ·   1 / 6 / 11 don't overlap",
             s_ap_count);
    lv_label_set_text(s_chan_status, msg);
}

static void render_wifi(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_wifi_list);

    if (s_ap_count == 0) {
        mk_label(s_wifi_list, "No networks found", &lv_font_montserrat_20,
                 C_MUTED, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (int i = 0; i < s_ap_count; i++) {
            wifi_ap_t *a = &s_aps[i];
            bool open = (a->auth == WIFI_AUTH_OPEN);
            lv_obj_t *row = result_row(s_wifi_list, rssi_color(a->rssi), wifi_row_cb, i);

            mk_label(row, a->ssid[0] ? a->ssid : "<hidden>", &lv_font_montserrat_20,
                     C_TEXT, LV_ALIGN_TOP_LEFT, 14, 8);
            char sub[80];
            snprintf(sub, sizeof sub, "%s   ch %d   %s   %d dBm",
                     a->bssid, a->channel, auth_name(a->auth), a->rssi);
            mk_label(row, sub, &lv_font_montserrat_14, open ? C_WARN : C_MUTED,
                     LV_ALIGN_BOTTOM_LEFT, 14, -8);
            if (open) {
                mk_label(row, LV_SYMBOL_WARNING, &lv_font_montserrat_20, C_WARN,
                         LV_ALIGN_RIGHT_MID, -70, 0);
            }
            signal_bars(row, rssi_bars(a->rssi), rssi_color(a->rssi));
        }
    }

    char msg[56];
    snprintf(msg, sizeof msg, "%d networks   ·   tap a row for detail", s_ap_count);
    set_scanning_ui(s_wifi_status, s_wifi_btn, s_wifi_spin, false, msg);
    populate_channels();
    lvgl_v9_port_unlock();
}

static void render_ble(void)
{
    lvgl_v9_port_lock(-1);
    lv_obj_clean(s_ble_list);

    if (s_ble_count == 0) {
        mk_label(s_ble_list, "No devices found", &lv_font_montserrat_20,
                 C_MUTED, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (int i = 0; i < s_ble_count; i++) {
            ble_dev_t *d = &s_bles[i];
            lv_obj_t *row = result_row(s_ble_list, rssi_color(d->rssi), ble_row_cb, i);

            const char *title = d->name[0] ? d->name : (d->mfr[0] ? d->mfr : "(no name)");
            mk_label(row, title, &lv_font_montserrat_20,
                     (d->name[0] || d->mfr[0]) ? C_TEXT : C_MUTED, LV_ALIGN_TOP_LEFT, 14, 8);

            char sub[110];
            const char *tag = d->adv_svc[0] ? d->adv_svc :
                              (d->name[0] && d->mfr[0] ? d->mfr : "");
            if (tag[0]) {
                snprintf(sub, sizeof sub, "%s   %s   %d dBm   %s",
                         d->addr, addr_type_name(d->addr_type), d->rssi, tag);
            } else {
                snprintf(sub, sizeof sub, "%s   %s   %d dBm",
                         d->addr, addr_type_name(d->addr_type), d->rssi);
            }
            mk_label(row, sub, &lv_font_montserrat_14, C_MUTED, LV_ALIGN_BOTTOM_LEFT, 14, -8);
            signal_bars(row, rssi_bars(d->rssi), rssi_color(d->rssi));

            /* The satisfying bit: a live reading straight off the broadcast. */
            if (d->decoded_count > 0) {
                mk_label(row, d->decoded[0], &lv_font_montserrat_20, C_OK,
                         LV_ALIGN_RIGHT_MID, -76, 0);
            }
        }
    }

    char msg[56];
    snprintf(msg, sizeof msg, "%d devices   ·   tap a row for detail", s_ble_count);
    set_scanning_ui(s_ble_status, s_ble_btn, s_ble_spin, false, msg);
    lvgl_v9_port_unlock();
}

/* ------------------------------------------------------------------ */
/* loop() side — blocking scans + cache population                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Advertisement decoding (passive — no connection)                    */
/* ------------------------------------------------------------------ */

/** SIG company ID (little-endian in the payload) to a friendly name. */
static const char *company_name(uint16_t id)
{
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0075: return "Samsung";
        case 0x0087: return "Garmin";
        case 0x0157: return "Huawei";
        case 0x012D: return "Sony";
        case 0x038F: return "Xiaomi";
        case 0x0499: return "Ruuvi";
        case 0x0059: return "Nordic";
        case 0x004F: return "Amazon";
        case 0x0171: return "Amazfit/Huami";
        default:     return NULL;
    }
}

static void add_decoded(ble_dev_t *o, const char *fmt, ...)
{
    if (o->decoded_count >= 3) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->decoded[o->decoded_count], sizeof o->decoded[0], fmt, ap);
    va_end(ap);
    o->decoded_count++;
}

/** Decode a Service Data block if it's a known sensor format. */
static void decode_service_data(ble_dev_t *o, uint16_t uuid,
                                const uint8_t *d, uint8_t n)
{
    if (uuid == 0xFEAA && n >= 14 && d[0] == 0x20) {       /* Eddystone TLM */
        uint16_t mv = (d[1] << 8) | d[2];
        int16_t  tr = (int16_t)((d[3] << 8) | d[4]);        /* 8.8 fixed, BE */
        if (mv) add_decoded(o, "Battery %.2f V", mv / 1000.0);
        add_decoded(o, "Temp %.1f C", tr / 256.0);
    } else if (uuid == 0xFCD2) {                            /* BTHome v2 */
        uint8_t i = 1;                                      /* skip device-info byte */
        while (i < n) {
            uint8_t id = d[i++];
            if (id == 0x00 && i < n)             { i += 1; }          /* packet id */
            else if (id == 0x01 && i < n)        { add_decoded(o, "Battery %d%%", d[i]); i += 1; }
            else if (id == 0x02 && i + 1 < n)    { int16_t t = (int16_t)(d[i] | (d[i+1] << 8)); add_decoded(o, "Temp %.2f C", t * 0.01); i += 2; }
            else if (id == 0x03 && i + 1 < n)    { uint16_t h = d[i] | (d[i+1] << 8); add_decoded(o, "Humidity %.1f%%", h * 0.01); i += 2; }
            else if (id == 0x2E && i < n)        { add_decoded(o, "Humidity %d%%", d[i]); i += 1; }
            else break;                                     /* unknown object size — stop */
        }
    } else if (uuid == 0xFE95) {
        strlcpy(o->adv_svc, "Xiaomi/Mijia", sizeof o->adv_svc);
    }
}

/** Walk the raw advertisement payload: [len][type][data...] structures. */
static void parse_advertisement(BLEAdvertisedDevice &adv, ble_dev_t *o)
{
    o->mfr[0] = o->adv_svc[0] = '\0';
    o->decoded_count = 0;

    uint8_t *p = adv.getPayload();
    size_t plen = adv.getPayloadLength();
    if (!p) return;

    size_t i = 0;
    while (i + 1 < plen) {
        uint8_t len = p[i];
        if (len == 0 || i + 1 + len > plen) break;
        uint8_t type = p[i + 1];
        const uint8_t *data = &p[i + 2];
        uint8_t dlen = len - 1;

        if (type == 0xFF && dlen >= 2) {                    /* Manufacturer Data */
            uint16_t cid = data[0] | (data[1] << 8);
            const char *cn = company_name(cid);
            if (cn) strlcpy(o->mfr, cn, sizeof o->mfr);
            else    snprintf(o->mfr, sizeof o->mfr, "0x%04X", cid);
            if (cid == 0x0499 && dlen >= 7 && data[2] == 0x05) {   /* Ruuvi RAWv2 */
                int16_t t = (int16_t)((data[3] << 8) | data[4]);
                add_decoded(o, "Temp %.2f C", t * 0.005);
                uint16_t h = (data[5] << 8) | data[6];
                add_decoded(o, "Humidity %.2f%%", h * 0.0025);
            }
        } else if (type == 0x16 && dlen >= 2) {             /* Service Data, 16-bit */
            uint16_t su = data[0] | (data[1] << 8);
            decode_service_data(o, su, data + 2, dlen - 2);
        } else if ((type == 0x02 || type == 0x03) && dlen >= 2 && !o->adv_svc[0]) {
            uint16_t su = data[0] | (data[1] << 8);          /* advertised 16-bit service */
            char full[42];
            snprintf(full, sizeof full, "0000%04x-0000-1000-8000-00805f9b34fb", su);
            const char *fn = uuid_friendly(full);
            if (fn) strlcpy(o->adv_svc, fn, sizeof o->adv_svc);
        }
        i += len + 1;
    }
}

static void cache_wifi(int16_t n)
{
    if (n < 0) n = 0;
    if (n > MAX_ROWS) n = MAX_ROWS;
    s_ap_count = n;
    for (int i = 0; i < n; i++) {
        strlcpy(s_aps[i].ssid, WiFi.SSID(i).c_str(), sizeof s_aps[i].ssid);
        strlcpy(s_aps[i].bssid, WiFi.BSSIDstr(i).c_str(), sizeof s_aps[i].bssid);
        s_aps[i].channel = WiFi.channel(i);
        s_aps[i].rssi    = WiFi.RSSI(i);
        s_aps[i].auth    = WiFi.encryptionType(i);
    }
}

static void cache_ble(BLEScanResults *res)
{
    int n = res ? res->getCount() : 0;
    if (n > MAX_ROWS) n = MAX_ROWS;
    s_ble_count = n;
    for (int i = 0; i < n; i++) {
        BLEAdvertisedDevice d = res->getDevice(i);
        ble_dev_t *o = &s_bles[i];
        strlcpy(o->name, d.haveName() ? d.getName().c_str() : "", sizeof o->name);
        strlcpy(o->addr, d.getAddress().toString().c_str(), sizeof o->addr);
        o->rssi       = d.getRSSI();
        o->addr_type  = d.getAddressType();
        o->have_tx    = d.haveTXPower();
        o->txpower    = o->have_tx ? d.getTXPower() : 0;
        o->appearance = d.getAppearance();
        o->svc_count  = d.getServiceUUIDCount();
        o->svc0[0]    = '\0';
        if (o->svc_count > 0) {
            strlcpy(o->svc0, d.getServiceUUID().toString().c_str(), sizeof o->svc0);
        }
        o->have_mfg = d.haveManufacturerData();
        o->mfg_hex[0] = '\0';
        o->mfg_len = 0;
        if (o->have_mfg) {
            String m = d.getManufacturerData();
            o->mfg_len = m.length();
            int show = o->mfg_len < 12 ? o->mfg_len : 12;
            char *p = o->mfg_hex;
            for (int b = 0; b < show; b++) {
                p += snprintf(p, 3, "%02X", (uint8_t)m[b]);
            }
        }
        parse_advertisement(d, o);
    }
}

/** Format a raw characteristic value: text if printable, else hex. */
static void format_value(const String &v, char *out, size_t outsz)
{
    int n = v.length();
    if (n == 0) { strlcpy(out, "(empty)", outsz); return; }

    bool printable = true;
    for (int i = 0; i < n && i < 24; i++) {
        uint8_t c = (uint8_t)v[i];
        if (c < 0x20 || c > 0x7E) { printable = false; break; }
    }
    if (printable) {
        int show = n < 24 ? n : 24;
        snprintf(out, outsz, "\"%.*s\"%s", show, v.c_str(), n > 24 ? "..." : "");
    } else {
        char *p = out;
        int show = n < 10 ? n : 10;
        for (int i = 0; i < show && (p - out) < (int)outsz - 4; i++) {
            p += snprintf(p, 4, "%02X ", (uint8_t)v[i]);
        }
        if (n > 10) strlcat(out, "...", outsz);
    }
}

static BLEClient *s_client = nullptr;

static void do_connect(int idx)
{
    s_svc_count = 0;
    s_chr_count = 0;

    if (idx < 0 || idx >= s_ble_count) {
        s_conn_ok = false;
        strlcpy(s_conn_msg, "Bad device index", sizeof s_conn_msg);
        return;
    }
    ble_dev_t *d = &s_bles[idx];

    if (!s_client) s_client = BLEDevice::createClient();

    Serial.printf("GATT: connecting to %s (type %d)...\n", d->addr, d->addr_type);
    bool ok = s_client->connect(BLEAddress(String(d->addr), d->addr_type),
                                d->addr_type, 8000);
    if (!ok) {
        s_conn_ok = false;
        snprintf(s_conn_msg, sizeof s_conn_msg, "Could not connect to %s", d->addr);
        Serial.println("GATT: connect failed");
        return;
    }

    std::map<std::string, BLERemoteService *> *services = s_client->getServices();
    for (auto &sv : *services) {
        if (s_svc_count >= MAX_SVC) break;
        gatt_svc_t *os = &s_svcs[s_svc_count];
        strlcpy(os->uuid, sv.second->getUUID().toString().c_str(), sizeof os->uuid);
        os->chr_start = s_chr_count;
        os->chr_count = 0;

        auto *chars = sv.second->getCharacteristics();
        for (auto &ch : *chars) {
            if (s_chr_count >= MAX_CHR) break;
            BLERemoteCharacteristic *rc = ch.second;
            gatt_chr_t *oc = &s_chrs[s_chr_count];
            strlcpy(oc->uuid, rc->getUUID().toString().c_str(), sizeof oc->uuid);
            oc->r = rc->canRead();
            oc->w = rc->canWrite();
            oc->n = rc->canNotify();
            oc->has_value = false;
            oc->value[0] = '\0';
            if (oc->r) {
                String val = rc->readValue();
                format_value(val, oc->value, sizeof oc->value);
                oc->has_value = true;
            }
            s_chr_count++;
            os->chr_count++;
        }
        s_svc_count++;
    }

    s_conn_ok = true;
    snprintf(s_conn_msg, sizeof s_conn_msg, "Connected — %d services, %d characteristics",
             s_svc_count, s_chr_count);
    Serial.printf("GATT: %s\n", s_conn_msg);

    s_client->disconnect();   /* snapshot taken; release the link */
}

void net_tool_loop(void)
{
    if (s_state == REQ_CONNECT) {
        s_state = RUNNING;
        do_connect(s_connect_idx);
        render_gatt();
        s_state = IDLE;
        return;
    }
    if (s_state == REQ_WIFI) {
        s_state = RUNNING;
        WiFi.mode(WIFI_STA);          /* reclaim the radio if BLE parked it */
        WiFi.disconnect();
        int16_t n = WiFi.scanNetworks(false, false);
        cache_wifi(n);
        WiFi.scanDelete();
        render_wifi();
        s_state = IDLE;
    } else if (s_state == REQ_BLE) {
        s_state = RUNNING;

        /* Give BLE the radio to itself. A live WiFi STA shares the single
         * 2.4 GHz radio and can starve the BLE scan to zero results. */
        WiFi.mode(WIFI_OFF);

        if (!s_ble_inited) {
            Serial.println("BLE: init...");
            BLEDevice::init("");
            s_ble_inited = true;
            Serial.println("BLE: init done");
        }
        BLEScan *scan = BLEDevice::getScan();
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);

        Serial.println("BLE: start (6s)...");
        BLEScanResults *res = scan->start(6, false);
        int cnt = res ? res->getCount() : -1;
        Serial.printf("BLE: start returned, count=%d\n", cnt);

        cache_ble(res);
        scan->clearResults();
        render_ble();
        s_state = IDLE;
    }
}

/* ------------------------------------------------------------------ */
/* UI build                                                            */
/* ------------------------------------------------------------------ */

static void build_scan_tab(lv_obj_t *tab, uint32_t accent, lv_obj_t **out_list,
                           lv_obj_t **out_status, lv_obj_t **out_btn,
                           lv_obj_t **out_spin, lv_event_cb_t cb)
{
    lv_obj_set_style_bg_color(tab, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(tab);
    lv_obj_set_size(btn, 150, 56);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "SCAN");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    *out_btn = btn;

    lv_obj_t *spin = lv_spinner_create(tab);
    lv_obj_set_size(spin, 44, 44);
    lv_obj_align(spin, LV_ALIGN_TOP_LEFT, 164, 6);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
    *out_spin = spin;

    *out_status = mk_label(tab, "Tap SCAN to begin", &lv_font_montserrat_20,
                           C_MUTED, LV_ALIGN_TOP_LEFT, 220, 16);

    lv_obj_t *list = lv_obj_create(tab);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100) - 72);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 10, 0);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    *out_list = list;

    mk_label(list, "Results appear here", &lv_font_montserrat_20,
             C_MUTED, LV_ALIGN_CENTER, 0, 0);
}

static void build_channels_tab(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_chan_status = mk_label(tab, "Scan WiFi to populate the channel map",
                             &lv_font_montserrat_20, C_MUTED, LV_ALIGN_TOP_LEFT, 4, 6);

    s_chan_chart = lv_chart_create(tab);
    lv_obj_set_size(s_chan_chart, LV_PCT(100), LV_PCT(100) - 90);
    lv_obj_align(s_chan_chart, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_chart_set_type(s_chan_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chan_chart, 13);
    lv_chart_set_axis_range(s_chan_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 8);
    lv_obj_set_style_bg_color(s_chan_chart, lv_color_hex(0x0E1524), 0);
    lv_obj_set_style_border_width(s_chan_chart, 0, 0);
    lv_obj_set_style_radius(s_chan_chart, 10, 0);
    lv_obj_set_style_pad_column(s_chan_chart, 6, 0);
    lv_chart_set_div_line_count(s_chan_chart, 5, 0);

    s_chan_ser = lv_chart_add_series(s_chan_chart, lv_color_hex(C_CYAN),
                                     LV_CHART_AXIS_PRIMARY_Y);
    for (int c = 0; c < 13; c++) lv_chart_set_next_value(s_chan_chart, s_chan_ser, 0);

    /* Channel-number labels under the bars. */
    lv_obj_t *labels = lv_obj_create(tab);
    lv_obj_set_size(labels, LV_PCT(100), 24);
    lv_obj_align(labels, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_obj_set_style_bg_opa(labels, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(labels, 0, 0);
    lv_obj_set_style_pad_all(labels, 0, 0);
    lv_obj_remove_flag(labels, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(labels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(labels, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int c = 1; c <= 13; c++) {
        char n[4]; snprintf(n, sizeof n, "%d", c);
        bool clear = (c == 1 || c == 6 || c == 11);
        lv_obj_t *l = lv_label_create(labels);
        lv_label_set_text(l, n);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(clear ? C_CYAN : C_MUTED), 0);
    }
}

static void build_overlay(lv_obj_t *scr)
{
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);   /* eat background taps */
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_set_size(card, 720, 460);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(title, 560);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_user_data(s_overlay, title);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, 56, 56);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_t *x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    lv_obj_add_event_cb(close, overlay_close_cb, LV_EVENT_CLICKED, NULL);

    s_overlay_body = lv_obj_create(card);
    lv_obj_set_size(s_overlay_body, LV_PCT(100), 350);
    lv_obj_align(s_overlay_body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay_body, 0, 0);
    lv_obj_set_style_pad_all(s_overlay_body, 0, 0);
    lv_obj_set_style_pad_row(s_overlay_body, 10, 0);
    lv_obj_set_flex_flow(s_overlay_body, LV_FLEX_FLOW_COLUMN);
}

void net_tool_create(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_size(tv, 60);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_20, 0);

    build_scan_tab(lv_tabview_add_tab(tv, LV_SYMBOL_WIFI "  WiFi"), C_ACCENT,
                   &s_wifi_list, &s_wifi_status, &s_wifi_btn, &s_wifi_spin, wifi_scan_cb);
    build_scan_tab(lv_tabview_add_tab(tv, LV_SYMBOL_BLUETOOTH "  BLE"), C_PURPLE,
                   &s_ble_list, &s_ble_status, &s_ble_btn, &s_ble_spin, ble_scan_cb);
    build_channels_tab(lv_tabview_add_tab(tv, LV_SYMBOL_LIST "  Channels"));

    build_overlay(scr);
}
