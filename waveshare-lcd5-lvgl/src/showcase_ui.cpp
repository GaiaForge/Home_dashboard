/**
 * Live telemetry dashboard — see showcase_ui.h.
 *
 * 1024x600: header 64, then three columns (300 / 400 / 260) of live panels.
 * Two timers drive everything — a 50 ms "stream" tick and a 1 s FPS tick —
 * plus LVGL animations for anything that should ease rather than jump.
 */
#include "showcase_ui.h"
#include <Arduino.h>

/* ------------------------------------------------------------------ */
#define C_BG      0x070C16
#define C_CARD    0x121B2E
#define C_INNER   0x1C2740
#define C_TEXT    0xE8EFFA
#define C_MUTED   0x7E93B2
#define C_ACCENT  0x3B82F6
#define C_OK      0x22C55E
#define C_WARN    0xF59E0B
#define C_BAD     0xEF4444
#define C_PURPLE  0xA855F7
#define C_CYAN    0x22D3EE

#define HEADER_H  64
#define PAD       16

#define CHART_POINTS 48
#define EQ_BARS      14

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *value;
    int       cur;
} gauge_t;

static gauge_t   s_gauge[2];
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_a;
static lv_chart_series_t *s_ser_b;
static lv_obj_t *s_eq[EQ_BARS];
static lv_obj_t *s_hbar[4];
static lv_obj_t *s_hval[4];
static lv_obj_t *s_led[4];
static lv_obj_t *s_fps;
static lv_obj_t *s_clock;

static uint32_t  s_frames;          /* incremented per rendered frame */

/* Load level, cycled automatically to measure what each panel costs:
 *   2 = chart + equaliser (everything)
 *   1 = chart only
 *   0 = neither; just the gauges and subsystem bars easing
 * Demand is 20 Hz (stream_tick period), so fps == 20 means we are keeping up
 * and anything lower means one frame takes longer than 50 ms. */
static int       s_load = 2;

static uint32_t  s_seconds = 14 * 3600 + 32 * 60;
static uint32_t  s_phase;           /* drives the synthetic waveforms  */

/* Small deterministic PRNG — Math.random() equivalents aren't available and
 * we want the same wiggle every boot anyway. */
static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 16;
}
static int rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* Cheap integer sine, 0..255 phase -> -100..100, no float needed. */
static int isin(uint32_t phase)
{
    int p = (int)(phase & 0xFF);
    int sign = 1;
    if (p >= 128) { p -= 128; sign = -1; }
    if (p > 64) p = 128 - p;
    return sign * (p * 100 / 64);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *panel(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *cap(lv_obj_t *parent, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, 12);
    return l;
}

/* Bisect switch: 1 = radial-gradient glow, 0 = flat disc.
 * Radial gradients are the only new renderer feature in this build, so this
 * is the variable under test for the first-frame hang. */
#define USE_RADIAL_GLOW 0

/** Soft radial glow behind a widget — needs COMPLEX_GRADIENTS. */
static void glow(lv_obj_t *parent, int size, uint32_t color, lv_opa_t opa)
{
#if !USE_RADIAL_GLOW
    LV_UNUSED(parent); LV_UNUSED(size); LV_UNUSED(color); LV_UNUSED(opa);
    return;
#else
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, size, size);
    lv_obj_center(g);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(g, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_grad_dir(g, LV_GRAD_DIR_RADIAL, 0);
    lv_obj_set_style_bg_opa(g, opa, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(g);
#endif
}

/* ------------------------------------------------------------------ */
/* Gauges                                                              */
/* ------------------------------------------------------------------ */

static void gauge_anim_cb(void *var, int32_t v)
{
    gauge_t *g = (gauge_t *)var;
    g->cur = (int)v;
    lv_arc_set_value(g->arc, (int)v);
    lv_label_set_text_fmt(g->value, "%d", (int)v);
}

static void gauge_retarget(gauge_t *g, int target, uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, gauge_anim_cb);
    lv_anim_set_values(&a, g->cur, target);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void build_gauge(lv_obj_t *parent, int idx, const char *title,
                        const char *unit, uint32_t color, int start, int y)
{
    lv_obj_t *p = panel(parent, 300, 260);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, PAD, y);
    cap(p, title);
    glow(p, 210, color, LV_OPA_30);

    lv_obj_t *arc = lv_arc_create(p);
    lv_obj_set_size(arc, 190, 190);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 14);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_value(arc, start);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_t *val = lv_label_create(p);
    lv_label_set_text_fmt(val, "%d", start);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(C_TEXT), 0);
    lv_obj_align(val, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *u = lv_label_create(p);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), 0);
    lv_obj_align(u, LV_ALIGN_CENTER, 0, 46);

    s_gauge[idx].arc = arc;
    s_gauge[idx].value = val;
    s_gauge[idx].cur = start;
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

