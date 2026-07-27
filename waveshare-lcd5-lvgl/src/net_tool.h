/**
 * 2.4 GHz Recon — a passive WiFi + BLE scanner for this board.
 *
 * Scope: reconnaissance and diagnostics only. It scans, lists, and shows
 * detail on what is broadcasting nearby. No deauth, no rogue AP, no traffic
 * injection — those are DoS / phishing and deliberately absent.
 *
 * Threading model (important): WiFi.scanNetworks() and BLEScan::start() both
 * BLOCK for seconds. Calling them from an LVGL event callback would freeze the
 * UI task. So button callbacks only set a request flag; the actual scan runs
 * in net_tool_loop() (called from Arduino loop(), a separate task) and writes
 * results back into the widgets under the LVGL lock.
 */
#pragma once

/** Build the UI on the active screen. Call with the LVGL lock held. */
void net_tool_create(void);

/** Pump scan requests. Call from Arduino loop() WITHOUT holding the lock. */
void net_tool_loop(void);
