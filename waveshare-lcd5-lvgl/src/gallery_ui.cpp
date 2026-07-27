/**
 * LVGL widget gallery — see gallery_ui.h.
 *
 * Layout: tabview across the top, each tab a wrapping grid of 316x236 cards.
 * Every card is one widget family, live and interactive. Lift a card's body
 * straight into your own code.
 */
#include "gallery_ui.h"

/* ------------------------------------------------------------------ */
#define C_BG      0x0B1220
#define C_CARD    0x161F33
#define C_INNER   0x1F2B45
#define C_TEXT    0xE6EDF7
#define C_MUTED   0x8296B4
#define C_ACCENT  0x2563EB
#define C_OK      0x22C55E
#define C_WARN    0xF59E0B
#define C_BAD     0xEF4444
#define C_PURPLE  0xA855F7
#define C_CYAN    0x06B6D4

#define CARD_W    316
#define CARD_H    236
#define TABBAR_H  64

static lv_obj_t *s_kb;          /* on-demand keyboard, overlays everything */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** A titled card. Returns the body you should put widgets into. */
static lv_obj_t *card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_MUTED), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 14, 11);

    lv_obj_t *body = lv_obj_create(c);
    lv_obj_set_size(body, CARD_W - 28, CARD_H - 46);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, -9);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    return body;
}

/** Stack a card body's children vertically, centred. */
static void stack(lv_obj_t *body, int gap)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, gap, 0);
}

static lv_obj_t *lbl(lv_obj_t *parent, const char *s, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *s, uint32_t bg, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

static void tab_setup(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_style_pad_row(tab, 16, 0);
    lv_obj_set_style_pad_column(tab, 16, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW_WRAP);
}

/* ------------------------------------------------------------------ */
/* Tab 1 — Basics                                                      */
/* ------------------------------------------------------------------ */

static void slider_lbl_cb(lv_event_t *e)
{
    lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text_fmt(l, "%d", (int)lv_slider_get_value(s));
}

static void build_basics(lv_obj_t *tab)
{
    /* Buttons ------------------------------------------------------- */
    lv_obj_t *b = card(tab, "BUTTON   normal / outline / toggle");
    stack(b, 10);
    button(b, "Primary", C_ACCENT, 210, 44);

    lv_obj_t *outline = button(b, "Outline", C_CARD, 210, 44);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outline, 2, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(C_ACCENT), 0);

    lv_obj_t *tog = button(b, "Toggle me", C_INNER, 210, 44);
    lv_obj_add_flag(tog, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(tog, lv_color_hex(C_OK), LV_STATE_CHECKED);

    /* Labels -------------------------------------------------------- */
    lv_obj_t *l = card(tab, "LABEL   recolor + auto-scroll");
    stack(l, 12);

    lv_obj_t *rec = lv_label_create(l);
    lv_label_set_recolor(rec, true);
    lv_label_set_text(rec, "#22c55e OK#   #f59e0b WARN#   #ef4444 FAIL#");
    lv_obj_set_style_text_font(rec, &lv_font_montserrat_20, 0);

    lv_obj_t *wrap = lv_label_create(l);
    lv_obj_set_width(wrap, CARD_W - 60);
    lv_label_set_long_mode(wrap, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(wrap, "Long text wraps automatically inside a fixed width.");
    lv_obj_set_style_text_font(wrap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wrap, lv_color_hex(C_MUTED), 0);

    lv_obj_t *scr = lv_label_create(l);
    lv_obj_set_width(scr, CARD_W - 60);
    lv_label_set_long_mode(scr, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(scr, "...or scrolls forever when it will not fit.   ");
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_CYAN), 0);

    /* Checkbox ------------------------------------------------------ */
    lv_obj_t *c = card(tab, "CHECKBOX");
    stack(c, 12);
    const char *opts[] = {"Log to SD card", "Upload on WiFi", "Low-power mode"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *cb = lv_checkbox_create(c);
        lv_checkbox_set_text(cb, opts[i]);
        lv_obj_set_style_text_font(cb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(C_TEXT), 0);
        if (i == 0) lv_obj_add_state(cb, LV_STATE_CHECKED);
        if (i == 2) lv_obj_add_state(cb, LV_STATE_DISABLED);
    }

    /* Switch -------------------------------------------------------- */
    lv_obj_t *s = card(tab, "SWITCH");
    stack(s, 14);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(s);
        lv_obj_set_size(row, CARD_W - 50, 40);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        static const char *names[] = {"Backlight", "Radio", "Sleep"};
        lv_obj_t *n = lbl(row, names[i], C_TEXT);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 60, 32);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(C_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (i < 2) lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    /* LED ----------------------------------------------------------- */
    lv_obj_t *ledc = card(tab, "LED   on / dimmed / off");
    lv_obj_set_flex_flow(ledc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ledc, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    uint32_t lc[] = {C_OK, C_WARN, C_BAD};
    uint8_t  br[] = {255, 120, 40};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *led = lv_led_create(ledc);
        lv_obj_set_size(led, 52, 52);
        lv_led_set_color(led, lv_color_hex(lc[i]));
        lv_led_set_brightness(led, br[i]);
    }

    /* Slider with live readout -------------------------------------- */
    lv_obj_t *sl = card(tab, "SLIDER   with live value");
    stack(sl, 18);
    lv_obj_t *val = lv_label_create(sl);
    lv_label_set_text(val, "50");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(C_CYAN), 0);

    lv_obj_t *slider = lv_slider_create(sl);
    lv_obj_set_width(slider, CARD_W - 70);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_CYAN), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_lbl_cb, LV_EVENT_VALUE_CHANGED, val);
}