static void render_ready_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_frames++;
}

static void fps_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* Also to serial: tells us whether LVGL timers run and frames render,
     * which a photo of the panel cannot. */
    /* LVGL keeps its own heap; ESP.getFreeHeap() cannot see growth there
     * (e.g. animations or draw layers piling up), so report both. */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("load=%d  fps=%u  esp_heap=%u  lv_used=%u%%  lv_free=%u\n",
                  s_load, (unsigned)s_frames, (unsigned)ESP.getFreeHeap(),
                  (unsigned)mon.used_pct, (unsigned)mon.free_size);
    lv_label_set_text_fmt(s_fps, "%u FPS", (unsigned)s_frames);
    lv_obj_set_style_text_color(s_fps,
                                lv_color_hex(s_frames >= 25 ? C_OK :
                                             s_frames >= 15 ? C_WARN : C_BAD), 0);
    s_frames = 0;

    s_seconds = (s_seconds + 1) % 86400;
    lv_label_set_text_fmt(s_clock, "%02u:%02u:%02u",
                          (unsigned)(s_seconds / 3600),
                          (unsigned)((s_seconds / 60) % 60),
                          (unsigned)(s_seconds % 60));
}

/** 50 ms: push chart points, jiggle the equaliser. */
static void stream_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_phase += 5;

    if (s_load >= 1) {
        lv_chart_set_next_value(s_chart, s_ser_a, 50 + isin(s_phase) * 32 / 100
                                                  + rnd_range(-4, 4));
        lv_chart_set_next_value(s_chart, s_ser_b, 48 + isin(s_phase / 2 + 40) * 24 / 100
                                                  + rnd_range(-3, 3));
    }

    if (s_load < 2) {
        return;
    }

    for (int i = 0; i < EQ_BARS; i++) {
        int v = 30 + isin(s_phase * 2 + (uint32_t)i * 18) * 45 / 100 + rnd_range(-12, 12);
        if (v < 2)   v = 2;
        if (v > 100) v = 100;
        lv_bar_set_value(s_eq[i], v, LV_ANIM_ON);
    }
}

/** 2.5 s: send the gauges and bars somewhere new, smoothly. */
static void retarget_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    gauge_retarget(&s_gauge[0], rnd_range(35, 95), 2000);
    gauge_retarget(&s_gauge[1], rnd_range(20, 80), 2400);

    for (int i = 0; i < 4; i++) {
        int v = rnd_range(15, 98);
        lv_bar_set_value(s_hbar[i], v, LV_ANIM_ON);
        lv_label_set_text_fmt(s_hval[i], "%d%%", v);
    }

    /* Blink one status LED so the panel never looks frozen. */
    int k = rnd_range(0, 3);
    lv_led_toggle(s_led[k]);
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static void build_header(lv_obj_t *scr)
{
    lv_obj_t *h = lv_obj_create(scr);
    lv_obj_set_size(h, LV_PCT(100), HEADER_H);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(h);
    lv_label_set_text(t, "LIVE TELEMETRY");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *d = lv_label_create(h);
    lv_label_set_text(d, "ORF-BAS-003   ·   streaming");
    lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(C_MUTED), 0);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 230, 0);

    s_clock = lv_label_create(h);
    lv_label_set_text(s_clock, "14:32:00");
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(C_TEXT), 0);
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, -140, 0);

    s_fps = lv_label_create(h);
    lv_label_set_text(s_fps, "-- FPS");
    lv_obj_set_style_text_font(s_fps, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_fps, lv_color_hex(C_OK), 0);
    lv_obj_align(s_fps, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void build_centre(lv_obj_t *scr)
{
    /* Streaming chart ------------------------------------------------ */
    lv_obj_t *p = panel(scr, 400, 300);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, PAD * 2 + 300, HEADER_H + PAD);
    cap(p, "ACOUSTIC LEVEL   ·   live, 20 Hz");

    s_chart = lv_chart_create(p);
    lv_obj_set_size(s_chart, 368, 228);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 10, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x2A3550), 0);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);   /* hide points */
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);

    s_ser_a = lv_chart_add_series(s_chart, lv_color_hex(C_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_b = lv_chart_add_series(s_chart, lv_color_hex(C_PURPLE), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < CHART_POINTS; i++) {
        lv_chart_set_next_value(s_chart, s_ser_a, 50);
        lv_chart_set_next_value(s_chart, s_ser_b, 48);
    }

    /* Horizontal bars ------------------------------------------------ */
    lv_obj_t *b = panel(scr, 400, 220);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, PAD * 2 + 300, HEADER_H + PAD * 2 + 300);
    cap(b, "SUBSYSTEMS");

    static const char *names[] = {"Storage", "Battery", "Link", "Buffer"};
    uint32_t cols[] = {C_ACCENT, C_OK, C_CYAN, C_PURPLE};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *n = lv_label_create(b);
        lv_label_set_text(n, names[i]);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(C_TEXT), 0);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 16, 46 + i * 40);

        s_hval[i] = lv_label_create(b);
        lv_label_set_text(s_hval[i], "--");
        lv_obj_set_style_text_font(s_hval[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_hval[i], lv_color_hex(C_MUTED), 0);
        lv_obj_align(s_hval[i], LV_ALIGN_TOP_RIGHT, -16, 46 + i * 40);

        s_hbar[i] = lv_bar_create(b);
        lv_obj_set_size(s_hbar[i], 368, 10);
        lv_obj_align(s_hbar[i], LV_ALIGN_TOP_LEFT, 16, 66 + i * 40);
        lv_bar_set_value(s_hbar[i], 50, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_hbar[i], lv_color_hex(C_INNER), 0);
        lv_obj_set_style_bg_color(s_hbar[i], lv_color_hex(cols[i]), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_hbar[i], 5, LV_PART_INDICATOR);
    }
}

