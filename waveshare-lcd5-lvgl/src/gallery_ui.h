/**
 * LVGL widget gallery — a tabbed tour of what this board can draw.
 *
 * Six tabs, each a grid of cards, every widget live and interactive. Meant for
 * poking at, not for shipping: find something you like, then lift the code for
 * that card out of gallery_ui.cpp.
 *
 * Everything here uses only widgets already enabled in include/lv_conf.h, so
 * it compiles without touching the config.
 */
#pragma once

#include <lvgl.h>

/** Build the gallery on the active screen. Call with the LVGL lock held. */
void gallery_ui_create(void);