/* ------------------------------------------------------------------ */
/* Tab 2 — Input                                                       */
/* ------------------------------------------------------------------ */

static void spin_inc_cb(lv_event_t *e)
{
    lv_spinbox_increment((lv_obj_t *)lv_event_get_user_data(e));
}
static void spin_dec_cb(lv_event_t *e)
{
    lv_spinbox_decrement((lv_obj_t *)lv_event_get_user_data(e));
}

static void build_input(lv_obj_t *tab)
{
    /* Arc ----------------------------------------------------------- */
    lv_obj_t *a = card(tab, "ARC   drag the ring");
    lv_obj_t *arc = lv_arc_create(a);
    lv_obj_set_size(arc, 150, 150);
    lv_obj_center(arc);
    lv_arc_set_value(arc, 65);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);

    /* Dropdown ------------------------------------------------------ */
    lv_obj_t *d = card(tab, "DROPDOWN");
    stack(d, 12);
    lv_obj_t *dd = lv_dropdown_create(d);
    lv_obj_set_width(dd, CARD_W - 70);
    lv_dropdown_set_options(dd, "800x480\n1024x600\n1280x720\n1920x1080");
    lv_dropdown_set_selected(dd, 1);

    lv_obj_t *dd2 = lv_dropdown_create(d);
    lv_obj_set_width(dd2, CARD_W - 70);
    lv_dropdown_set_options(dd2, "BLE\nWiFi\nRS485\nCAN");

    /* Roller -------------------------------------------------------- */
    lv_obj_t *r = card(tab, "ROLLER   infinite scroll");
    lv_obj_t *roller = lv_roller_create(r);
    lv_roller_set_options(roller,
                          "January\nFebruary\nMarch\nApril\nMay\nJune\n"
                          "July\nAugust\nSeptember\nOctober\nNovember\nDecember",
                          LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_width(roller, CARD_W - 90);
    lv_obj_center(roller);
    lv_obj_set_style_bg_color(roller, lv_color_hex(C_ACCENT), LV_PART_SELECTED);

    /* Spinbox ------------------------------------------------------- */
    lv_obj_t *sp = card(tab, "SPINBOX   +/- stepper");
    lv_obj_t *sb = lv_spinbox_create(sp);
    lv_spinbox_set_range(sb, 0, 9999);
    lv_spinbox_set_digit_format(sb, 4, 0);
    lv_spinbox_set_step(sb, 10);
    lv_obj_set_width(sb, 110);
    lv_obj_align(sb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(sb, &lv_font_montserrat_20, 0);

    lv_obj_t *plus = button(sp, "+", C_ACCENT, 52, 46);
    lv_obj_align(plus, LV_ALIGN_CENTER, 100, 0);
    lv_obj_add_event_cb(plus, spin_inc_cb, LV_EVENT_CLICKED, sb);

    lv_obj_t *minus = button(sp, "-", C_INNER, 52, 46);
    lv_obj_align(minus, LV_ALIGN_CENTER, -100, 0);
    lv_obj_add_event_cb(minus, spin_dec_cb, LV_EVENT_CLICKED, sb);

    /* Button matrix ------------------------------------------------- */
    lv_obj_t *bm = card(tab, "BUTTONMATRIX   keypad");
    static const char *const map[] = {"1", "2", "3", "\n",
                                      "4", "5", "6", "\n",
                                      "7", "8", "9", "\n",
                                      LV_SYMBOL_LEFT, "0", LV_SYMBOL_OK, ""};
    lv_obj_t *mx = lv_buttonmatrix_create(bm);
    lv_buttonmatrix_set_map(mx, map);
    lv_obj_set_size(mx, CARD_W - 60, CARD_H - 70);
    lv_obj_center(mx);
    lv_obj_set_style_bg_opa(mx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mx, 0, 0);
    lv_obj_set_style_bg_color(mx, lv_color_hex(C_INNER), LV_PART_ITEMS);
    lv_obj_set_style_text_color(mx, lv_color_hex(C_TEXT), LV_PART_ITEMS);

    /* Range slider -------------------------------------------------- */
    lv_obj_t *rs = card(tab, "SLIDER   range + vertical");
    lv_obj_t *range = lv_slider_create(rs);
    lv_slider_set_mode(range, LV_SLIDER_MODE_RANGE);
    lv_obj_set_width(range, CARD_W - 130);
    lv_slider_set_value(range, 80, LV_ANIM_OFF);
    lv_slider_set_left_value(range, 20, LV_ANIM_OFF);
    lv_obj_align(range, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(range, lv_color_hex(C_WARN), LV_PART_INDICATOR);

    lv_obj_t *vert = lv_slider_create(rs);
    lv_obj_set_size(vert, 14, CARD_H - 90);
    lv_slider_set_value(vert, 70, LV_ANIM_OFF);
    lv_obj_align(vert, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(vert, lv_color_hex(C_OK), LV_PART_INDICATOR);
}

/* ------------------------------------------------------------------ */
/* Tab 3 — Data                                                        */
/* ------------------------------------------------------------------ */

static void build_data(lv_obj_t *tab)
{
    /* Line chart ---------------------------------------------------- */
    lv_obj_t *c1 = card(tab, "CHART   two series, curved");
    lv_obj_t *chart = lv_chart_create(c1);
    lv_obj_set_size(chart, CARD_W - 50, CARD_H - 70);
    lv_obj_center(chart);
    lv_chart_set_type(chart, LV_CHART_TYPE_CURVE);
    lv_chart_set_point_count(chart, 12);
    lv_chart_set_div_line_count(chart, 4, 6);
    lv_obj_set_style_bg_color(chart, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(chart, 0, 0);

    lv_chart_series_t *s1 = lv_chart_add_series(chart, lv_color_hex(C_CYAN),
                                                LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *s2 = lv_chart_add_series(chart, lv_color_hex(C_PURPLE),
                                                LV_CHART_AXIS_PRIMARY_Y);
    static const int t1[] = {30, 42, 38, 55, 70, 62, 74, 88, 80, 66, 58, 71};
    static const int t2[] = {60, 55, 48, 40, 35, 44, 52, 47, 39, 50, 62, 57};
    for (int i = 0; i < 12; i++) {
        lv_chart_set_next_value(chart, s1, t1[i]);
        lv_chart_set_next_value(chart, s2, t2[i]);
    }

    /* Bar chart ----------------------------------------------------- */
    lv_obj_t *c2 = card(tab, "CHART   bar type");
    lv_obj_t *bars = lv_chart_create(c2);
    lv_obj_set_size(bars, CARD_W - 50, CARD_H - 70);
    lv_obj_center(bars);
    lv_chart_set_type(bars, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(bars, 7);
    lv_obj_set_style_bg_color(bars, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_chart_series_t *bs = lv_chart_add_series(bars, lv_color_hex(C_OK),
                                                LV_CHART_AXIS_PRIMARY_Y);
    static const int bv[] = {40, 65, 52, 88, 71, 33, 59};
    for (int i = 0; i < 7; i++) lv_chart_set_next_value(bars, bs, bv[i]);

    /* Bars ---------------------------------------------------------- */
    lv_obj_t *c3 = card(tab, "BAR   horizontal progress");
    stack(c3, 16);
    uint32_t bc[] = {C_OK, C_WARN, C_BAD};
    int bvals[] = {82, 55, 23};
    const char *bn[] = {"Battery", "Storage", "Signal"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(c3);
        lv_obj_set_size(row, CARD_W - 50, 34);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *n = lbl(row, bn[i], C_MUTED);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_size(bar, CARD_W - 50, 12);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_bar_set_value(bar, bvals[i], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_INNER), 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bc[i]), LV_PART_INDICATOR);
    }

    /* Scale + needle (the v9 replacement for the old meter) ---------- */
    lv_obj_t *c4 = card(tab, "SCALE   round gauge + needle");
    lv_obj_t *sc = lv_scale_create(c4);
    lv_obj_set_size(sc, 150, 150);
    lv_obj_center(sc);
    lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(sc, 21);
    lv_scale_set_major_tick_every(sc, 5);
    lv_scale_set_range(sc, 0, 100);
    lv_obj_set_style_line_color(sc, lv_color_hex(C_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, lv_color_hex(C_MUTED), LV_PART_INDICATOR);

    lv_obj_t *needle = lv_line_create(sc);
    lv_obj_set_style_line_width(needle, 5, 0);
    lv_obj_set_style_line_color(needle, lv_color_hex(C_BAD), 0);
    lv_obj_set_style_line_rounded(needle, true, 0);
    lv_scale_set_line_needle_value(sc, needle, 58, 72);

    /* Table --------------------------------------------------------- */
    lv_obj_t *c5 = card(tab, "TABLE");
    lv_obj_t *tbl = lv_table_create(c5);
    lv_obj_set_size(tbl, CARD_W - 40, CARD_H - 66);
    lv_obj_center(tbl);
    lv_table_set_column_count(tbl, 2);
    lv_table_set_row_count(tbl, 5);
    lv_table_set_column_width(tbl, 0, 150);
    lv_table_set_column_width(tbl, 1, 110);
    static const char *rows[5][2] = {
        {"Sensor", "Value"},
        {"Temp", "21.4 C"},
        {"Humidity", "58 %"},
        {"CO2", "612 ppm"},
        {"VPD", "1.02 kPa"},
    };
    for (int r = 0; r < 5; r++) {
        lv_table_set_cell_value(tbl, r, 0, rows[r][0]);
        lv_table_set_cell_value(tbl, r, 1, rows[r][1]);
    }
    lv_obj_set_style_bg_color(tbl, lv_color_hex(C_INNER), LV_PART_ITEMS);
    lv_obj_set_style_text_color(tbl, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_width(tbl, 0, 0);
    lv_obj_set_style_text_font(tbl, &lv_font_montserrat_14, LV_PART_ITEMS);

    /* Spinner ------------------------------------------------------- */
    lv_obj_t *c6 = card(tab, "SPINNER   indeterminate");
    lv_obj_t *spin = lv_spinner_create(c6);
    lv_obj_set_size(spin, 110, 110);
    lv_obj_center(spin);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_INNER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
}

/* ------------------------------------------------------------------ */
/* Tab 4 — Layout                                                      */
/* ------------------------------------------------------------------ */

static void build_layout(lv_obj_t *tab)
{
    /* Flex row wrap ------------------------------------------------- */
    lv_obj_t *f = card(tab, "FLEX   row wrap, auto-placed");
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(f, 8, 0);
    lv_obj_set_style_pad_column(f, 8, 0);
    uint32_t fc[] = {C_ACCENT, C_OK, C_WARN, C_BAD, C_PURPLE, C_CYAN};
    for (int i = 0; i < 9; i++) {
        lv_obj_t *box = lv_obj_create(f);
        lv_obj_set_size(box, 78, 46);
        lv_obj_set_style_bg_color(box, lv_color_hex(fc[i % 6]), 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Grid ---------------------------------------------------------- */
    lv_obj_t *g = card(tab, "GRID   3x3 with spans");
    static int32_t cols[] = {80, 80, 80, LV_GRID_TEMPLATE_LAST};
    static int32_t rows_g[] = {50, 50, 50, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(g, cols, rows_g);
    lv_obj_set_style_pad_row(g, 6, 0);
    lv_obj_set_style_pad_column(g, 6, 0);

    struct { int c, r, cs, rs; uint32_t col; } cells[] = {
        {0, 0, 2, 1, C_ACCENT}, {2, 0, 1, 2, C_PURPLE},
        {0, 1, 1, 1, C_OK},     {1, 1, 1, 1, C_CYAN},
        {0, 2, 3, 1, C_WARN},
    };
    for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        lv_obj_t *cell = lv_obj_create(g);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, cells[i].c, cells[i].cs,
                             LV_GRID_ALIGN_STRETCH, cells[i].r, cells[i].rs);
        lv_obj_set_style_bg_color(cell, lv_color_hex(cells[i].col), 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* List ---------------------------------------------------------- */
    lv_obj_t *li = card(tab, "LIST   icons + scroll");
    lv_obj_t *list = lv_list_create(li);
    lv_obj_set_size(list, CARD_W - 40, CARD_H - 66);
    lv_obj_center(list);
    lv_obj_set_style_bg_color(list, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_list_add_text(list, "Storage");
    lv_list_add_button(list, LV_SYMBOL_FILE, "New file");
    lv_list_add_button(list, LV_SYMBOL_DIRECTORY, "Open folder");
    lv_list_add_button(list, LV_SYMBOL_SAVE, "Save");
    lv_list_add_text(list, "Connectivity");
    lv_list_add_button(list, LV_SYMBOL_WIFI, "WiFi");
    lv_list_add_button(list, LV_SYMBOL_BLUETOOTH, "Bluetooth");
    lv_list_add_button(list, LV_SYMBOL_USB, "USB");

    /* Tileview ------------------------------------------------------ */
    lv_obj_t *tv = card(tab, "TILEVIEW   swipe left / right");
    lv_obj_t *tiles = lv_tileview_create(tv);
    lv_obj_set_size(tiles, CARD_W - 40, CARD_H - 66);
    lv_obj_center(tiles);
    lv_obj_set_style_bg_color(tiles, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(tiles, 0, 0);
    const char *tn[] = {"Swipe ->", "<- or ->", "<- Back"};
    uint32_t tc[] = {C_ACCENT, C_PURPLE, C_OK};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *t = lv_tileview_add_tile(tiles, i, 0, LV_DIR_HOR);
        lv_obj_t *tl = lv_label_create(t);
        lv_label_set_text(tl, tn[i]);
        lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(tc[i]), 0);
        lv_obj_center(tl);
    }

    /* Nested scroll ------------------------------------------------- */
    lv_obj_t *sc = card(tab, "SCROLL   momentum + snap");
    lv_obj_t *scroll = lv_obj_create(sc);
    lv_obj_set_size(scroll, CARD_W - 40, CARD_H - 66);
    lv_obj_center(scroll);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_snap_y(scroll, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_style_pad_row(scroll, 8, 0);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *item = lv_obj_create(scroll);
        lv_obj_set_size(item, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(item, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *il = lv_label_create(item);
        lv_label_set_text_fmt(il, "Item %d", i + 1);
        lv_obj_set_style_text_color(il, lv_color_hex(C_TEXT), 0);
        lv_obj_center(il);
    }

    /* Window -------------------------------------------------------- */
    lv_obj_t *wc = card(tab, "WIN   title bar + content");
    lv_obj_t *win = lv_win_create(wc);
    lv_obj_set_size(win, CARD_W - 40, CARD_H - 66);
    lv_obj_center(win);
    lv_win_add_title(win, "Settings");
    lv_win_add_button(win, LV_SYMBOL_CLOSE, 40);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_t *wcontent = lv_win_get_content(win);
    lv_obj_t *wl = lv_label_create(wcontent);
    lv_label_set_text(wl, "Window content area");
    lv_obj_set_style_text_color(wl, lv_color_hex(C_MUTED), 0);
}

/* ------------------------------------------------------------------ */
/* Tab 5 — Text                                                        */
/* ------------------------------------------------------------------ */

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void kb_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, NULL);
}

static void msgbox_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, "Erase recordings?");
    lv_msgbox_add_text(mb, "This deletes all clips on the SD card. "
                           "There is no undo.");
    lv_msgbox_add_close_button(mb);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "Erase");
    lv_obj_set_style_bg_color(ok, lv_color_hex(C_BAD), 0);
    lv_msgbox_add_footer_button(mb, "Cancel");
}

static void build_text(lv_obj_t *tab)
{
    /* Text area + keyboard ------------------------------------------ */
    lv_obj_t *t = card(tab, "TEXTAREA   tap to open keyboard");
    stack(t, 12);
    lv_obj_t *ta = lv_textarea_create(t);
    lv_obj_set_size(ta, CARD_W - 60, 70);
    lv_textarea_set_placeholder_text(ta, "Tap here to type...");
    lv_textarea_set_one_line(ta, false);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pw = lv_textarea_create(t);
    lv_obj_set_size(pw, CARD_W - 60, 46);
    lv_textarea_set_one_line(pw, true);
    lv_textarea_set_password_mode(pw, true);
    lv_textarea_set_text(pw, "hunter2");
    lv_obj_add_event_cb(pw, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    /* Span (rich text) ---------------------------------------------- */
    lv_obj_t *sp = card(tab, "SPAN   mixed styles in one flow");
    lv_obj_t *sg = lv_spangroup_create(sp);
    lv_obj_set_size(sg, CARD_W - 50, CARD_H - 70);
    lv_obj_center(sg);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);

    lv_span_t *sp1 = lv_spangroup_new_span(sg);
    lv_span_set_text(sp1, "Unit ");
    lv_style_set_text_color(lv_span_get_style(sp1), lv_color_hex(C_MUTED));

    lv_span_t *sp2 = lv_spangroup_new_span(sg);
    lv_span_set_text(sp2, "ORF-BAS-003 ");
    lv_style_set_text_color(lv_span_get_style(sp2), lv_color_hex(C_TEXT));
    lv_style_set_text_font(lv_span_get_style(sp2), &lv_font_montserrat_20);

    lv_span_t *sp3 = lv_spangroup_new_span(sg);
    lv_span_set_text(sp3, "PANIC");
    lv_style_set_text_color(lv_span_get_style(sp3), lv_color_hex(C_BAD));
    lv_style_set_text_font(lv_span_get_style(sp3), &lv_font_montserrat_20);

    lv_span_t *sp4 = lv_spangroup_new_span(sg);
    lv_span_set_text(sp4, "  — one paragraph, several styles, wraps as normal text.");
    lv_style_set_text_color(lv_span_get_style(sp4), lv_color_hex(C_MUTED));

    /* Arc label ----------------------------------------------------- */
    lv_obj_t *al = card(tab, "ARCLABEL   text on a curve");
    lv_obj_t *arcl = lv_arclabel_create(al);
    lv_obj_set_size(arcl, 180, 160);
    lv_obj_center(arcl);
    lv_arclabel_set_text(arcl, "GAIAFORGE  ·  FIELD  ·  ");
    lv_arclabel_set_radius(arcl, 74);
    lv_arclabel_set_angle_start(arcl, 180);
    lv_arclabel_set_angle_size(arcl, 180);
    lv_obj_set_style_text_color(arcl, lv_color_hex(C_CYAN), 0);
    lv_obj_set_style_text_font(arcl, &lv_font_montserrat_14, 0);

    /* Calendar ------------------------------------------------------ */
    lv_obj_t *cc = card(tab, "CALENDAR");
    lv_obj_t *cal = lv_calendar_create(cc);
    lv_obj_set_size(cal, CARD_W - 46, CARD_H - 62);
    lv_obj_center(cal);
    lv_calendar_set_today_date(cal, 2026, 7, 25);
    lv_calendar_set_month_shown(cal, 2026, 7);
    lv_obj_set_style_bg_color(cal, lv_color_hex(C_INNER), 0);
    lv_obj_set_style_border_width(cal, 0, 0);
    lv_obj_set_style_text_font(cal, &lv_font_montserrat_14, 0);

    /* Message box --------------------------------------------------- */
    lv_obj_t *mb = card(tab, "MSGBOX   modal dialog");
    stack(mb, 12);
    lbl(mb, "Modal, dims the background,", C_MUTED);
    lbl(mb, "and blocks input behind it.", C_MUTED);
    lv_obj_t *open = button(mb, "Open dialog", C_BAD, 190, 48);
    lv_obj_add_event_cb(open, msgbox_cb, LV_EVENT_CLICKED, NULL);

    /* Fonts --------------------------------------------------------- */
    lv_obj_t *fc = card(tab, "FONTS   sizes compiled in");
    stack(fc, 6);
    struct { const lv_font_t *f; const char *n; } fonts[] = {
        {&lv_font_montserrat_14, "14  small print"},
        {&lv_font_montserrat_20, "20  body"},
        {&lv_font_montserrat_24, "24  heading"},
        {&lv_font_montserrat_32, "32  display"},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *fl = lv_label_create(fc);
        lv_label_set_text(fl, fonts[i].n);
        lv_obj_set_style_text_font(fl, fonts[i].f, 0);
        lv_obj_set_style_text_color(fl, lv_color_hex(C_TEXT), 0);
    }
}

/* ------------------------------------------------------------------ */
/* Tab 6 — Style & animation                                           */
/* ------------------------------------------------------------------ */

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void anim_zoom_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)var, v, 0);
}

static void build_style(lv_obj_t *tab)
{
    /* Gradients ----------------------------------------------------- */
    lv_obj_t *g = card(tab, "GRADIENT   vertical / horizontal");
    stack(g, 12);
    struct { uint32_t a, b; lv_grad_dir_t d; } grads[] = {
        {C_ACCENT, C_PURPLE, LV_GRAD_DIR_HOR},
        {C_OK,     C_CYAN,   LV_GRAD_DIR_VER},
        {C_WARN,   C_BAD,    LV_GRAD_DIR_HOR},
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(g);
        lv_obj_set_size(box, CARD_W - 60, 44);
        lv_obj_set_style_bg_color(box, lv_color_hex(grads[i].a), 0);
        lv_obj_set_style_bg_grad_color(box, lv_color_hex(grads[i].b), 0);
        lv_obj_set_style_bg_grad_dir(box, grads[i].d, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Shadows and radius -------------------------------------------- */
    lv_obj_t *s = card(tab, "SHADOW + RADIUS");
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int radii[] = {0, 12, 40};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(s);
        lv_obj_set_size(box, 76, 76);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, radii[i], 0);
        lv_obj_set_style_shadow_width(box, 22, 0);
        lv_obj_set_style_shadow_color(box, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_shadow_offset_y(box, 8, 0);
        lv_obj_set_style_shadow_opa(box, LV_OPA_50, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Opacity ------------------------------------------------------- */
    lv_obj_t *o = card(tab, "OPACITY   100 / 70 / 40 / 15 %");
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_opa_t opas[] = {LV_OPA_COVER, LV_OPA_70, LV_OPA_40, LV_OPA_20};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(o);
        lv_obj_set_size(box, 56, 92);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_PURPLE), 0);
        lv_obj_set_style_bg_opa(box, opas[i], 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Animation: translate ------------------------------------------ */
    lv_obj_t *a = card(tab, "ANIMATION   ping-pong translate");
    lv_obj_t *ball = lv_obj_create(a);
    lv_obj_set_size(ball, 44, 44);
    lv_obj_set_style_bg_color(ball, lv_color_hex(C_CYAN), 0);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ball, 0, 0);
    lv_obj_align(ball, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, ball);
    lv_anim_set_exec_cb(&an, anim_x_cb);
    lv_anim_set_values(&an, 0, CARD_W - 90);
    lv_anim_set_duration(&an, 1400);
    lv_anim_set_playback_duration(&an, 1400);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);

    /* Animation: scale ---------------------------------------------- */
    lv_obj_t *z = card(tab, "ANIMATION   scale transform");
    lv_obj_t *sq = lv_obj_create(z);
    lv_obj_set_size(sq, 80, 80);
    lv_obj_set_style_bg_color(sq, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_hex(C_BAD), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(sq, 14, 0);
    lv_obj_set_style_border_width(sq, 0, 0);
    lv_obj_center(sq);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t az;
    lv_anim_init(&az);
    lv_anim_set_var(&az, sq);
    lv_anim_set_exec_cb(&az, anim_zoom_cb);
    lv_anim_set_values(&az, 180, 330);
    lv_anim_set_duration(&az, 1000);
    lv_anim_set_playback_duration(&az, 1000);
    lv_anim_set_repeat_count(&az, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&az);

    /* Pressed-state feedback ---------------------------------------- */
    lv_obj_t *p = card(tab, "STATES   press these");
    stack(p, 10);
    lv_obj_t *p1 = button(p, "Press: colour", C_ACCENT, 200, 44);
    lv_obj_set_style_bg_color(p1, lv_color_hex(C_OK), LV_STATE_PRESSED);

    lv_obj_t *p2 = button(p, "Press: shrink", C_PURPLE, 200, 44);
    lv_obj_set_style_transform_scale(p2, 240, LV_STATE_PRESSED);

    lv_obj_t *p3 = button(p, "Disabled", C_INNER, 200, 44);
    lv_obj_add_state(p3, LV_STATE_DISABLED);
}

/* ------------------------------------------------------------------ */

void gallery_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, TABBAR_H);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x131C2E), 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_20, 0);

    build_basics(lv_tabview_add_tab(tv, "Basics"));
    build_input (lv_tabview_add_tab(tv, "Input"));
    build_data  (lv_tabview_add_tab(tv, "Data"));
    build_layout(lv_tabview_add_tab(tv, "Layout"));
    build_text  (lv_tabview_add_tab(tv, "Text"));
    build_style (lv_tabview_add_tab(tv, "Style"));

    /* Keyboard lives on the screen so it can overlay any tab. */
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, LV_PCT(100), 240);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_close_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_close_cb, LV_EVENT_CANCEL, NULL);
}