static void build_right(lv_obj_t *scr)
{
    const int x = PAD * 3 + 300 + 400;

    /* Equaliser ------------------------------------------------------ */
    lv_obj_t *e = panel(scr, 260, 300);
    lv_obj_align(e, LV_ALIGN_TOP_LEFT, x, HEADER_H + PAD);
    cap(e, "SPECTRUM");

    uint32_t eq_cols[] = {C_CYAN, C_ACCENT, C_PURPLE};
    for (int i = 0; i < EQ_BARS; i++) {
        s_eq[i] = lv_bar_create(e);
        lv_obj_set_size(s_eq[i], 12, 210);
        lv_obj_align(s_eq[i], LV_ALIGN_BOTTOM_LEFT, 14 + i * 17, -18);
        lv_bar_set_value(s_eq[i], 30, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_eq[i], lv_color_hex(C_INNER), 0);
        lv_obj_set_style_bg_color(s_eq[i], lv_color_hex(eq_cols[i % 3]),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_eq[i], 6, 0);
        lv_obj_set_style_radius(s_eq[i], 6, LV_PART_INDICATOR);
    }

    /* Status LEDs ---------------------------------------------------- */
    lv_obj_t *s = panel(scr, 260, 220);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, x, HEADER_H + PAD * 2 + 300);
    cap(s, "STATUS");

    static const char *ln[] = {"Mic", "SD", "Radio", "Power"};
    uint32_t lc[] = {C_OK, C_CYAN, C_WARN, C_OK};
    for (int i = 0; i < 4; i++) {
        s_led[i] = lv_led_create(s);
        lv_obj_set_size(s_led[i], 26, 26);
        lv_obj_align(s_led[i], LV_ALIGN_TOP_LEFT, 22, 50 + i * 40);
        lv_led_set_color(s_led[i], lv_color_hex(lc[i]));
        lv_led_set_brightness(s_led[i], 255);

        lv_obj_t *l = lv_label_create(s);
        lv_label_set_text(l, ln[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 62, 55 + i * 40);
    }
}

void showcase_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    build_header(scr);

    build_gauge(scr, 0, "SIGNAL  dB", "of 100", C_CYAN, 62, HEADER_H + PAD);
    build_gauge(scr, 1, "CPU LOAD  %", "8 MB PSRAM", C_PURPLE, 38,
                HEADER_H + PAD * 2 + 260);

    build_centre(scr);
    build_right(scr);

    /* Real measured FPS, not a guess. */
    lv_display_add_event_cb(lv_display_get_default(), render_ready_cb,
                            LV_EVENT_RENDER_READY, NULL);

    /* 100 ms, not 50. MEASURED: this UI renders ~9 fps, so demanding 20 Hz
     * just queues work it cannot finish and the frames arrive unevenly.
     * Matching demand to measured capability looks smoother at no cost. */
    lv_timer_create(stream_tick, 100, NULL);
    lv_timer_create(retarget_tick, 2500, NULL);
    lv_timer_create(fps_tick, 1000, NULL);

    retarget_tick(NULL);
}
