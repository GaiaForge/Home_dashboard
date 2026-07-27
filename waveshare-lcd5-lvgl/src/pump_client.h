/**
 * Nous A5T (Tasmota) power-strip client — mains-rated switching for pumps
 * and other AC appliances.
 *
 * Deliberately separate hardware from the garden valve controller: the A5T
 * switches MAINS voltage (110/220V AC), while irrigation solenoids are
 * low-voltage (typically 24VAC) and stay on the custom valve controller.
 * Never wire a low-voltage solenoid into this strip's mains relays.
 *
 * Talks to the strip's built-in Tasmota HTTP command API directly — same
 * "panel is a client, device is the source of truth" pattern as the valve
 * controller. Transport is isolated in this file so a later move to MQTT
 * (if more Tasmota/Zigbee2MQTT devices show up) only touches this file.
 *
 * Power reading caveat: the A5T's CSE7766 sensor measures TOTAL draw across
 * all 3 outlets + 3 USB combined, not per-outlet. Dry-run/jam detection off
 * the power reading is only meaningful if the monitored pump is the only
 * significant load plugged into the strip.
 */
#pragma once

#include <lvgl.h>

/** Build the tab's contents on an already-created tabview tab. */
void pump_tab_build(lv_obj_t *tab);

/** Pump blocking HTTP polls/commands. Call from loop() WITHOUT the LVGL lock. */
void pump_client_loop(void);

/** Load the strip's address from NVS (falls back to secrets.h's PUMP_HOST on
 *  first run). Call once at startup, before pump_tab_build(). */
void pump_client_load_host(void);

/** Set + persist the strip's address (IP or hostname, e.g. "192.168.1.42"
 *  or "tasmota-1234.local"). Takes effect on the next poll. */
void pump_client_set_host(const char *host);

/** Currently active address — for populating the settings-overlay field. */
const char *pump_client_get_host(void);
