/**
 * MicroClimate quick-dashboard.
 *
 * A read-only wall panel for the MicroClimate Monitor system:
 *   - Indoor tab  : the local BME280 (bench reference)
 *   - Field Hubs  : live nodes fetched from GET /api/nodes on the server
 *
 * WiFi connect + NTP + the blocking HTTP fetch all run from mc_dash_loop()
 * (Arduino loop() task); the sensor read + clock run from an LVGL timer.
 * The board only ever issues GETs — it never writes to the server.
 */
#pragma once

/** Build the UI. Call with the LVGL lock held. */
void mc_dash_create(void);

/** Pump WiFi + periodic hub fetch. Call from loop() WITHOUT the lock held. */
void mc_dash_loop(void);
