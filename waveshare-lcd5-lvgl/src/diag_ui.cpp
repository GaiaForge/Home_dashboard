/**
 * GaiaForge Field Diagnostics — UI mockup (see diag_ui.h for the data model).
 *
 * Layout: 1024x600, master-detail.
 *   header  1024 x  76
 *   list     352 x 524   (scrollable, one row per unit)
 *   detail   672 x 524   (scrollable, rebuilt on selection)
 */
#include "diag_ui.h"

/* ------------------------------------------------------------------ */
/* Palette — dark, high contrast, chosen for outdoor glare            */
/* ------------------------------------------------------------------ */
#define C_BG        0x0B1220
#define C_PANEL     0x131C2E
#define C_CARD      0x1B2740
#define C_LINE      0x24314F
#define C_TEXT      0xE6EDF7
#define C_MUTED     0x8296B4
#define C_ACCENT    0x2563EB
#define C_OK        0x22C55E
#define C_WARN      0xF59E0B
#define C_BAD       0xEF4444
#define C_OFF       0x64748B

#define HEADER_H    76
#define LIST_W      352
#define ROW_H       92          /* >= 60 px: gloved-hand touch target */

static lv_obj_t   *s_detail;            /* right-hand pane, cleared on select */
static lv_obj_t   *s_rows[16];          /* list rows, for selection highlight */
static uint8_t     s_selected;

static lv_timer_t *s_fetch_timer;       /* coredump fetch simulation */
static lv_obj_t   *s_fetch_bar;
static lv_obj_t   *s_fetch_status;
static int         s_fetch_pct;

static lv_obj_t   *s_clock;
static int         s_clock_s = 14 * 3600 + 32 * 60;   /* fake wall clock */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t status_color(diag_status_t s)
{
    switch (s) {
        case DIAG_OK:      return C_OK;
        case DIAG_WARN:    return C_WARN;
        case DIAG_PANIC:   return C_BAD;
        default:           return C_OFF;
    }
}

static const char *status_text(diag_status_t s)
{
    switch (s) {
        case DIAG_OK:      return "HEALTHY";
        case DIAG_WARN:    return "WARNING";
        case DIAG_PANIC:   return "PANIC";
        default:           return "OFFLINE";
    }
}

/** A flat panel with no border/padding — the base for every surface here. */
static lv_obj_t *panel(lv_obj_t *parent, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/** Rounded status chip, e.g. PANIC / HEALTHY. */
static lv_obj_t *pill(lv_obj_t *parent, const char *s, uint32_t color)
{
    lv_obj_t *p = panel(parent, color, 12);
    lv_obj_set_size(p, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_t *l = text(p, s, &lv_font_montserrat_14, 0x0B1220);
    lv_obj_center(l);
    return p;
}

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

static void clock_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_clock_s = (s_clock_s + 1) % 86400;
    lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d",
                          s_clock_s / 3600, (s_clock_s / 60) % 60, s_clock_s % 60);
}

static void build_header(lv_obj_t *parent)
{
    lv_obj_t *h = panel(parent, C_PANEL, 0);
    lv_obj_set_size(h, LV_PCT(100), HEADER_H);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *title = text(h, "Field Diagnostics", &lv_font_montserrat_24, C_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, -10);

    lv_obj_t *sub = text(h, "GaiaForge  |  6 units in range", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 24, 16);

    /* Never let a screenshot of this be mistaken for live fleet data. */
    lv_obj_t *mock = pill(h, "MOCK DATA", C_WARN);
    lv_obj_align(mock, LV_ALIGN_LEFT_MID, 300, 0);

    s_clock = text(h, "14:32:00", &lv_font_montserrat_24, C_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, -24, -10);
    lv_timer_create(clock_tick, 1000, nullptr);

    lv_obj_t *sd = text(h, "SD  OK  |  BATT  82%", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(sd, LV_ALIGN_RIGHT_MID, -24, 16);
}

/* ------------------------------------------------------------------ */
/* Detail pane                                                         */
/* ------------------------------------------------------------------ */

static lv_obj_t *stat_tile(lv_obj_t *parent, const char *label, const char *value, uint32_t color)
{
    lv_obj_t *t = panel(parent, C_CARD, 10);
    lv_obj_set_size(t, 151, 88);

    lv_obj_t *v = text(t, value, &lv_font_montserrat_32, color);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 14, 10);

    lv_obj_t *l = text(t, label, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 14, -10);
    return t;
}

