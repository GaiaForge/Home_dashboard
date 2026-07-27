/**
 * Live telemetry dashboard — the "what can this thing actually do" screen.
 *
 * Unlike the widget gallery, nothing here is static: charts stream, gauges
 * ease between targets, an equaliser reacts every frame, and a real FPS
 * counter (driven by LV_EVENT_RENDER_READY, not a guess) sits in the header.
 *
 * Uses radial gradients, which need LV_USE_DRAW_SW_COMPLEX_GRADIENTS = 1.
 */
#pragma once

#include <lvgl.h>

/** Build the dashboard on the active screen. Call with the LVGL lock held. */
void showcase_ui_create(void);