static void fetch_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_fetch_pct += 4;

    if (s_fetch_pct >= 100) {
        s_fetch_pct = 100;
        lv_bar_set_value(s_fetch_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_fetch_status,
                          "Saved  ORF-BAS-003_20260725_1432.core  ->  /sd/coredumps");
        lv_obj_set_style_text_color(s_fetch_status, lv_color_hex(C_OK), 0);
        lv_timer_delete(s_fetch_timer);
        s_fetch_timer = nullptr;
        return;
    }

    lv_bar_set_value(s_fetch_bar, s_fetch_pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_fetch_status, "Fetching over BLE...  %d%%  (%d / 64 KB)",
                          s_fetch_pct, (65536 * s_fetch_pct / 100) / 1024);
}

static void fetch_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_fetch_timer) {
        return;                              /* already running */
    }
    s_fetch_pct = 0;
    lv_obj_set_style_text_color(s_fetch_status, lv_color_hex(C_MUTED), 0);
    s_fetch_timer = lv_timer_create(fetch_tick, 90, nullptr);
}

static void build_detail(const diag_device_t *d)
{
    /* A fetch in flight refers to widgets we are about to delete. */
    if (s_fetch_timer) {
        lv_timer_delete(s_fetch_timer);
        s_fetch_timer = nullptr;
    }
    s_fetch_bar = nullptr;
    s_fetch_status = nullptr;

    lv_obj_clean(s_detail);

    lv_obj_t *col = lv_obj_create(s_detail);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 24, 0);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    /* --- title row --- */
    lv_obj_t *head = panel(col, C_PANEL, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_size(head, LV_PCT(100), 56);

    lv_obj_t *serial = text(head, d->serial, &lv_font_montserrat_32, C_TEXT);
    lv_obj_align(serial, LV_ALIGN_LEFT_MID, 0, -8);

    char meta[96];
    lv_snprintf(meta, sizeof(meta), "%s  ·  fw %s  ·  seen %s", d->product, d->fw, d->last_seen);
    lv_obj_t *ml = text(head, meta, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(ml, LV_ALIGN_LEFT_MID, 0, 20);

    lv_obj_t *st = pill(head, status_text(d->status), status_color(d->status));
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);

    /* --- stat tiles --- */
    lv_obj_t *stats = panel(col, C_PANEL, 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_TRANSP, 0);
    lv_obj_set_size(stats, LV_PCT(100), 88);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats, 12, 0);

    char buf[32];
    lv_snprintf(buf, sizeof(buf), "%u", (unsigned)d->boot_count);
    stat_tile(stats, "BOOT COUNT", buf, C_TEXT);

    stat_tile(stats, "UPTIME", d->uptime, C_TEXT);

    if (d->battery_pct >= 0) {
        lv_snprintf(buf, sizeof(buf), "%d%%", d->battery_pct);
        stat_tile(stats, "BATTERY", buf, d->battery_pct < 50 ? C_WARN : C_TEXT);
    } else {
        stat_tile(stats, "BATTERY", "—", C_MUTED);
    }

    lv_snprintf(buf, sizeof(buf), "%d", d->rssi);
    stat_tile(stats, "RSSI dBm", buf, d->rssi < -85 ? C_WARN : C_TEXT);

    /* --- breadcrumb: what it was doing when it died --- */
    lv_obj_t *bc = panel(col, C_CARD, 10);
    lv_obj_set_size(bc, LV_PCT(100), 92);
    lv_obj_set_style_border_width(bc, 2, 0);
    lv_obj_set_style_border_color(bc, lv_color_hex(status_color(d->status)), 0);
    lv_obj_set_style_border_side(bc, LV_BORDER_SIDE_LEFT, 0);

    lv_obj_t *bcl = text(bc, "PANIC BREADCRUMB  (retained RAM)", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(bcl, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *bcv = text(bc, d->breadcrumb, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(bcv, LV_ALIGN_BOTTOM_LEFT, 16, -14);

    /* --- reset-cause ring buffer --- */
    text(col, "RESET HISTORY", &lv_font_montserrat_14, C_MUTED);

    if (d->event_count == 0) {
        lv_obj_t *none = panel(col, C_CARD, 10);
        lv_obj_set_size(none, LV_PCT(100), 64);
        lv_obj_t *nl = text(none, "No history — unit unreachable since last contact",
                            &lv_font_montserrat_20, C_MUTED);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, 16, 0);
    }

    for (uint8_t i = 0; i < d->event_count; i++) {
        const diag_event_t *ev = &d->events[i];

        lv_obj_t *row = panel(col, C_CARD, 10);
        lv_obj_set_size(row, LV_PCT(100), 66);
        lv_obj_set_style_border_width(row, 3, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(status_color(ev->severity)), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);

        lv_obj_t *cause = text(row, ev->cause, &lv_font_montserrat_20,
                               status_color(ev->severity));
        lv_obj_align(cause, LV_ALIGN_TOP_LEFT, 16, 9);

        lv_obj_t *det = text(row, ev->detail, &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(det, LV_ALIGN_BOTTOM_LEFT, 16, -9);

        lv_obj_t *ts = text(row, ev->ts, &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(ts, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* --- coredump action --- */
    if (d->has_coredump) {
        lv_obj_t *cd = panel(col, C_CARD, 10);
        lv_obj_set_size(cd, LV_PCT(100), 132);

        lv_obj_t *cdl = text(cd, "COREDUMP AVAILABLE  ·  64 KB", &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(cdl, LV_ALIGN_TOP_LEFT, 16, 12);

        lv_obj_t *btn = lv_button_create(cd);
        lv_obj_set_size(btn, 220, 60);          /* gloved-hand target */
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 16, 38);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_t *bl = text(btn, "Fetch to SD", &lv_font_montserrat_20, 0xFFFFFF);
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, fetch_cb, LV_EVENT_CLICKED, nullptr);

        s_fetch_bar = lv_bar_create(cd);
        lv_obj_set_size(s_fetch_bar, 380, 12);
        lv_obj_align(s_fetch_bar, LV_ALIGN_TOP_LEFT, 256, 62);
        lv_obj_set_style_bg_color(s_fetch_bar, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_bg_color(s_fetch_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        lv_bar_set_value(s_fetch_bar, 0, LV_ANIM_OFF);

        s_fetch_status = text(cd, "Symbolication happens on the laptop — this tool collects.",
                              &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(s_fetch_status, LV_ALIGN_TOP_LEFT, 256, 84);
    }
}

/* ------------------------------------------------------------------ */
/* Device list                                                         */
/* ------------------------------------------------------------------ */

static void highlight(uint8_t idx)
{
    for (uint8_t i = 0; i < DIAG_DEVICE_COUNT && i < 16; i++) {
        bool sel = (i == idx);
        lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(sel ? C_LINE : C_CARD), 0);
        lv_obj_set_style_border_width(s_rows[i], sel ? 4 : 0, 0);
    }
    s_selected = idx;
}

static void row_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    highlight(idx);
    build_detail(&DIAG_DEVICES[idx]);
}

static void build_list(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, LIST_W, LV_PCT(100));
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 12, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (uint8_t i = 0; i < DIAG_DEVICE_COUNT && i < 16; i++) {
        const diag_device_t *d = &DIAG_DEVICES[i];

        lv_obj_t *row = panel(list, C_CARD, 10);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(row, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_rows[i] = row;

        lv_obj_t *serial = text(row, d->serial, &lv_font_montserrat_20, C_TEXT);
        lv_obj_align(serial, LV_ALIGN_TOP_LEFT, 16, 12);

        char sub[64];
        lv_snprintf(sub, sizeof(sub), "%s  ·  %s", d->product, d->last_seen);
        lv_obj_t *sl = text(row, sub, &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 16, -12);

        /* status dot */
        lv_obj_t *dot = panel(row, status_color(d->status), 8);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -16, 16);

        if (d->has_coredump) {
            lv_obj_t *cd = text(row, "CORE", &lv_font_montserrat_14, C_WARN);
            lv_obj_align(cd, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
        }
    }
}

/* ------------------------------------------------------------------ */

void diag_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, LV_PCT(100), LV_VER_RES - HEADER_H);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_list(body);

    s_detail = lv_obj_create(body);
    lv_obj_set_size(s_detail, LV_HOR_RES - LIST_W, LV_PCT(100));
    lv_obj_align(s_detail, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);

    highlight(0);
    build_detail(&DIAG_DEVICES[0]);
}
